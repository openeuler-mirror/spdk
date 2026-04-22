/* -
 * GNU GPL LICENSE
 * Copyright (c) Huawei Technologies Co., Ltd. 2021-2022. All rights reserved.
 * This file contains code segment derived form libfuse
 * Original copyright notice:
 *   Copyright (C) 2001-2007 Miklos Szeredi <miklos@szeredi.hu> 
 *   This program can be distributed under the terms of the GNU GPL.
 */

#include "spdk/env.h"
#include "spdk/string.h"
#include "spdk/thread.h"
#include "spdk/ssam.h"
#include "spdk/likely.h"

#include "ssam_config.h"
#include "ssam_fs_internal.h"

static void ssam_fs_dump_info_json(struct spdk_ssam_session *smsession,
				   struct spdk_json_write_ctx *w);
static void ssam_fs_response_worker(struct spdk_ssam_session *smsession, void *arg);
static void ssam_fs_write_config_json(struct spdk_ssam_session *smsession,
				      struct spdk_json_write_ctx *w);
static void ssam_fs_remove_self(struct spdk_ssam_session *smsession);
static void ssam_fs_show_iostat_json(struct spdk_ssam_session *smsession, uint32_t id,
				     struct spdk_json_write_ctx *w);
static void ssam_fs_clear_iostat_json(struct spdk_ssam_session *smsession);

static void ssam_free_fs_session(struct spdk_ssam_fs_session *fsmsession);

static const struct spdk_ssam_session_backend g_ssam_fs_session_backend = {
	.type = VIRTIO_TYPE_FS,
	.remove_session = NULL,
	.request_worker = NULL,
	.destroy_bdev_device = NULL,
	.response_worker = ssam_fs_response_worker,
	.no_data_req_worker = NULL,
	.ssam_get_config = NULL,
	.print_stuck_io_info = NULL,
	.dump_info_json = ssam_fs_dump_info_json,
	.write_config_json = ssam_fs_write_config_json,
	.show_iostat_json = ssam_fs_show_iostat_json,
	.clear_iostat_json = ssam_fs_clear_iostat_json,
	.get_bdev = NULL,
	.remove_self = ssam_fs_remove_self,
};

static struct spdk_ssam_fs_poller_ctx g_ssam_fs_poller_ctx = { 0 };

static struct lo_data lo_map[SSAM_HOSTEP_NUM_MAX] = { 0 };

static struct lo_data *lo_data(fuse_req_t req)
{
	return (struct lo_data *)fuse_req_userdata(req);
}

static struct lo_inode *lo_inode(fuse_req_t req, fuse_ino_t ino)
{
	if (ino == FUSE_ROOT_ID) {
		return &lo_data(req)->root;
	}

	return (struct lo_inode *)(uintptr_t)ino;
}

static int lo_fd(fuse_req_t req, fuse_ino_t ino)
{
	return lo_inode(req, ino)->fd;
}

static void lo_init(void *userdata, struct fuse_conn_info *conn)
{
	struct lo_data *lo = (struct lo_data *)userdata;
	struct fuse_session *se = lo->se;

	if (conn->capable & FUSE_CAP_EXPORT_SUPPORT) {
		conn->want |= FUSE_CAP_EXPORT_SUPPORT;
	}

	if (lo->writeback && conn->capable & FUSE_CAP_WRITEBACK_CACHE) {
		SPDK_INFOLOG(ssam_fs, "lo_init: activating writeback\n");
		conn->want |= FUSE_CAP_WRITEBACK_CACHE;
	}
	if (lo->flock && conn->capable & FUSE_CAP_FLOCK_LOCKS) {
		SPDK_INFOLOG(ssam_fs, "lo_init: activating flock locks\n");
		conn->want |= FUSE_CAP_FLOCK_LOCKS;
	}
	conn->want &= ~FUSE_CAP_SPLICE_READ;
	conn->max_write = SSAM_FS_MAX_PAGES * getpagesize();
	se->got_destroy = 0;
	lo->mounted = true;
}

static void lo_destroy(void *userdata)
{
	struct lo_data *lo = (struct lo_data *)userdata;
	struct fuse_session *se = lo->se;

	while (lo->root.next != &lo->root) {
		struct lo_inode *next = lo->root.next;
		lo->root.next = next->next;
		if (next->fd > 0) {
			close(next->fd);
		}
		free(next);
	}
	se->got_init = 0;
	lo->mounted = false;
}

static void lo_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	int res;
	struct stat buf;
	struct lo_data *lo = lo_data(req);
	unsigned lcore_id = rte_lcore_id();
	struct spdk_ssam_fs_session *fsmsession = (struct spdk_ssam_fs_session *)lo->smsession[lcore_id];

	(void)fi;

	res = fstatat(lo_fd(req, ino), "", &buf, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW);
	if (res == -1) {
		return (void)fuse_reply_err(req, errno);
	}

	ssam_fuse_reply_attr(req, &buf, lo->timeout, fsmsession);
}

static void lo_setattr(fuse_req_t req, fuse_ino_t ino, struct stat *attr, int valid,
		       struct fuse_file_info *fi)
{
	int saverr;
	char procname[64];
	struct lo_inode *inode = lo_inode(req, ino);
	int ifd = inode->fd;
	int res;

	if (valid & FUSE_SET_ATTR_MODE) {
		if (fi) {
			res = fchmod(fi->fh, attr->st_mode);
		} else {
			snprintf(procname, sizeof(procname), "/proc/self/fd/%i", ifd);
			res = chmod(procname, attr->st_mode);
		}
		if (res == -1) {
			goto out_err;
		}
	}
	if (valid & (FUSE_SET_ATTR_UID | FUSE_SET_ATTR_GID)) {
		uid_t uid = (valid & FUSE_SET_ATTR_UID) ? attr->st_uid : (uid_t) -1;
		gid_t gid = (valid & FUSE_SET_ATTR_GID) ? attr->st_gid : (gid_t) -1;

		res = fchownat(ifd, "", uid, gid, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW);
		if (res == -1) {
			goto out_err;
		}
	}
	if (valid & FUSE_SET_ATTR_SIZE) {
		if (fi) {
			res = ftruncate(fi->fh, attr->st_size);
		} else {
			snprintf(procname, sizeof(procname), "/proc/self/fd/%i", ifd);
			res = truncate(procname, attr->st_size);
		}
		if (res == -1) {
			goto out_err;
		}
	}
	if (valid & (FUSE_SET_ATTR_ATIME | FUSE_SET_ATTR_MTIME)) {
		struct timespec tv[2];

		tv[0].tv_sec = 0;
		tv[1].tv_sec = 0;
		tv[0].tv_nsec = UTIME_OMIT;
		tv[1].tv_nsec = UTIME_OMIT;

		if (valid & FUSE_SET_ATTR_ATIME_NOW) {
			tv[0].tv_nsec = UTIME_NOW;
		} else if (valid & FUSE_SET_ATTR_ATIME) {
			tv[0] = attr->st_atim;
		}

		if (valid & FUSE_SET_ATTR_MTIME_NOW) {
			tv[1].tv_nsec = UTIME_NOW;
		} else if (valid & FUSE_SET_ATTR_MTIME) {
			tv[1] = attr->st_mtim;
		}

		if (fi) {
			res = futimens(fi->fh, tv);
		} else {
			snprintf(procname, sizeof(procname), "/proc/self/fd/%i", ifd);
			res = utimensat(AT_FDCWD, procname, tv, 0);
		}
		if (res == -1) {
			goto out_err;
		}
	}

	return lo_getattr(req, ino, fi);

out_err:
	saverr = errno;
	fuse_reply_err(req, saverr);
}

static struct lo_inode *lo_find(struct lo_data *lo, struct stat *st)
{
	struct lo_inode *p;
	struct lo_inode *ret = NULL;

	pthread_mutex_lock(&lo->mutex);
	for (p = lo->root.next; p != &lo->root; p = p->next) {
		if (p->ino == st->st_ino && p->dev == st->st_dev) {
			assert(p->refcount > 0);
			ret = p;
			ret->refcount++;
			break;
		}
	}
	pthread_mutex_unlock(&lo->mutex);
	return ret;
}

static int lo_do_lookup(fuse_req_t req, fuse_ino_t parent, const char *name,
			struct fuse_entry_param *e)
{
	int newfd;
	int res;
	int saverr;
	struct lo_data *lo = lo_data(req);
	struct lo_inode *inode;

	memset(e, 0, sizeof(*e));
	e->attr_timeout = lo->timeout;
	e->entry_timeout = lo->timeout;

	newfd = openat(lo_fd(req, parent), name, O_PATH | O_NOFOLLOW);
	if (newfd == -1) {
		goto out_err;
	}

	res = fstatat(newfd, "", &e->attr, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW);
	if (res == -1) {
		goto out_err;
	}

	inode = lo_find(lo_data(req), &e->attr);
	if (inode) {
		close(newfd);
		newfd = -1;
	} else {
		struct lo_inode *prev, *next;

		saverr = ENOMEM;
		inode = calloc(1, sizeof(struct lo_inode));
		if (!inode) {
			goto out_err;
		}

		inode->refcount = 1;
		inode->fd = newfd;
		inode->ino = e->attr.st_ino;
		inode->dev = e->attr.st_dev;

		pthread_mutex_lock(&lo->mutex);
		prev = &lo->root;
		next = prev->next;
		next->prev = inode;
		inode->next = next;
		inode->prev = prev;
		prev->next = inode;
		pthread_mutex_unlock(&lo->mutex);
	}
	e->ino = (uintptr_t)inode;

	SPDK_INFOLOG(ssam_fs, "%lli/%s -> %lli\n", (unsigned long long)parent, name,
		     (unsigned long long)e->ino);

	return 0;

out_err:
	saverr = errno;
	if (newfd != -1) {
		close(newfd);
	}
	return saverr;
}

static void lo_lookup(fuse_req_t req, fuse_ino_t parent, const char *name)
{
	struct fuse_entry_param e;
	int err;
	struct lo_data *lo = lo_data(req);
	unsigned lcore_id = rte_lcore_id();
	struct spdk_ssam_fs_session *fsmsession = (struct spdk_ssam_fs_session *)lo->smsession[lcore_id];

	SPDK_INFOLOG(ssam_fs, "lo_lookup(parent=%" PRIu64 ", name=%s)\n", parent, name);

	err = lo_do_lookup(req, parent, name, &e);
	if (err) {
		fuse_reply_err(req, err);
	} else {
		ssam_fuse_reply_entry(req, &e, fsmsession);
	}
}

static int mknod_wrapper(int dirfd, const char *path, const char *link, int mode, dev_t rdev)
{
	int res;

	if (S_ISREG(mode)) {
		res = openat(dirfd, path, O_CREAT | O_EXCL | O_WRONLY, mode);
		if (res >= 0) {
			res = close(res);
		}
	} else if (S_ISDIR(mode)) {
		res = mkdirat(dirfd, path, mode);
	} else if (S_ISLNK(mode) && link != NULL) {
		res = symlinkat(link, dirfd, path);
	} else if (S_ISFIFO(mode)) {
		res = mkfifoat(dirfd, path, mode);
#ifdef __FreeBSD__
	} else if (S_ISSOCK(mode)) {
		struct sockaddr_un su;
		int fd;

		if (strlen(path) >= sizeof(su.sun_path)) {
			errno = ENAMETOOLONG;
			return -1;
		}
		fd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (fd >= 0) {
			/*
			 * We must bind the socket to the underlying file
			 * system to create the socket file, even though
			 * we'll never listen on this socket.
			 */
			su.sun_family = AF_UNIX;
			snprintf(su.sun_path, sizeof(su.sun_path), "%s", path);
			res = bindat(dirfd, fd, (struct sockaddr *)&su, sizeof(su));
			if (res == 0) {
				close(fd);
			}
		} else {
			res = -1;
		}
#endif
	} else {
		res = mknodat(dirfd, path, mode, rdev);
	}

	return res;
}

static void lo_mknod_symlink(fuse_req_t req, fuse_ino_t parent, const char *name, mode_t mode,
			     dev_t rdev,
			     const char *link)
{
	int res;
	int saverr;
	struct lo_data *lo = lo_data(req);
	struct lo_inode *dir = lo_inode(req, parent);
	struct fuse_entry_param e;
	unsigned lcore_id = rte_lcore_id();
	struct spdk_ssam_fs_session *fsmsession = (struct spdk_ssam_fs_session *)lo->smsession[lcore_id];

	res = mknod_wrapper(dir->fd, name, link, mode, rdev);

	saverr = errno;
	if (res == -1) {
		goto out;
	}

	saverr = lo_do_lookup(req, parent, name, &e);
	if (saverr) {
		goto out;
	}

	SPDK_INFOLOG(ssam_fs, "%lli/%s -> %lli\n", (unsigned long long)parent, name,
		     (unsigned long long)e.ino);

	ssam_fuse_reply_entry(req, &e, fsmsession);
	return;

out:
	fuse_reply_err(req, saverr);
}

static void lo_mknod(fuse_req_t req, fuse_ino_t parent, const char *name, mode_t mode, dev_t rdev)
{
	lo_mknod_symlink(req, parent, name, mode, rdev, NULL);
}

static void lo_mkdir(fuse_req_t req, fuse_ino_t parent, const char *name, mode_t mode)
{
	lo_mknod_symlink(req, parent, name, S_IFDIR | mode, 0, NULL);
}

static void lo_symlink(fuse_req_t req, const char *link, fuse_ino_t parent, const char *name)
{
	lo_mknod_symlink(req, parent, name, S_IFLNK, 0, link);
}

static void lo_link(fuse_req_t req, fuse_ino_t ino, fuse_ino_t parent, const char *name)
{
	int res;
	struct lo_data *lo = lo_data(req);
	struct lo_inode *inode = lo_inode(req, ino);
	struct fuse_entry_param e;
	char procname[64];
	int saverr;
	unsigned lcore_id = rte_lcore_id();
	struct spdk_ssam_fs_session *fsmsession = (struct spdk_ssam_fs_session *)lo->smsession[lcore_id];

	memset(&e, 0, sizeof(struct fuse_entry_param));
	e.attr_timeout = lo->timeout;
	e.entry_timeout = lo->timeout;

	snprintf(procname, sizeof(procname), "/proc/self/fd/%i", inode->fd);
	res = linkat(AT_FDCWD, procname, lo_fd(req, parent), name, AT_SYMLINK_FOLLOW);
	if (res == -1) {
		goto out_err;
	}

	res = fstatat(inode->fd, "", &e.attr, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW);
	if (res == -1) {
		goto out_err;
	}

	pthread_mutex_lock(&lo->mutex);
	inode->refcount++;
	pthread_mutex_unlock(&lo->mutex);
	e.ino = (uintptr_t)inode;

	SPDK_INFOLOG(ssam_fs, "%lli/%s -> %lli\n", (unsigned long long)parent, name,
		     (unsigned long long)e.ino);

	ssam_fuse_reply_entry(req, &e, fsmsession);
	return;

out_err:
	saverr = errno;
	fuse_reply_err(req, saverr);
}

static void lo_rmdir(fuse_req_t req, fuse_ino_t parent, const char *name)
{
	int res;

	res = unlinkat(lo_fd(req, parent), name, AT_REMOVEDIR);

	fuse_reply_err(req, res == -1 ? errno : 0);
}

static void lo_rename(fuse_req_t req, fuse_ino_t parent, const char *name, fuse_ino_t newparent,
		      const char *newname,
		      unsigned int flags)
{
	int res;

	if (flags) {
		fuse_reply_err(req, EINVAL);
		return;
	}

	res = renameat(lo_fd(req, parent), name, lo_fd(req, newparent), newname);

	fuse_reply_err(req, res == -1 ? errno : 0);
}

static void lo_unlink(fuse_req_t req, fuse_ino_t parent, const char *name)
{
	int res;

	res = unlinkat(lo_fd(req, parent), name, 0);

	fuse_reply_err(req, res == -1 ? errno : 0);
}

static void unref_inode(struct lo_data *lo, struct lo_inode *inode, uint64_t n)
{
	if (!inode) {
		return;
	}

	pthread_mutex_lock(&lo->mutex);
	assert(inode->refcount >= n);
	inode->refcount -= n;
	if (!inode->refcount) {
		struct lo_inode *prev, *next;

		prev = inode->prev;
		next = inode->next;
		next->prev = prev;
		prev->next = next;

		pthread_mutex_unlock(&lo->mutex);
		close(inode->fd);
		free(inode);
	} else {
		pthread_mutex_unlock(&lo->mutex);
	}
}

static void lo_forget_one(fuse_req_t req, fuse_ino_t ino, uint64_t nlookup)
{
	struct lo_data *lo = lo_data(req);
	struct lo_inode *inode = lo_inode(req, ino);
	struct lo_inode *p;

	pthread_mutex_lock(&lo->mutex);
	for (p = lo->root.next; p != &lo->root; p = p->next) {
		if (p == inode) {
			break;
		}
	}
	pthread_mutex_unlock(&lo->mutex);
	if (p == &lo->root) {
		SPDK_INFOLOG(ssam_fs, "forget %lli-%lli\n", (unsigned long long)ino, (unsigned long long)nlookup);
		return;
	}

	SPDK_INFOLOG(ssam_fs, "forget %lli %lli -%lli\n", (unsigned long long)ino,
		     (unsigned long long)inode->refcount,
		     (unsigned long long)nlookup);

	unref_inode(lo, inode, nlookup);
}

static udaa_error_t udaa_eml_queue_empty_complete(int depth_idx, struct udaa_emlq *emlq,
		uint16_t tid,
		struct spdk_ssam_fs_session *fsmsession);

static void lo_forget(fuse_req_t req, fuse_ino_t ino, uint64_t nlookup)
{
	struct lo_data *lo = lo_data(req);
	unsigned lcore_id = rte_lcore_id();
	struct spdk_ssam_fs_session *fsmsession = (struct spdk_ssam_fs_session *)lo->smsession[lcore_id];
	unsigned tid = fsmsession->smsession.smdev->tid;

	lo_forget_one(req, ino, nlookup);
	udaa_eml_queue_empty_complete(0, lo->udaa_fs_queues[lcore_id], tid, fsmsession);
	fuse_reply_none(req);
}

static void lo_forget_multi(fuse_req_t req, size_t count, struct fuse_forget_data *forgets)
{
	size_t i;
	struct lo_data *lo = lo_data(req);
	unsigned lcore_id = rte_lcore_id();
	struct spdk_ssam_fs_session *fsmsession = (struct spdk_ssam_fs_session *)lo->smsession[lcore_id];
	unsigned tid = fsmsession->smsession.smdev->tid;

	for (i = 0; i < count; i++) {
		lo_forget_one(req, forgets[i].ino, forgets[i].nlookup);
	}
	udaa_eml_queue_empty_complete(0, lo->udaa_fs_queues[lcore_id], tid, fsmsession);
	fuse_reply_none(req);
}

static void lo_readlink(fuse_req_t req, fuse_ino_t ino)
{
	int res;
	struct lo_data *lo = lo_data(req);
	unsigned lcore_id = rte_lcore_id();
	struct spdk_ssam_fs_session *fsmsession = (struct spdk_ssam_fs_session *)lo->smsession[lcore_id];

	memset(fsmsession->static_buf, 0, SSAM_FS_STATIC_BUF_SIZE);
	res = readlinkat(lo_fd(req, ino), "", fsmsession->static_buf, SSAM_FS_STATIC_BUF_SIZE);
	if (res == -1) {
		return (void)fuse_reply_err(req, errno);
	}

	if (res == sizeof(fsmsession->static_buf)) {
		return (void)fuse_reply_err(req, ENAMETOOLONG);
	}

	fsmsession->static_buf[res] = '\0';

	fuse_reply_readlink(req, fsmsession->static_buf);
}

struct lo_dirp {
	DIR *dp;
	struct dirent *entry;
	off_t offset;
};

static struct lo_dirp *lo_dirp(struct fuse_file_info *fi)
{
	return (struct lo_dirp *)(uintptr_t)fi->fh;
}

static void lo_opendir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	int error = ENOMEM;
	struct lo_data *lo = lo_data(req);
	struct lo_dirp *d = NULL;
	int fd;
	unsigned lcore_id = rte_lcore_id();
	struct spdk_ssam_fs_session *fsmsession = (struct spdk_ssam_fs_session *)lo->smsession[lcore_id];

	d = calloc(1, sizeof(struct lo_dirp));
	if (d == NULL) {
		goto out_err;
	}

	fd = openat(lo_fd(req, ino), ".", O_RDONLY);
	if (fd == -1) {
		goto out_errno;
	}

	d->dp = fdopendir(fd);
	if (d->dp == NULL) {
		goto out_errno;
	}

	d->offset = 0;
	d->entry = NULL;

	fi->fh = (uintptr_t)d;
	if (lo->cache == CACHE_ALWAYS) {
		fi->cache_readdir = 1;
	}
	ssam_fuse_reply_open(req, fi, fsmsession);
	return;

out_errno:
	error = errno;
out_err:
	if (d) {
		if (fd != -1) {
			close(fd);
		}
		free(d);
	}
	fuse_reply_err(req, error);
}

static int is_dot_or_dotdot(const char *name)
{
	return name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'));
}

static void lo_do_readdir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t offset,
			  struct fuse_file_info *fi,
			  int plus)
{
	struct lo_dirp *d = lo_dirp(fi);
	char *p = NULL;
	size_t rem = size;
	int err;
	struct lo_data *lo = lo_data(req);
	unsigned lcore_id = rte_lcore_id();
	struct spdk_ssam_fs_session *fsmsession = (struct spdk_ssam_fs_session *)lo->smsession[lcore_id];
	uint64_t phys_addr;

	(void)ino;

	fsmsession->dynamic_buf = ssam_mempool_alloc(fsmsession->smsession.mp, size, &phys_addr);
	if (!fsmsession->dynamic_buf) {
		err = ENOMEM;
		goto error;
	}
	p = fsmsession->dynamic_buf;

	if (offset != d->offset) {
		seekdir(d->dp, offset);
		d->entry = NULL;
		d->offset = offset;
	}
	while (1) {
		size_t entsize;
		off_t nextoff;
		const char *name;

		if (!d->entry) {
			errno = 0;
			d->entry = readdir(d->dp);
			if (!d->entry) {
				if (errno) { /* Error */
					err = errno;
					goto error;
				} else { /* End of stream */
					break;
				}
			}
		}
		nextoff = d->entry->d_off;
		name = d->entry->d_name;
		fuse_ino_t entry_ino = 0;
		if (plus) {
			struct fuse_entry_param e;
			if (is_dot_or_dotdot(name)) {
				e = (struct fuse_entry_param) {
					.attr.st_ino = d->entry->d_ino,
					.attr.st_mode = d->entry->d_type << 12,
				};
			} else {
				err = lo_do_lookup(req, ino, name, &e);
				if (err) {
					goto error;
				}
				entry_ino = e.ino;
			}

			entsize = fuse_add_direntry_plus(req, p, rem, name, &e, nextoff);
		} else {
			struct stat st = {
				.st_ino = d->entry->d_ino,
				.st_mode = d->entry->d_type << 12,
			};
			entsize = fuse_add_direntry(req, p, rem, name, &st, nextoff);
		}
		if (entsize > rem) {
			if (entry_ino != 0) {
				lo_forget_one(req, entry_ino, 1);
			}
			break;
		}

		p += entsize;
		rem -= entsize;

		d->entry = NULL;
		d->offset = nextoff;
	}

	err = 0;
error:
	/* If there's an error, we can only signal it if we haven't stored
	 * any entries yet - otherwise we'd end up with wrong lookup
	 * counts for the entries that are already in the buffer. So we
	 * return what we've collected until that point.
	 */
	if (err && rem == size) {
		fuse_reply_err(req, err);
		ssam_mempool_free(fsmsession->smsession.mp, fsmsession->dynamic_buf);
		fsmsession->dynamic_buf = NULL;
	} else {
		fuse_reply_buf(req, fsmsession->dynamic_buf, size - rem);
	}
}

static void lo_readdir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t offset,
		       struct fuse_file_info *fi)
{
	lo_do_readdir(req, ino, size, offset, fi, 0);
}

static void lo_readdirplus(fuse_req_t req, fuse_ino_t ino, size_t size, off_t offset,
			   struct fuse_file_info *fi)
{
	lo_do_readdir(req, ino, size, offset, fi, 1);
}

static void lo_releasedir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	struct lo_dirp *d = lo_dirp(fi);
	(void)ino;
	closedir(d->dp);
	free(d);
	fuse_reply_err(req, 0);
}

static void lo_create(fuse_req_t req, fuse_ino_t parent, const char *name, mode_t mode,
		      struct fuse_file_info *fi)
{
	int fd;
	struct lo_data *lo = lo_data(req);
	struct fuse_entry_param e;
	int err;
	unsigned lcore_id = rte_lcore_id();
	struct spdk_ssam_fs_session *fsmsession = (struct spdk_ssam_fs_session *)lo->smsession[lcore_id];

	SPDK_INFOLOG(ssam_fs, "lo_create(parent=%" PRIu64 ", name=%s)\n", parent, name);

	fd = openat(lo_fd(req, parent), name, (fi->flags | O_CREAT) & ~O_NOFOLLOW, mode);
	if (fd == -1) {
		return (void)fuse_reply_err(req, errno);
	}

	fi->fh = fd;
	if (lo->cache == CACHE_NEVER) {
		fi->direct_io = 1;
	} else if (lo->cache == CACHE_ALWAYS) {
		fi->keep_cache = 1;
	}

	err = lo_do_lookup(req, parent, name, &e);
	if (err) {
		fuse_reply_err(req, err);
	} else {
		ssam_fuse_reply_create(req, &e, fi, fsmsession);
	}
}

static void lo_fsyncdir(fuse_req_t req, fuse_ino_t ino, int datasync, struct fuse_file_info *fi)
{
	int res;
	int fd = dirfd(lo_dirp(fi)->dp);
	(void)ino;
	if (datasync) {
		res = fdatasync(fd);
	} else {
		res = fsync(fd);
	}
	fuse_reply_err(req, res == -1 ? errno : 0);
}

static void lo_open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	int fd;
	char buf[64];
	struct lo_data *lo = lo_data(req);
	unsigned lcore_id = rte_lcore_id();
	struct spdk_ssam_fs_session *fsmsession = (struct spdk_ssam_fs_session *)lo->smsession[lcore_id];

	SPDK_INFOLOG(ssam_fs, "lo_open(ino=%" PRIu64 ", flags=%d)\n", ino, fi->flags);

	/* With writeback cache, kernel may send read requests even
	   when userspace opened write-only */
	if (lo->writeback && (fi->flags & O_ACCMODE) == O_WRONLY) {
		fi->flags &= ~O_ACCMODE;
		fi->flags |= O_RDWR;
	}

	/* With writeback cache, O_APPEND is handled by the kernel.
	 *  This breaks atomicity (since the file may change in the
	 *  underlying filesystem, so that the kernel's idea of the
	 *  end of the file isn't accurate anymore). In this example,
	 *  we just accept that. A more rigorous filesystem may want
	 *  to return an error here
	 */
	if (lo->writeback && (fi->flags & O_APPEND)) {
		fi->flags &= ~O_APPEND;
	}

	snprintf(buf, sizeof(buf), "/proc/self/fd/%i", lo_fd(req, ino));
	fd = open(buf, fi->flags & ~O_NOFOLLOW);
	if (fd == -1) {
		return (void)fuse_reply_err(req, errno);
	}

	fi->fh = fd;
	if (lo->cache == CACHE_NEVER) {
		fi->direct_io = 1;
	} else if (lo->cache == CACHE_ALWAYS) {
		fi->keep_cache = 1;
	}
	ssam_fuse_reply_open(req, fi, fsmsession);
}

static void lo_release(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	(void)ino;

	close(fi->fh);
	fuse_reply_err(req, 0);
}

static void lo_flush(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	int res;
	(void)ino;
	res = close(dup(fi->fh));
	fuse_reply_err(req, res == -1 ? errno : 0);
}

static void lo_fsync(fuse_req_t req, fuse_ino_t ino, int datasync, struct fuse_file_info *fi)
{
	int res;
	(void)ino;
	if (datasync) {
		res = fdatasync(fi->fh);
	} else {
		res = fsync(fi->fh);
	}
	fuse_reply_err(req, res == -1 ? errno : 0);
}

static void lo_read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t offset,
		    struct fuse_file_info *fi)
{
	struct fuse_bufvec buf = FUSE_BUFVEC_INIT(size);
	struct lo_data *lo = lo_data(req);
	unsigned lcore_id = rte_lcore_id();
	struct spdk_ssam_fs_session *fsmsession = (struct spdk_ssam_fs_session *)lo->smsession[lcore_id];

	SPDK_INFOLOG(ssam_fs, "lo_read(ino=%" PRIu64 ", size=%zd, off=%lu)\n", ino, size,
		     (unsigned long)offset);
	fsmsession->fs_stat.payload_size = size;
	buf.buf[0].flags = FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK;
	buf.buf[0].fd = fi->fh;
	buf.buf[0].pos = offset;

	ssam_fuse_reply_data(req, &buf, FUSE_BUF_SPLICE_MOVE, fsmsession);
}

static void lo_write_buf(fuse_req_t req, fuse_ino_t ino, struct fuse_bufvec *in_buf, off_t off,
			 struct fuse_file_info *fi)
{
	(void)ino;
	ssize_t res;
	struct fuse_bufvec out_buf = FUSE_BUFVEC_INIT(fuse_buf_size(in_buf));
	struct lo_data *lo = lo_data(req);
	unsigned lcore_id = rte_lcore_id();
	struct spdk_ssam_fs_session *fsmsession = (struct spdk_ssam_fs_session *)lo->smsession[lcore_id];

	out_buf.buf[0].flags = FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK;
	out_buf.buf[0].fd = fi->fh;
	out_buf.buf[0].pos = off;

	SPDK_INFOLOG(ssam_fs, "lo_write(ino=%" PRIu64 ", size=%zd, off=%lu)\n", ino, out_buf.buf[0].size,
		     (unsigned long)off);
	fsmsession->fs_stat.payload_size = out_buf.buf[0].size;
	res = fuse_buf_copy(&out_buf, in_buf, 0);
	if (res < 0) {
		fuse_reply_err(req, -res);
	} else {
		ssam_fuse_reply_write(req, (size_t)res, fsmsession);
	}
}

static void lo_statfs(fuse_req_t req, fuse_ino_t ino)
{
	int res;
	struct statvfs stbuf;
	struct lo_data *lo = lo_data(req);
	unsigned lcore_id = rte_lcore_id();
	struct spdk_ssam_fs_session *fsmsession = (struct spdk_ssam_fs_session *)lo->smsession[lcore_id];

	res = fstatvfs(lo_fd(req, ino), &stbuf);
	if (res == -1) {
		fuse_reply_err(req, errno);
	} else {
		ssam_fuse_reply_statfs(req, &stbuf, fsmsession);
	}
}

static void lo_fallocate(fuse_req_t req, fuse_ino_t ino, int mode, off_t offset, off_t length,
			 struct fuse_file_info *fi)
{
	int err = EOPNOTSUPP;
	(void)ino;

#ifdef HAVE_FALLOCATE
	err = fallocate(fi->fh, mode, offset, length);
	if (err < 0) {
		err = errno;
	}

#elif defined(HAVE_POSIX_FALLOCATE)
	if (mode) {
		fuse_reply_err(req, EOPNOTSUPP);
		return;
	}

	err = posix_fallocate(fi->fh, offset, length);
#endif

	fuse_reply_err(req, err);
}

static void lo_flock(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi, int op)
{
	int res;
	(void)ino;

	res = flock(fi->fh, op);

	fuse_reply_err(req, res == -1 ? errno : 0);
}

static void lo_getxattr(fuse_req_t req, fuse_ino_t ino, const char *name, size_t size)
{
	char procname[64];
	struct lo_inode *inode = lo_inode(req, ino);
	ssize_t ret;
	int saverr;
	struct lo_data *lo = lo_data(req);
	unsigned lcore_id = rte_lcore_id();
	struct spdk_ssam_fs_session *fsmsession = (struct spdk_ssam_fs_session *)lo->smsession[lcore_id];
	uint64_t phys_addr;

	saverr = ENOSYS;
	if (!lo_data(req)->xattr) {
		goto out;
	}

	SPDK_INFOLOG(ssam_fs, "lo_getxattr(ino=%" PRIu64 ", name=%s size=%zd)\n", ino, name, size);

	snprintf(procname, sizeof(procname), "/proc/self/fd/%i", inode->fd);

	if (size) {
		fsmsession->dynamic_buf = ssam_mempool_alloc(fsmsession->smsession.mp, size, &phys_addr);
		if (!fsmsession->dynamic_buf) {
			goto out_err;
		}

		ret = getxattr(procname, name, fsmsession->dynamic_buf, size);
		if (ret == -1) {
			goto out_err;
		}
		saverr = 0;
		if (ret == 0) {
			goto out;
		}

		fuse_reply_buf(req, fsmsession->dynamic_buf, ret);
		return;
	} else {
		ret = getxattr(procname, name, NULL, 0);
		if (ret == -1) {
			goto out_err;
		}

		ssam_fuse_reply_xattr(req, ret, fsmsession);
	}
out_free:
	if (fsmsession->dynamic_buf != NULL) {
		ssam_mempool_free(fsmsession->smsession.mp, fsmsession->dynamic_buf);
		fsmsession->dynamic_buf = NULL;
	}
	return;

out_err:
	saverr = errno;
out:
	fuse_reply_err(req, saverr);
	goto out_free;
}

static void lo_listxattr(fuse_req_t req, fuse_ino_t ino, size_t size)
{
	char procname[64];
	struct lo_inode *inode = lo_inode(req, ino);
	ssize_t ret;
	int saverr;
	struct lo_data *lo = lo_data(req);
	unsigned lcore_id = rte_lcore_id();
	struct spdk_ssam_fs_session *fsmsession = (struct spdk_ssam_fs_session *)lo->smsession[lcore_id];
	uint64_t phys_addr;

	saverr = ENOSYS;
	if (!lo_data(req)->xattr) {
		goto out;
	}

	SPDK_INFOLOG(ssam_fs, "lo_listxattr(ino=%" PRIu64 ", size=%zd)\n", ino, size);

	snprintf(procname, sizeof(procname), "/proc/self/fd/%i", inode->fd);

	if (size) {
		fsmsession->dynamic_buf = ssam_mempool_alloc(fsmsession->smsession.mp, size, &phys_addr);
		if (!fsmsession->dynamic_buf) {
			goto out_err;
		}

		ret = listxattr(procname, fsmsession->dynamic_buf, size);
		if (ret == -1) {
			goto out_err;
		}
		saverr = 0;
		if (ret == 0) {
			goto out;
		}

		fuse_reply_buf(req, fsmsession->dynamic_buf, ret);
		return;
	} else {
		ret = listxattr(procname, NULL, 0);
		if (ret == -1) {
			goto out_err;
		}

		ssam_fuse_reply_xattr(req, ret, fsmsession);
	}
out_free:
	if (fsmsession->dynamic_buf != NULL) {
		ssam_mempool_free(fsmsession->smsession.mp, fsmsession->dynamic_buf);
		fsmsession->dynamic_buf = NULL;
	}
	return;

out_err:
	saverr = errno;
out:
	fuse_reply_err(req, saverr);
	goto out_free;
}

static void lo_setxattr(fuse_req_t req, fuse_ino_t ino, const char *name, const char *value,
			size_t size, int flags)
{
	char procname[64];
	struct lo_inode *inode = lo_inode(req, ino);
	ssize_t ret;
	int saverr;

	saverr = ENOSYS;
	if (!lo_data(req)->xattr) {
		goto out;
	}

	SPDK_INFOLOG(ssam_fs, "lo_setxattr(ino=%" PRIu64 ", name=%s value=%s size=%zd)\n", ino, name, value,
		     size);

	snprintf(procname, sizeof(procname), "/proc/self/fd/%i", inode->fd);

	ret = setxattr(procname, name, value, size, flags);
	saverr = ret == -1 ? errno : 0;

out:
	fuse_reply_err(req, saverr);
}

static void lo_removexattr(fuse_req_t req, fuse_ino_t ino, const char *name)
{
	char procname[64];
	struct lo_inode *inode = lo_inode(req, ino);
	ssize_t ret;
	int saverr;

	saverr = ENOSYS;
	if (!lo_data(req)->xattr) {
		goto out;
	}

	SPDK_INFOLOG(ssam_fs, "lo_removexattr(ino=%" PRIu64 ", name=%s)\n", ino, name);

	snprintf(procname, sizeof(procname), "/proc/self/fd/%i", inode->fd);

	ret = removexattr(procname, name);
	saverr = ret == -1 ? errno : 0;

out:
	fuse_reply_err(req, saverr);
}

#ifdef HAVE_COPY_FILE_RANGE
static void lo_copy_file_range(fuse_req_t req, fuse_ino_t ino_in, off_t off_in,
			       struct fuse_file_info *fi_in,
			       fuse_ino_t ino_out, off_t off_out, struct fuse_file_info *fi_out, size_t len, int flags)
{
	ssize_t res;
	struct lo_data *lo = lo_data(req);
	unsigned lcore_id = rte_lcore_id();
	struct spdk_ssam_fs_session *fsmsession = (struct spdk_ssam_fs_session *)lo->smsession[lcore_id];

	SPDK_INFOLOG(ssam_fs,
		     "lo_copy_file_range(ino=%" PRIu64 "/fd=%lu, off=%lu, ino=%" PRIu64 "/fd=%lu, "
		     "off=%lu, size=%zd, flags=0x%x)\n",
		     ino_in, fi_in->fh, off_in, ino_out, fi_out->fh, off_out, len, flags);

	res = copy_file_range(fi_in->fh, &off_in, fi_out->fh, &off_out, len, flags);
	if (res < 0) {
		fuse_reply_err(req, errno);
	} else {
		ssam_fuse_reply_write(req, res, fsmsession);
	}
}
#endif

static void lo_lseek(fuse_req_t req, fuse_ino_t ino, off_t off, int whence,
		     struct fuse_file_info *fi)
{
	off_t res;

	(void)ino;
	res = lseek(fi->fh, off, whence);
	if (res != -1) {
		fuse_reply_lseek(req, res);
	} else {
		fuse_reply_err(req, errno);
	}
}

static const struct fuse_lowlevel_ops lo_oper = {
	.init = lo_init,
	.destroy = lo_destroy,
	.lookup = lo_lookup,
	.mkdir = lo_mkdir,
	.mknod = lo_mknod,
	.symlink = lo_symlink,
	.link = lo_link,
	.unlink = lo_unlink,
	.rmdir = lo_rmdir,
	.rename = lo_rename,
	.forget = lo_forget,
	.forget_multi = lo_forget_multi,
	.getattr = lo_getattr,
	.setattr = lo_setattr,
	.readlink = lo_readlink,
	.opendir = lo_opendir,
	.readdir = lo_readdir,
	.readdirplus = lo_readdirplus,
	.releasedir = lo_releasedir,
	.fsyncdir = lo_fsyncdir,
	.create = lo_create,
	.open = lo_open,
	.release = lo_release,
	.flush = lo_flush,
	.fsync = lo_fsync,
	.read = lo_read,
	.write_buf = lo_write_buf,
	.statfs = lo_statfs,
	.fallocate = lo_fallocate,
	.flock = lo_flock,
	.getxattr = lo_getxattr,
	.listxattr = lo_listxattr,
	.setxattr = lo_setxattr,
	.removexattr = lo_removexattr,
#ifdef HAVE_COPY_FILE_RANGE
	.copy_file_range = lo_copy_file_range,
#endif
	.lseek = lo_lseek,
};

static void
ssam_task_stat_tick(uint64_t *tsc)
{
#ifdef PERF_STAT
	*tsc = spdk_get_ticks();
#endif
	return;
}

static void
ssam_fs_stat_statistics(struct spdk_ssam_session *smsession, uint8_t status)
{
#ifdef PERF_STAT
	struct spdk_ssam_fs_session *fsmsession = (struct spdk_ssam_fs_session *)smsession;
	uint64_t total_tsc = fsmsession->fs_stat.complete_end_tsc - fsmsession->fs_stat.start_tsc;

	if (fsmsession->fs_stat.op_type == SSAM_FUSE_OPCODE_READ) {   /* read */
		fsmsession->fs_stat.read_latency_ticks += total_tsc;
		fsmsession->fs_stat.bytes_read += fsmsession->fs_stat.payload_size;
		fsmsession->fs_stat.num_read_ops++;
		if (status == 0) {
			fsmsession->fs_stat.complete_read_ios++;
		} else {
			fsmsession->fs_stat.err_read_ios++;
		}
	} else if (fsmsession->fs_stat.op_type == SSAM_FUSE_OPCODE_WRITE) {    /* write */
		fsmsession->fs_stat.write_latency_ticks += total_tsc;
		fsmsession->fs_stat.bytes_written += fsmsession->fs_stat.payload_size;
		fsmsession->fs_stat.num_write_ops++;
		if (status == 0) {
			fsmsession->fs_stat.complete_write_ios++;
		} else {
			fsmsession->fs_stat.err_write_ios++;
		}
	} else if (fsmsession->fs_stat.op_type == SSAM_FUSE_OPCODE_FLUSH) {   /* flush */
		fsmsession->fs_stat.flush_ios++;
		if (status == 0) {
			fsmsession->fs_stat.complete_flush_ios++;
		} else {
			fsmsession->fs_stat.err_flush_ios++;
		}
	} else {
		fsmsession->fs_stat.other_ios++;
	}

	fsmsession->fs_stat.payload_size = 0;
#endif
}

static udaa_error_t udaa_eml_queue_create(uint32_t depth, struct udaa_emlq **emlq,
		uint16_t queue_id)
{
	struct udaa_emlq *eml_queue = NULL;

	if (depth <= 0) {
		SPDK_ERRLOG("Invalid depth: %d\n", depth);
		return UDAA_ERROR_INVALID_VALUE;
	}

	eml_queue = (struct udaa_emlq *)malloc(sizeof(struct udaa_emlq));
	if (eml_queue == NULL) {
		return UDAA_ERROR_NO_MEMORY;
	}

	eml_queue->vmio_req = (struct ssam_request **)malloc(depth * sizeof(struct ssam_request *));
	if (eml_queue->vmio_req == NULL) {
		return UDAA_ERROR_NO_MEMORY;
	}

	eml_queue->queue_id = queue_id;
	eml_queue->eml_type = UDAA_PCI_FUNC_VIRTIO_FS;
	*emlq = eml_queue;

	return UDAA_SUCCESS;
}

static udaa_error_t vio_do_dma_async(uint16_t queue_id, struct ssam_dma_request *dma_req)
{
	int res;

	res = ssam_dma_data_request(queue_id, dma_req);
	if (res != 0) {
		SPDK_ERRLOG("ssam_dma_data_request failed: %d\n", res);
		return UDAA_ERROR_IO_FAILED;
	}

	return UDAA_SUCCESS;
}

static udaa_error_t vio_build_request(uint16_t queue_id, struct ssam_request *vmio_req, void *buf,
				      int num_sge,
				      int len, int skip_sges, struct spdk_ssam_fs_session *fsmsession)
{
	struct ssam_dma_request dma_req = { 0 };
	udaa_error_t result;
	struct spdk_ssam_dma_cb dma_cb = {
		.status = 0,
		.req_dir = 0, /* read */
		.gfunc_id = vmio_req->gfunc_id,
		.vq_idx = 0,
		.task_idx = 0
	};

	/* DMA sges from host */
	dma_req.src = &vmio_req->req.cmd.iovs[skip_sges];
	dma_req.src_num = num_sge;
	dma_req.cb = (void *) * (uint64_t *)&dma_cb;
	dma_req.direction = READ_HOST_MODE;
	fsmsession->dst_iov.iov_len = len;
	fsmsession->dst_iov.iov_base = (void *)spdk_vtophys((void *)buf, NULL);
	dma_req.data_len = len;
	dma_req.dst = &fsmsession->dst_iov;
	dma_req.dst_num = 1;
	dma_req.flr_seq = vmio_req->flr_seq;
	dma_req.gfunc_id = vmio_req->gfunc_id;

	result = vio_do_dma_async(queue_id, &dma_req);
	if (result != UDAA_SUCCESS) {
		SPDK_ERRLOG("vio_do_dma_async failed: %d\n", result);
		return result;
	}

	return UDAA_ERROR_AGAIN;
}

static void ssam_fuse_share_memory(struct spdk_ssam_fs_session *fsmsession)
{
	struct fuse_buf *fbuf = &fsmsession->fbuf;
	struct fuse_in_header *in = fbuf->mem;
	char name[SHM_NAME] = {0};
	int shm_fd = 0;
	struct mount_info *info = NULL;

	if (in->opcode == SSAM_FUSE_OPCODE_INIT) {
		snprintf(name, sizeof(name), "shm_name%d", fsmsession->p_lo->gfunc_id);
		shm_fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
		if (shm_fd == -1) {
			SPDK_NOTICELOG("could not open %s\n", name);
			return;
		}

		if (ftruncate(shm_fd, SHM_SIZE) != 0) {
			SPDK_ERRLOG("could not truncate %s\n", name);
			close(shm_fd);
			return;
		}

		info = mmap(NULL, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
		if (info == MAP_FAILED) {
			close(shm_fd);
			SPDK_ERRLOG("fail to set shared memory\n");
			return;
		}

		info->fd = fsmsession->fbuf.fd;
		info->flags = fsmsession->fbuf.flags;
		info->pos = fsmsession->fbuf.pos;
		info->size = fsmsession->fbuf.size;
		info->opcode = in->opcode;
		info->gid = in->gid;
		info->len = in->len;
		info->nodeid = in->nodeid;
		info->padding = in->padding;
		info->pid = in->pid;
		info->total_extlen = in->total_extlen;
		info->uid = in->uid;
		info->unique = in->unique;
		munmap(info, SHM_SIZE);
		close(shm_fd);
		SPDK_NOTICELOG("succss store to shm_mount\n");
	} else if (in->opcode == SSAM_FUSE_OPCODE_DESTROY) {
		snprintf(name, sizeof(name), "shm_name%d", fsmsession->p_lo->gfunc_id);
		shm_unlink(name);
		SPDK_NOTICELOG("success close shm_mount\n");
	}

	return;
}

static int vio_vmio_complete(uint16_t queue_id, struct ssam_request *req, void *buf, int sge_index,
			     struct spdk_ssam_fs_session *fsmsession)
{
	struct ssam_io_response resp;
	struct ssam_virtio_res *virtio_res = (struct ssam_virtio_res *)&resp.data;
	struct iovec iov;

	memset(&resp, 0, sizeof(resp));
	resp.gfunc_id = req->gfunc_id;
	resp.iocb_id = req->iocb_id;
	resp.flr_seq = req->flr_seq;
	resp.status = req->status;
	resp.req = req;

	memcpy(&iov, &req->req.cmd.iovs[sge_index], sizeof(req->req.cmd.iovs[sge_index]));
	virtio_res->iovs = &iov;
	virtio_res->iovcnt = 1;
	virtio_res->rsp = buf;
	virtio_res->rsp_len = req->req.cmd.iovs[sge_index].iov_len;

	fsmsession->fbuf_used = false;
	ssam_dev_io_dec(fsmsession->smsession.smdev);

	struct spdk_ssam_session *smsession = (struct spdk_ssam_session *)fsmsession;
	ssam_fs_stat_statistics(smsession, resp.status);
	return ssam_io_complete(queue_id, &resp);
}

static int vio_vmio_empty_complete(uint16_t queue_id, struct ssam_request *req,
				   struct spdk_ssam_fs_session *fsmsession)
{
	struct ssam_io_response resp;
	struct ssam_virtio_res *virtio_res = (struct ssam_virtio_res *)&resp.data;

	memset(&resp, 0, sizeof(resp));
	resp.gfunc_id = req->gfunc_id;
	resp.iocb_id = req->iocb_id;
	resp.flr_seq = req->flr_seq;
	resp.status = req->status;
	resp.req = req;

	virtio_res->rsp_len = 0;
	virtio_res->iovcnt = 0;

	fsmsession->fbuf_used = false;
	ssam_dev_io_dec(fsmsession->smsession.smdev);

	struct spdk_ssam_session *smsession = (struct spdk_ssam_session *)fsmsession;
	ssam_fs_stat_statistics(smsession, resp.status);
	return ssam_io_complete(queue_id, &resp);
}

static size_t iov_length(const struct iovec *iov, size_t count)
{
	size_t seg;
	size_t ret = 0;

	for (seg = 0; seg < count; seg++) {
		ret += iov[seg].iov_len;
	}
	return ret;
}

static udaa_error_t udaa_get_hdr_len(udaa_eml_type_t func_eml_type, int *hdr_len)
{
	switch (func_eml_type) {
	case UDAA_PCI_FUNC_VIRTIO_FS:
		*hdr_len = 40;
		break;
	case UDAA_PCI_FUNC_NVME:
	case UDAA_PCI_FUNC_VIRTIO_NET:
	case UDAA_PCI_FUNC_VIRTIO_BLK:
	case UDAA_PCI_FUNC_VIRTIO_SCSI:
	case UDAA_PCI_FUNC_VIRTIO_VSOCK:
	default:
		SPDK_ERRLOG("Not supported func_eml_type:%d\n", func_eml_type);
		return UDAA_ERROR_NOT_SUPPORTED;
	}
	return UDAA_SUCCESS;
}

static udaa_error_t udaa_poll_batch_blocking(int *polled_num, uint16_t tid, uint16_t poll_num,
		struct ssam_request **vmio_req, struct iovec *ext, uint16_t queue_id, ssize_t *in_len,
		struct lo_data *p_lo)
{
	struct ssam_request_poll_opt poll_opt = {
		.sge1_iov = ext,
		.queue_id = queue_id,
	};
	pthread_mutex_lock(&g_ssam_fs_poller_ctx.poll_mutex[p_lo->gfunc_id]);
	*polled_num = ssam_request_poll_ext(tid, poll_num, vmio_req, &poll_opt);
	pthread_mutex_unlock(&g_ssam_fs_poller_ctx.poll_mutex[p_lo->gfunc_id]);
	if ((*polled_num) < 0) {
		*in_len = *polled_num;
		return UDAA_ERROR_AGAIN;
	}
	if ((*polled_num) == 0) {
		return UDAA_ERROR_AGAIN;
	}

	return UDAA_SUCCESS;
}

static udaa_error_t udaa_poll_batch_non_blocking(int *polled_num, uint16_t tid, uint16_t poll_num,
		struct ssam_request **vmio_req, struct iovec *ext, uint16_t queue_id, ssize_t *in_len,
		struct lo_data *p_lo)
{
	struct ssam_request_poll_opt poll_opt = {
		.sge1_iov = ext,
		.queue_id = queue_id,
	};
	pthread_mutex_lock(&g_ssam_fs_poller_ctx.poll_mutex[p_lo->gfunc_id]);
	*polled_num = ssam_request_poll_ext(tid, poll_num, vmio_req, &poll_opt);
	pthread_mutex_unlock(&g_ssam_fs_poller_ctx.poll_mutex[p_lo->gfunc_id]);
	if ((*polled_num) < 0) {
		*in_len = *polled_num;
		return UDAA_ERROR_IO_FAILED;
	}
	if ((*polled_num) == 0) {
		return UDAA_ERROR_AGAIN;
	}

	return UDAA_SUCCESS;
}

static udaa_error_t udaa_eml_queue_progress_retrieve(struct udaa_emlq *emlq,
		struct udaa_eml_req *eml_req,
		int depth_idx, ssize_t *in_len, int is_blocking, unsigned tid,
		struct spdk_ssam_fs_session *fsmsession)
{
	int polled_num = 0;
	uint16_t queue_id = emlq->queue_id, poll_num = 1;
	size_t iov0_len;
	struct iovec ext; /* For sge1 pre-fetch (extension) */
	struct ssam_request **vmio_req = &(emlq->vmio_req[depth_idx]);
	udaa_error_t result;
	void *buf = eml_req->buf;
	size_t buf_len = eml_req->buf_len;
	int skip_sges = 0;
	int hdr_len;
	udaa_eml_type_t func_eml_type = emlq->eml_type;

	result = udaa_get_hdr_len(func_eml_type, &hdr_len);
	if (result != UDAA_SUCCESS) {
		SPDK_ERRLOG("Failed to get hdr_len. udaa_error value: %d\n", result);
		return result;
	}

	ext.iov_base = (uint8_t *)buf + hdr_len;
	ext.iov_len = buf_len - hdr_len;

	if (is_blocking) {
		result = udaa_poll_batch_blocking(&polled_num, tid, poll_num, vmio_req, &ext, queue_id, in_len,
						  fsmsession->p_lo);
		if (result != UDAA_SUCCESS) {
			if (result != UDAA_ERROR_AGAIN) {
				SPDK_ERRLOG("Failed to poll request polled_num = %d. udaa_error value: %d\n", polled_num, result);
			}
			return result;
		}
	} else {
		result = udaa_poll_batch_non_blocking(&polled_num, tid, poll_num, vmio_req, &ext, queue_id, in_len,
						      fsmsession->p_lo);
		if (result != UDAA_SUCCESS) {
			if (result != UDAA_ERROR_AGAIN) {
				SPDK_ERRLOG("Failed to poll request polled_num = %d. udaa_error value: %d\n", polled_num, result);
			}
			return result;
		}
	}
	fsmsession->fbuf_used = true;
	fsmsession->smsession.smdev->io_num++;

	/* user can mount tag again when reboot host without umount */
	if (fsmsession->p_lo->flr_seq != UINT32_MAX && fsmsession->p_lo->flr_seq != vmio_req[0]->flr_seq &&
	    fsmsession->p_lo->mounted == true) {
		lo_destroy((void *)fsmsession->p_lo);
	}
	fsmsession->p_lo->flr_seq = vmio_req[0]->flr_seq;

	ssam_task_stat_tick(&fsmsession->fs_stat.start_tsc);
	/* An element contains one request and the space to send our response
	 * They're spread over multiple descriptors in a scatter/gather set
	 * and we can't trust the guest to keep them still; so copy in/out.
	 */
	unsigned int in_num = vmio_req[0]->req.cmd.writable;
	struct iovec *in_sges = vmio_req[0]->req.cmd.iovs;
	*in_len = iov_length(in_sges, in_num);

	if (*in_len > (ssize_t)buf_len) {
		SPDK_ERRLOG("in_len exceed buf_len. in_len=%zd, buf_len=%zd\n", *in_len, buf_len);
		*in_len = -1;
		return UDAA_ERROR_IO_FAILED;
	}

	iov0_len = vmio_req[0]->req.cmd.iovs[0].iov_len;

	/* Copy fuse_in_header from buffer */
	memcpy(buf, vmio_req[0]->req.cmd.header, hdr_len);
	buf = (uint8_t *)buf + iov0_len + ext.iov_len;

	/* Fill Emulation request's type and buffers */
	eml_req->type = (int)func_eml_type;

	/* Get the rest of the request */
	if (in_num > 1) {
		if (ext.iov_len == 0) {
			/* Couldn't prefetch sge1, need to get it with DMA */
			skip_sges = 1;
		} else {
			skip_sges = 2;
		}
		in_num -= skip_sges;
		if (in_num > 0) {
			result = vio_build_request(tid, vmio_req[0], buf, in_num, *in_len - iov0_len - ext.iov_len,
						   skip_sges,
						   fsmsession);
			if (result != UDAA_ERROR_AGAIN) {
				SPDK_ERRLOG("vio_build_request failed: %d\n", result);
				*in_len = -1;
				return UDAA_ERROR_IO_FAILED;
			}
			fsmsession->in_len = *in_len;
		}
	}

	return result;
}

static udaa_error_t udaa_eml_queue_progress_response(struct udaa_emlq *emlq,
		struct udaa_eml_req *eml_req,
		struct iovec *iov, int count, int depth_idx, ssize_t *out_len, unsigned tid,
		struct spdk_ssam_fs_session *fsmsession)
{
	struct ssam_dma_request dma_req;
	int i;
	int len;
	size_t dst_len;
	int sge_index;
	struct ssam_request **vmio_req = &(emlq->vmio_req[depth_idx]);
	udaa_error_t result = UDAA_SUCCESS;
	struct spdk_ssam_dma_cb dma_cb = {
		.status = 0,
		.req_dir = 1, /* write */
		.gfunc_id = vmio_req[0]->gfunc_id,
		.vq_idx = 0,
		.task_idx = 0
	};
	struct fuse_in_header *in = fsmsession->fbuf.mem;

	dma_req.cb = (void *) * (uint64_t *)&dma_cb;
	dma_req.direction = WRITE_HOST_MODE;
	dma_req.data_len = 0;

	if (count <= 0) {
		SPDK_ERRLOG("Invalid count: %d\n", count);
		*out_len = -1;
		return UDAA_ERROR_IO_FAILED;
	}

	sge_index = vmio_req[0]->req.cmd.writable;
	fsmsession->fs_stat.op_type = in->opcode;

	if (count == 1) {
		goto complete;
	}

	fsmsession->src_iov = calloc(count - 1, sizeof(*fsmsession->src_iov));
	if (fsmsession->src_iov == NULL) {
		SPDK_ERRLOG("Failed to alloc src_iov\n");
		*out_len = -1;
		result = UDAA_ERROR_NO_MEMORY;
		goto complete;
	}

	for (i = 1; i < count; i++) {
		if (in->opcode == SSAM_FUSE_OPCODE_INIT) {
			memcpy(fsmsession->static_buf, iov[i].iov_base, sizeof(struct fuse_init_out));
			fsmsession->src_iov[i - 1].iov_base = (void *)spdk_vtophys(fsmsession->static_buf, NULL);
			len = sizeof(struct fuse_init_out);
		} else {
			fsmsession->src_iov[i - 1].iov_base = (void *)spdk_vtophys(iov[i].iov_base, NULL);
			len = iov[i].iov_len;
		}
		fsmsession->src_iov[i - 1].iov_len = len;
		dma_req.data_len += len;
	}

	if (dma_req.data_len == 0) {
		goto complete;
	}

	dma_req.src = fsmsession->src_iov;
	dma_req.src_num = count - 1;
	dma_req.dst =  vmio_req[0]->req.cmd.iovs + sge_index + 1;
	/* Set destination len */
	dst_len = dma_req.data_len;
	for (i = 0; dst_len > 0; i++) {
		if (dma_req.dst[i].iov_len < dst_len) {
			dst_len -= dma_req.dst[i].iov_len;
		} else {
			dma_req.dst[i].iov_len = dst_len;
			dma_req.dst_num = i + 1;
			break;
		}
	}
	dma_req.flr_seq = vmio_req[0]->flr_seq;
	dma_req.gfunc_id = vmio_req[0]->gfunc_id;

	result = vio_do_dma_async(tid, &dma_req);
	if (result != UDAA_SUCCESS) {
		SPDK_ERRLOG("vio_do_dma_sync failed: %d\n", result);
		*out_len = -1;
		result = UDAA_ERROR_IO_FAILED;
		goto complete;
	}
	memcpy(&fsmsession->iov_header, iov[0].iov_base, sizeof(fsmsession->iov_header));
	return result;


complete:
	if (fsmsession->src_iov) {
		free(fsmsession->src_iov);
		fsmsession->src_iov = NULL;
	}
	ssam_task_stat_tick(&fsmsession->fs_stat.complete_end_tsc);
	vio_vmio_complete(tid, vmio_req[0], iov[0].iov_base, sge_index, fsmsession);

	if (*out_len != -1) {
		*out_len = dma_req.data_len + iov[0].iov_len;
	}
	return result;
}

static udaa_error_t udaa_eml_queue_empty_complete(int depth_idx, struct udaa_emlq *emlq,
		uint16_t tid,
		struct spdk_ssam_fs_session *fsmsession)
{
	int res;
	struct ssam_request **vmio_req = &(emlq->vmio_req[depth_idx]);

	ssam_task_stat_tick(&fsmsession->fs_stat.complete_end_tsc);
	res = vio_vmio_empty_complete(tid, vmio_req[0], fsmsession);
	if (res != 0) {
		SPDK_ERRLOG("queue_id %d, vio_vmio_empty_complete failed: %d\n", tid, res);
		return UDAA_ERROR_IO_FAILED;
	}
	return UDAA_SUCCESS;
}

static ssize_t fuse_udaa_read(int fd, void *buf, size_t buf_len, void *userdata)
{
	udaa_error_t result;
	struct lo_data *p_lo = (struct lo_data *)userdata;
	ssize_t in_len = 0;
	int depth_idx = 0;
	struct udaa_eml_req *eml_req = NULL;
	struct udaa_emlq *emlq = NULL;
	int is_blocking = 1;
	unsigned lcore_id = rte_lcore_id();
	if (lcore_id >= SSAM_FS_LCORE_ID_MAX) {
		SPDK_ERRLOG("lcore_id is out of range. lcore_id: %d\n", lcore_id);
		return -1;
	}
	struct spdk_ssam_fs_session *fsmsession = (struct spdk_ssam_fs_session *)p_lo->smsession[lcore_id];

	eml_req = &p_lo->fs_reqs[lcore_id];
	eml_req->buf = buf;
	eml_req->buf_len = buf_len;
	emlq = p_lo->udaa_fs_queues[lcore_id];

	result = udaa_eml_queue_progress_retrieve(emlq, eml_req, depth_idx, &in_len, is_blocking,
			fsmsession->smsession.smdev->tid, fsmsession);
	if (result != UDAA_SUCCESS) {
		if (result == UDAA_ERROR_AGAIN) {
			return INT_MAX;
		}
		SPDK_ERRLOG("Failed to retrieve data from emulation queue. udaa_error value: %d\n", result);
	}

	return in_len;
}

static ssize_t fuse_udaa_writev(int fd, struct iovec *iov, int count, void *userdata)
{
	udaa_error_t result = UDAA_SUCCESS;
	int depth_idx = 0;
	ssize_t out_len = 0;
	struct lo_data *p_lo = (struct lo_data *)userdata;
	struct udaa_eml_req *eml_req = NULL;
	struct udaa_emlq *emlq = NULL;
	unsigned lcore_id;
	lcore_id = rte_lcore_id();
	if (lcore_id >= SSAM_FS_LCORE_ID_MAX) {
		SPDK_ERRLOG("lcore_id is out of range. lcore_id: %d\n", lcore_id);
		return -1;
	}
	struct spdk_ssam_fs_session *fsmsession = (struct spdk_ssam_fs_session *)p_lo->smsession[lcore_id];

	eml_req = &p_lo->fs_reqs[lcore_id];
	emlq = p_lo->udaa_fs_queues[lcore_id];

	if (spdk_likely(p_lo->have_shm == false)) {
		result = udaa_eml_queue_progress_response(emlq, eml_req, iov, count, depth_idx, &out_len,
				fsmsession->smsession.smdev->tid, fsmsession);
	} else {
		p_lo->have_shm = false;
	}

	if (result != UDAA_SUCCESS) {
		SPDK_ERRLOG("Failed to respond to the emulation queue. udaa_error value: %d\n", result);
	}

	return out_len;
}

static udaa_error_t create_fs_device(struct udaa_emlq **emlq, uint32_t num_queues,
				     uint16_t gfunc_id)
{
	udaa_error_t result;
	uint16_t queue_id = ssam_get_queue_id(gfunc_id);

	/* Create the EmlQ */
	for (uint32_t i = 0; i < num_queues; i++) {
		result = udaa_eml_queue_create(1, &emlq[i], queue_id);
		if (result != UDAA_SUCCESS) {
			SPDK_ERRLOG("Unable to create emulation queue: %d\n", result);
			return result;
		}
	}

	return UDAA_SUCCESS;
}

static udaa_error_t udaa_eml_queues_destroy(struct udaa_emlq **emlqs, int num_queues)
{
	if (emlqs == NULL) {
		return UDAA_SUCCESS;
	}

	for (int i = 0; i < num_queues; i++) {
		if (emlqs[i]) {
			if (emlqs[i]->vmio_req) {
				free(emlqs[i]->vmio_req);
			}
			free(emlqs[i]);
		}
	}
	free(emlqs);

	return UDAA_SUCCESS;
}

static void
ssam_fs_remove_self(struct spdk_ssam_session *smsession)
{
	struct spdk_ssam_fs_session *fsmsession = (struct spdk_ssam_fs_session *)smsession;
	struct lo_data *p_lo = fsmsession->p_lo;

	pthread_mutex_lock(&p_lo->exit_mutex);
	p_lo->exit_num++;
	if (p_lo->exit_num == p_lo->num_queues) {
		fuse_session_reset(p_lo->se);
		fuse_session_destroy(p_lo->se);
		udaa_eml_queues_destroy(p_lo->udaa_fs_queues, SSAM_FS_LCORE_ID_MAX);
		if (p_lo->fs_reqs != NULL) {
			free(p_lo->fs_reqs);
		}
		if (p_lo->source != NULL) {
			free(p_lo->source);
		}
		if (p_lo->name != NULL) {
			free(p_lo->name);
		}
		memset(p_lo, 0, sizeof(*p_lo));
	}
	pthread_mutex_unlock(&p_lo->exit_mutex);

	if (fsmsession->fs_poller != NULL) {
		spdk_poller_unregister(&fsmsession->fs_poller);
		fsmsession->fs_poller = NULL;
	}

	if (smsession->name != NULL) {
		free(smsession->name);
		smsession->name = NULL;
	}

	ssam_free_fs_session(fsmsession);
	return;
}

static void
ssam_fs_dump_info_json(struct spdk_ssam_session *smsession, struct spdk_json_write_ctx *w)
{
	struct spdk_ssam_fs_session *fsmsession = (struct spdk_ssam_fs_session *)smsession;

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "name", ssam_session_get_name(smsession));
	spdk_json_write_named_uint32(w, "function_id", (uint32_t)smsession->gfunc_id);
	spdk_json_write_named_uint32(w, "queues", (uint32_t)smsession->max_queues);
	spdk_json_write_named_string(w, "dbdf", fsmsession->p_lo->dbdf);
	spdk_json_write_named_uint32(w, "max_threads", fsmsession->p_lo->num_queues);
	spdk_json_write_object_end(w);
}

static void
ssam_fs_clear_iostat_json(struct spdk_ssam_session *smsession)
{
	struct spdk_ssam_fs_session *fsmsession = (struct spdk_ssam_fs_session *)smsession;
	memset(&fsmsession->fs_stat, 0, sizeof(struct ssam_fs_stat));
}

static void
ssam_fs_show_iostat_json(struct spdk_ssam_session *smsession, uint32_t id,
			 struct spdk_json_write_ctx *w)
{
	struct spdk_ssam_fs_session *fsmsession = (struct spdk_ssam_fs_session *)smsession;

	struct ssam_fs_stat fs_stat;

	memcpy(&fs_stat, &fsmsession->fs_stat, sizeof(struct ssam_fs_stat));

	spdk_json_write_object_begin(w);
	spdk_json_write_named_uint32(w, "function_id", (uint32_t)smsession->gfunc_id);
	spdk_json_write_named_string(w, "name", ssam_session_get_name(smsession));
	spdk_json_write_named_string(w, "ctrlr", ssam_dev_get_name(smsession->smdev));
	spdk_json_write_named_string_fmt(w, "cpumask", "0x%s",
					 spdk_cpuset_fmt(spdk_thread_get_cpumask(smsession->smdev->thread)));
	spdk_json_write_named_string(w, "dbdf", fsmsession->p_lo->dbdf);
	spdk_json_write_named_uint64(w, "complete_read_ios", fs_stat.complete_read_ios);
	spdk_json_write_named_uint64(w, "err_read_ios", fs_stat.err_read_ios);
	spdk_json_write_named_uint64(w, "complete_write_ios", fs_stat.complete_write_ios);
	spdk_json_write_named_uint64(w, "err_write_ios", fs_stat.err_write_ios);
	spdk_json_write_named_uint64(w, "flush_ios", fs_stat.flush_ios);
	spdk_json_write_named_uint64(w, "complete_flush_ios", fs_stat.complete_flush_ios);
	spdk_json_write_named_uint64(w, "err_flush_ios", fs_stat.err_flush_ios);

	spdk_json_write_named_uint64(w, "other_ios", fs_stat.other_ios);
	spdk_json_write_named_uint64(w, "bytes_read", fs_stat.bytes_read);
	spdk_json_write_named_uint64(w, "num_read_ops", fs_stat.num_read_ops);
	spdk_json_write_named_uint64(w, "bytes_written", fs_stat.bytes_written);
	spdk_json_write_named_uint64(w, "num_write_ops", fs_stat.num_write_ops);
	spdk_json_write_named_uint64(w, "read_latency_ticks", fs_stat.read_latency_ticks);
	spdk_json_write_named_uint64(w, "write_latency_ticks", fs_stat.write_latency_ticks);
	spdk_json_write_named_uint64(w, "fatal_ios", fs_stat.fatal_ios);
	spdk_json_write_object_end(w);
}

static void
ssam_fs_write_config_json(struct spdk_ssam_session *smsession, struct spdk_json_write_ctx *w)
{
	struct spdk_ssam_fs_session *fsmsession = (struct spdk_ssam_fs_session *)smsession;

	if (fsmsession == NULL || fsmsession->p_lo == NULL || fsmsession->need_write_config != true) {
		return;
	}

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "method", "fs_controller_create");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "dbdf", fsmsession->p_lo->dbdf);
	spdk_json_write_named_string(w, "fs_name", fsmsession->p_lo->source);
	spdk_json_write_named_string(w, "name", fsmsession->p_lo->name);
	spdk_json_write_named_uint32(w, "max_threads", fsmsession->p_lo->num_queues);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

static void
ssam_free_fs_session(struct spdk_ssam_fs_session *fsmsession)
{
	if (fsmsession->fbuf.mem != NULL) {
		spdk_free(fsmsession->fbuf.mem);
		fsmsession->fbuf.mem = NULL;
	}

	if (fsmsession->smsession.name != NULL) {
		free(fsmsession->smsession.name);
		fsmsession->smsession.name = NULL;
	}

	if (fsmsession->dynamic_buf != NULL) {
		ssam_mempool_free(fsmsession->smsession.mp, fsmsession->dynamic_buf);
		fsmsession->dynamic_buf = NULL;
	}

	if (fsmsession->static_buf != NULL) {
		spdk_free(fsmsession->static_buf);
		fsmsession->static_buf = NULL;
	}

	ssam_sessions_remove(fsmsession->smsession.smdev->smsessions, &fsmsession->smsession);
	fsmsession->smsession.smdev->active_session_num--;
	fsmsession->smsession.smdev = NULL;

	memset(fsmsession, 0, sizeof(*fsmsession));
	free(fsmsession);
	fsmsession = NULL;
}

static void
ssam_free_lo_data(struct lo_data *p_lo)
{
	for (int i = 0; i < SSAM_FS_LCORE_ID_MAX; i++) {
		if (p_lo->smsession[i] != NULL) {
			ssam_free_fs_session((struct spdk_ssam_fs_session *)p_lo->smsession[i]);
			p_lo->smsession[i] = NULL;
		}
	}
	udaa_eml_queues_destroy(p_lo->udaa_fs_queues, SSAM_FS_LCORE_ID_MAX);
	if (p_lo->fs_reqs != NULL) {
		free(p_lo->fs_reqs);
	}
	if (p_lo->source != NULL) {
		free(p_lo->source);
	}
	if (p_lo->name != NULL) {
		free(p_lo->name);
	}
	if (p_lo->dbdf != NULL) {
		free(p_lo->dbdf);
	}
	memset(p_lo, 0, sizeof(*p_lo));
}

static void
ssam_free_fuse_session(struct lo_data *p_lo, struct spdk_ssam_fs_session *fsmsession)
{
	pthread_mutex_lock(&p_lo->exit_mutex);
	p_lo->exit_num++;
	if (p_lo->exit_num < p_lo->num_queues) {
		if (fsmsession->fs_poller != NULL) {
			spdk_poller_unregister(&fsmsession->fs_poller);
			fsmsession->fs_poller = NULL;
		}
		pthread_mutex_unlock(&p_lo->exit_mutex);
		return;
	}
	pthread_mutex_unlock(&p_lo->exit_mutex);
	SPDK_NOTICELOG("fs controller %u is removed\n", fsmsession->smsession.gfunc_id);
	if (fsmsession->fs_poller != NULL) {
		spdk_poller_unregister(&fsmsession->fs_poller);
		fsmsession->fs_poller = NULL;
	}

	fuse_session_reset(p_lo->se);
	fuse_session_destroy(p_lo->se);

	if (p_lo->rsp_fn != NULL) {
		p_lo->rsp_fn(p_lo->rsp_ctx, 0);
		p_lo->rsp_fn = NULL;
	}

	ssam_free_lo_data(p_lo);

	return;
}

static void
ssam_fs_response_worker(struct spdk_ssam_session *smsession, void *arg)
{
	struct ssam_dma_rsp *dma_rsp = (struct ssam_dma_rsp *)arg;
	struct spdk_ssam_dma_cb *dma_cb = (struct spdk_ssam_dma_cb *)&dma_rsp->cb;
	struct spdk_ssam_fs_session *fsmsession = (struct spdk_ssam_fs_session *)smsession;
	unsigned lcore_id = rte_lcore_id();
	struct udaa_emlq *emlq = fsmsession->p_lo->udaa_fs_queues[lcore_id];
	struct ssam_request **vmio_req = &(emlq->vmio_req[0]);

	if (dma_rsp->status != 0) {
		SPDK_ERRLOG("Response error status %d\n", dma_rsp->status);
		return;
	}

	if (dma_cb->req_dir == 0) { /* read */
		fsmsession->fbuf.size = fsmsession->in_len;
		fuse_session_process_buf(fsmsession->p_lo->se, &fsmsession->fbuf);
	} else { /* write */
		if (fsmsession->src_iov) {
			free(fsmsession->src_iov);
			fsmsession->src_iov = NULL;
		}
		ssam_task_stat_tick(&fsmsession->fs_stat.complete_end_tsc);
		vio_vmio_complete(smsession->smdev->tid, vmio_req[0], &fsmsession->iov_header,
				  vmio_req[0]->req.cmd.writable,
				  fsmsession);
		if (fsmsession->dynamic_buf != NULL) {
			ssam_mempool_free(fsmsession->smsession.mp, fsmsession->dynamic_buf);
			fsmsession->dynamic_buf = NULL;
		}
	}

	return;
}

static int
ssam_fuse_session_loop(void *arg)
{
	struct spdk_ssam_session *smsession = (struct spdk_ssam_session *)arg;
	struct spdk_ssam_fs_session *fsmsession = (struct spdk_ssam_fs_session *)smsession;
	struct fuse_session *se = fsmsession->p_lo->se;
	struct lo_data *p_lo = (struct lo_data *)se->userdata;
	int res = 0;
	struct fuse_buf *fbuf = &fsmsession->fbuf;

	if (fsmsession->fbuf_used == true) {
		return SPDK_POLLER_BUSY;
	}

	if (!p_lo->delete_flag) {
		res = fuse_session_receive_buf(se, fbuf);
		if (res == -EINTR || res == INT_MAX) {
			return SPDK_POLLER_BUSY;
		}
		ssam_fuse_share_memory(fsmsession);
		if (res > 0) {
			fuse_session_process_buf(se, fbuf);
			return SPDK_POLLER_BUSY;
		}
	}

	ssam_free_fuse_session(p_lo, fsmsession);
	return SPDK_POLLER_BUSY;
}

static void
ssam_fs_read_shm_mem(struct spdk_ssam_fs_session *fsmsession)
{
	struct lo_data *p_lo = (struct lo_data *)fsmsession->p_lo;
	struct fuse_session *se = p_lo->se;
	char name[SHM_NAME] = {0};
	int shm_fd = 0;
	struct mount_info *info = NULL;
	struct fuse_buf *fb = NULL;
	struct fuse_in_header *in = NULL;
	void *inarg = NULL;
	struct fuse_init_in *arg = NULL;

	snprintf(name, sizeof(name), "shm_name%d", fsmsession->smsession.gfunc_id);

	shm_fd = shm_open(name, O_RDWR, 0600);
	if (shm_fd < 0 || p_lo->have_shm == true) {
		close(shm_fd);
		return;
	}

	if (ftruncate(shm_fd, SHM_SIZE) != 0) {
		close(shm_fd);
		SPDK_ERRLOG("could not truncate %s\n", name);
		return;
	}

	info = mmap(NULL, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
	if (info == MAP_FAILED) {
		close(shm_fd);
		SPDK_ERRLOG("can not read mmap");
		return;
	}

	fb = (struct fuse_buf *)malloc(sizeof(struct fuse_buf));
	if (fb == NULL) {
		munmap(info, SHM_SIZE);
		close(shm_fd);
		SPDK_ERRLOG("can not alloc memory");
		return;
	}

	fb->mem = malloc(se->bufsize);
	if (fb->mem == NULL) {
		munmap(info, SHM_SIZE);
		close(shm_fd);
		SPDK_ERRLOG("can not alloc memory");
		return;
	}

	in = fb->mem;
	inarg = (void *) &in[1];
	arg = (struct fuse_init_in *) inarg;

	fb->fd = info->fd;
	fb->flags = info->flags;
	fb->pos = info->pos;
	fb->size = info->size;
	in->opcode = info->opcode;
	in->padding = info->padding;
	in->gid = info->gid;
	in->len = info->len;
	in->pid = info->pid;
	in->total_extlen = info->total_extlen;
	in->uid = info->uid;
	in->unique = info->unique;
	in->nodeid = info->nodeid;
	munmap(info, SHM_SIZE);
	close(shm_fd);
	arg->major = MEM_MAJOR;
	arg->minor = MEM_MINOR;
	p_lo->have_shm = true;

	fuse_session_process_buf(se, fb);
	SPDK_NOTICELOG("success reset mount\n");

	free(fb->mem);
	free(fb);
}

static int
ssam_fs_reactor_loop_start(struct spdk_ssam_session *smsession, void **unused)
{
	struct spdk_ssam_fs_session *fsmsession = (struct spdk_ssam_fs_session *)smsession;
	struct lo_data *p_lo = (struct lo_data *)fsmsession->p_lo;

	pthread_mutex_lock(&p_lo->mutex);
	if (p_lo->load_shm_flag == false) {
		ssam_fs_read_shm_mem(fsmsession);
		p_lo->load_shm_flag = true;
	}
	pthread_mutex_unlock(&p_lo->mutex);

	if (fsmsession->fs_poller == NULL) {
		fsmsession->fs_poller = SPDK_POLLER_REGISTER(ssam_fuse_session_loop, smsession, 0);
	}

	return 0;
}

static int
ssam_init_fs_session_reg_info(struct spdk_ssam_session_reg_info *reg_info,
			      struct ssam_fs_construct_info *info)
{
	snprintf(reg_info->type_name, SPDK_SESSION_TYPE_MAX_LEN, "%s", SPDK_SESSION_TYPE_FS);
	reg_info->backend = &g_ssam_fs_session_backend;
	reg_info->session_ctx_size = sizeof(struct spdk_ssam_fs_session) - sizeof(struct spdk_ssam_session);
	reg_info->gfunc_id = info->gfunc_id;
	reg_info->queues = ssam_get_queues();
	if (reg_info->queues > SPDK_SSAM_MAX_VQUEUES) {
		SPDK_ERRLOG("Queue number out of range, need less or equal than %u, actually %u.\n",
			    SPDK_SSAM_MAX_VQUEUES, reg_info->queues);
		return -ERANGE;
	}
	reg_info->name = info->name;
	return 0;
}

static int
ssam_create_fs_session(struct ssam_fs_construct_info *info, struct lo_data *lo)
{
	int ret;
	struct spdk_ssam_session_reg_info reg_info;
	struct spdk_ssam_session *smsession = NULL;
	struct spdk_ssam_fs_session *fsmsession = NULL;
	uint16_t max_threads = 0;
	uint32_t thread_mask;

	ret = ssam_init_fs_session_reg_info(&reg_info, info);
	if (ret != 0) {
		SPDK_ERRLOG("failed to init fs session reg info\n");
		return ret;
	}

	thread_mask = ssam_get_tids(info->max_threads);
	for (int i = 0; i < SSAM_FS_LCORE_ID_MAX; i++) {
		if (thread_mask & (1 << i)) {
			reg_info.tid = i;
			ret = ssam_session_register(&reg_info, &smsession);
			if (ret != 0) {
				SPDK_ERRLOG("failed to register session\n");
				return ret;
			}
			fsmsession = (struct spdk_ssam_fs_session *)smsession;
			fsmsession->p_lo = lo;
			fsmsession->fbuf.mem = NULL;
			fsmsession->fbuf_used = false;
			fsmsession->dynamic_buf = NULL;
			fsmsession->need_write_config = (max_threads == 0);
			fsmsession->static_buf = spdk_zmalloc(SSAM_FS_STATIC_BUF_SIZE, SSAM_FS_DEFAULT_ALIGN, NULL,
							      SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
			if (!fsmsession->static_buf) {
				SPDK_ERRLOG("malloc_buf spdk_zmalloc() failed\n");
				return -ENOMEM;
			}
			ssam_session_start_done(smsession, 0);
			lo->smsession[smsession->smdev->lcore_id] = smsession;
			max_threads++;
		}
	}

	info->max_threads = max_threads;
	return 0;
}

static int
ssam_init_lo_data(struct ssam_fs_construct_info *info, struct lo_data *lo)
{
	struct stat stat;
	char source[PATH_MAX] = {0};
	int ret;

	pthread_mutex_init(&lo->mutex, NULL);
	pthread_mutex_init(&lo->exit_mutex, NULL);
	lo->root.next = lo->root.prev = &lo->root;
	lo->root.fd = -1;
	lo->cache = CACHE_NORMAL;
	lo->gfunc_id = info->gfunc_id;
	lo->exit_num = 0;
	lo->mounted = false;
	lo->cache = CACHE_NORMAL;
	lo->root.refcount = SSAM_FS_REF_COUNT;
	lo->num_queues = info->max_threads;
	lo->used = true;
	lo->flr_seq = UINT32_MAX;
	lo->have_shm = false;
	lo->load_shm_flag = false;
	lo->delete_flag = false;
	lo->rsp_fn = NULL;
	lo->rsp_ctx = NULL;

	lo->name = spdk_sprintf_alloc("%s", info->name);
	if (lo->name == NULL) {
		SPDK_ERRLOG("snprintf cotroller name failed\n");
		return -ENOMEM;
	}

	lo->dbdf = spdk_sprintf_alloc("%s", info->dbdf);
	if (lo->dbdf == NULL) {
		SPDK_ERRLOG("snprintf cotroller dbdf failed\n");
		return -ENOMEM;
	}

	if (realpath(info->fs_name, source) == NULL) {
		SPDK_ERRLOG("Failed to execute the realpath function.\n");
		return -EINVAL;
	}

	lo->source = spdk_sprintf_alloc("%s", source);
	if (lo->source == NULL) {
		SPDK_ERRLOG("snprintf cotroller source failed\n");
		return -ENOMEM;
	}

	if (lo->source) {
		ret = lstat(lo->source, &stat);
		if (ret == -1) {
			SPDK_ERRLOG("failed to stat source (\"%s\")\n", lo->source);
			return ret;
		}
		if (!S_ISDIR(stat.st_mode)) {
			SPDK_ERRLOG("source is not a directory\n");
			return -ENOTDIR;
		}
	} else {
		lo->source = strdup("/");
		if (!lo->source) {
			SPDK_ERRLOG("fuse: memory allocation failed\n");
			return -ENOMEM;
		}
	}

	if (!lo->timeout_set) {
		switch (lo->cache) {
		case CACHE_NEVER:
			lo->timeout = 0.0;
			break;
		case CACHE_NORMAL:
			lo->timeout = 1.0;
			break;
		case CACHE_ALWAYS:
			lo->timeout = 86400.0;
			break;
		}
	} else if (lo->timeout < 0) {
		SPDK_ERRLOG("timeout is negative (%lf)\n", lo->timeout);
		return -EINVAL;
	}

	lo->root.fd = open(lo->source, O_PATH);
	if (lo->root.fd == -1) {
		SPDK_ERRLOG("open fs path failed\n");
		return -EINVAL;
	}

	lo->udaa_fs_queues = (struct udaa_emlq **)calloc(SSAM_FS_LCORE_ID_MAX, sizeof(struct udaa_emlq *));
	if (lo->udaa_fs_queues == NULL) {
		SPDK_ERRLOG("failed to alloc udaa_fs_queues\n");
		return -ENOMEM;
	}

	lo->fs_reqs = (struct udaa_eml_req *)calloc(SSAM_FS_LCORE_ID_MAX, sizeof(struct udaa_eml_req));
	if (lo->fs_reqs == NULL) {
		SPDK_ERRLOG("failed to alloc fs_reqs\n");
		return -ENOMEM;
	}

	if (create_fs_device(lo->udaa_fs_queues, SSAM_FS_LCORE_ID_MAX, info->gfunc_id) != UDAA_SUCCESS) {
		return -ENOMEM;
	}
	return 0;
}

static int
ssam_check_contrller_info(struct ssam_fs_construct_info *info)
{
	if (info->gfunc_id >= SSAM_HOSTEP_NUM_MAX) {
		SPDK_ERRLOG("gfunc_id %u out of range\n", info->gfunc_id);
		return -ERANGE;
	}
	if (lo_map[info->gfunc_id].used == true) {
		SPDK_ERRLOG("fs controller %u already exists\n", info->gfunc_id);
		return -EEXIST;
	}
	return 0;
}

static int
ssam_dev_io_scan_poller(void *pf_poller_ctx)
{
	int polled_num = 0;
	int tid = 0;
	int opcode = 0;
	uint8_t func_id = ((struct spdk_ssam_dev_io_scan_poller_ctx *)pf_poller_ctx)->func_id;
	bool restart_flag = ((struct spdk_ssam_dev_io_scan_poller_ctx *)pf_poller_ctx)->restart_flag;
	int queue_id = ssam_get_queue_id(func_id);
	struct ssam_io_response resp;
	struct ssam_virtio_res *virtio_res = (struct ssam_virtio_res *)&resp.data;
	char buffer[SSAM_FS_BUF_LEN];
	struct iovec ext;
	ext.iov_base = buffer;
	ext.iov_len = SSAM_FS_BUF_LEN;
	struct iovec iov;
	struct ssam_request *io_req[1] = { 0 };
	struct ssam_request_poll_opt poll_opt = {
		.sge1_iov = &ext,
		.queue_id = queue_id,
	};
	struct fuse_release_in *arg = (struct fuse_release_in *)buffer;
	struct lo_dirp *d = NULL;
	int fd;

	pthread_mutex_lock(&g_ssam_fs_poller_ctx.poll_mutex[func_id]);
	polled_num = ssam_request_poll_ext(tid, 1, io_req, &poll_opt);
	pthread_mutex_unlock(&g_ssam_fs_poller_ctx.poll_mutex[func_id]);
	if (io_req[0] != NULL && io_req[0]->type != SSAM_VIRTIO_FS_IO) {
		SPDK_ERRLOG(" get illegal type, io_req[0]->type is %d, io_req[0]->gfunc_id is %d, io_req[0]->status is %d\n",
			    io_req[0]->type, io_req[0]->gfunc_id, io_req[0]->status);
		return SPDK_POLLER_BUSY;
	}
	if (polled_num <= 0) {
		return SPDK_POLLER_BUSY;
	}
	opcode = ((struct fuse_in_header *)io_req[0]->req.cmd.header)->opcode;
	struct fuse_out_header rsp = {
		.len = sizeof(struct fuse_out_header),
		.error = opcode != SSAM_FUSE_OPCODE_DESTROY ? -ENODEV : 0,
		.unique = 0,
	};

	if (restart_flag != true) {
		if (opcode == SSAM_FUSE_OPCODE_RELEASE) {
			fd = arg->fh;
			close(fd);
		} else if (opcode == SSAM_FUSE_OPCODE_RELEASEDIR) {
			d = (struct lo_dirp *)(uintptr_t)arg->fh;
			closedir(d->dp);
			free(d);
		}
	}

	memset(&resp, 0, sizeof(resp));
	resp.gfunc_id = io_req[0]->gfunc_id;
	resp.iocb_id = io_req[0]->iocb_id;
	resp.flr_seq = io_req[0]->flr_seq;
	resp.status = io_req[0]->status;
	resp.req = io_req[0];

	memcpy(&iov, &io_req[0]->req.cmd.iovs[io_req[0]->req.cmd.writable],
	       sizeof(io_req[0]->req.cmd.iovs[io_req[0]->req.cmd.writable]));
	virtio_res->iovs = &iov;
	virtio_res->iovcnt = 1;
	virtio_res->rsp = &rsp;
	virtio_res->rsp_len = sizeof(struct fuse_out_header);
	ssam_io_complete(0, &resp);

	return SPDK_POLLER_BUSY;
}

int
ssam_fs_construct(struct ssam_fs_construct_info *info)
{
	struct fuse_session *se = NULL;
	struct fuse_cmdline_opts opts = { 0 };
	int ret = -1;
	struct fuse_custom_io vio_io = {
		.read = fuse_udaa_read,
		.writev = fuse_udaa_writev
	};
	struct lo_data *lo = &lo_map[info->gfunc_id];
	struct spdk_ssam_send_event_flag send_event_flag = {
		.need_async = false,
		.need_rsp = false,
	};
	struct spdk_ssam_fs_session *fsmsession = NULL;

	ssam_lock();

	ret = ssam_check_contrller_info(info);
	if (ret != 0) {
		ssam_unlock();
		return ret;
	}

	ret = ssam_create_fs_session(info, lo);
	if (ret != 0) {
		goto err_out1;
	}

	/* ssam -s -f */
	char *argv[SSAM_FUSE_ARGS_NUM] = { "ssam", "-s", "-f" };
	struct fuse_args args = FUSE_ARGS_INIT(SSAM_FUSE_ARGS_NUM, argv);

	ret = fuse_parse_cmdline(&args, &opts);
	if (ret != 0) {
		goto err_out1;
	}

	ret = ssam_init_lo_data(info, lo);
	if (ret != 0) {
		goto err_out1;
	}

	se = fuse_session_new(&args, &lo_oper, sizeof(lo_oper), lo);
	if (se == NULL) {
		SPDK_ERRLOG("fuse_session_new failed\n");
		ret = -1;
		goto err_out1;
	}

	ret = fuse_session_custom_io(se, &vio_io, lo->root.fd);
	if (ret != 0) {
		goto err_out2;
	}

	ssam_update_virtio_device_used(info->gfunc_id, 1);

	if (g_ssam_fs_poller_ctx.pf_poller[info->gfunc_id] != NULL) {
		spdk_poller_unregister(&g_ssam_fs_poller_ctx.pf_poller[info->gfunc_id]);
		g_ssam_fs_poller_ctx.pf_poller[info->gfunc_id] = NULL;
	}
	fuse_daemonize(opts.foreground);
	se->mountpoint = lo->source;
	lo->se = se;

	for (int i = 0; i < SSAM_FS_LCORE_ID_MAX; i++) {
		if (lo->smsession[i] != NULL) {
			fsmsession = (struct spdk_ssam_fs_session *)lo->smsession[i];
			fsmsession->fbuf.mem = spdk_zmalloc(se->bufsize, SSAM_FS_DEFAULT_ALIGN, NULL, SPDK_ENV_LCORE_ID_ANY,
							    SPDK_MALLOC_DMA);
			if (fsmsession->fbuf.mem == NULL) {
				SPDK_ERRLOG("fbuf malloc failed\n");
				ret = -ENOMEM;
				goto err_out2;
			}
			ssam_send_event_to_session(lo->smsession[i], ssam_fs_reactor_loop_start, NULL, send_event_flag,
						   NULL);
		}
	}

	SPDK_NOTICELOG("fs controller %u is created by %u threads\n", info->gfunc_id, info->max_threads);
	ssam_unlock();
	return 0;

err_out2:
	fuse_session_destroy(se);
err_out1:
	ssam_free_lo_data(lo);
	ssam_unlock();
	return ret;
}

int
ssam_fs_destory(char *name, bool force, void *request,
		spdk_ssam_session_rsp_fn rpc_ssam_send_response_cb)
{
	char shm_name[SHM_NAME] = {0};

	for (int i = 0; i < SSAM_HOSTEP_NUM_MAX; i++) {
		if (lo_map[i].name != NULL && strcmp(lo_map[i].name, name) == 0) {
			if (lo_map[i].mounted == true && force != true) {
				SPDK_ERRLOG("fs controller %u is busy\n", lo_map[i].gfunc_id);
				return -EBUSY;
			}
			SPDK_NOTICELOG("fs controller %u removing, the force flag is %d\n", lo_map[i].gfunc_id, force);
			snprintf(shm_name, sizeof(shm_name), "shm_name%d", lo_map[i].gfunc_id);
			shm_unlink(shm_name);
			lo_map[i].delete_flag = true;
			lo_map[i].rsp_ctx = request;
			lo_map[i].rsp_fn = rpc_ssam_send_response_cb;
			g_ssam_fs_poller_ctx.pf_poller_ctx[lo_map[i].gfunc_id].restart_flag = false;
			g_ssam_fs_poller_ctx.pf_poller[lo_map[i].gfunc_id] = SPDK_POLLER_REGISTER(ssam_dev_io_scan_poller,
					&g_ssam_fs_poller_ctx.pf_poller_ctx[lo_map[i].gfunc_id], 0);
			ssam_update_virtio_device_used(lo_map[i].gfunc_id, 0);
			return 0;
		}
	}

	return -ENODEV;
}

static int ssam_fs_flr_poller(void *flr_map)
{
	int i;
	uint8_t *flr_map_p = NULL;

	flr_map_p = (uint8_t *)flr_map;
	for (i = 0; i < SSAM_HOSTEP_NUM_MAX; i++) {
		if (lo_map[i].used == true && lo_map[i].flr_seq != *flr_map_p && lo_map[i].flr_seq != UINT32_MAX) {
			if (lo_map[i].mounted == true && lo_map[i].se != NULL) {
				lo_destroy((void *)&lo_map[i]);
			}
			lo_map[i].flr_seq = *flr_map_p;
		}
		flr_map_p++;
	}

	return SPDK_POLLER_BUSY;
}

int spdk_ssam_fs_poller_init(void)
{
	int i;

	if (ssam_get_virtio_fs_enable() == false) {
		return 0;
	}

	for (i = 0; i < SSAM_HOSTEP_NUM_MAX; i++) {
		pthread_mutex_init(&g_ssam_fs_poller_ctx.poll_mutex[i], NULL);
	}

	g_ssam_fs_poller_ctx.flr_fd = open(SSAM_FS_FLR_SEQ_PATH, O_RDONLY);
	if (g_ssam_fs_poller_ctx.flr_fd == -1) {
		return -1;
	}

	g_ssam_fs_poller_ctx.flr_map = mmap(NULL, SSAM_HOSTEP_NUM_MAX * sizeof(uint8_t), PROT_READ,
					    MAP_SHARED,
					    g_ssam_fs_poller_ctx.flr_fd, 0);
	if (g_ssam_fs_poller_ctx.flr_map == MAP_FAILED) {
		close(g_ssam_fs_poller_ctx.flr_fd);
		return -1;
	}

	if (g_ssam_fs_poller_ctx.flr_seq_poller == NULL) {
		g_ssam_fs_poller_ctx.flr_seq_poller = SPDK_POLLER_REGISTER(ssam_fs_flr_poller,
						      g_ssam_fs_poller_ctx.flr_map,
						      SSAM_FS_FLR_POLLER_PERIOD);
	}

	for (int i = 0; i < SSAM_HOSTEP_NUM_MAX; i++) {
		if (ssam_get_virtio_type(i) != SSAM_DEVICE_VIRTIO_FS) {
			continue;
		}
		g_ssam_fs_poller_ctx.pf_poller_ctx[i].func_id = i;
		g_ssam_fs_poller_ctx.pf_poller_ctx[i].restart_flag = true;
		g_ssam_fs_poller_ctx.pf_poller[i] = SPDK_POLLER_REGISTER(ssam_dev_io_scan_poller,
						    &g_ssam_fs_poller_ctx.pf_poller_ctx[i], 0);
	}

	return 0;
}

void spdk_ssam_fs_poller_destroy(void)
{
	if (g_ssam_fs_poller_ctx.flr_seq_poller != NULL) {
		spdk_poller_unregister(&g_ssam_fs_poller_ctx.flr_seq_poller);
		g_ssam_fs_poller_ctx.flr_seq_poller = NULL;
	}

	if (g_ssam_fs_poller_ctx.flr_map != MAP_FAILED) {
		munmap(g_ssam_fs_poller_ctx.flr_map, SSAM_HOSTEP_NUM_MAX * sizeof(uint8_t));
	}

	if (g_ssam_fs_poller_ctx.flr_fd >= 0) {
		close(g_ssam_fs_poller_ctx.flr_fd);
	}

	for (int i = 0; i < SSAM_HOSTEP_NUM_MAX; i++) {
		if (ssam_get_virtio_type(i) != SSAM_DEVICE_VIRTIO_FS) {
			continue;
		}
		if (g_ssam_fs_poller_ctx.pf_poller[i] != NULL) {
			spdk_poller_unregister(&g_ssam_fs_poller_ctx.pf_poller[i]);
			g_ssam_fs_poller_ctx.pf_poller[i] = NULL;
		}
	}
}

SPDK_LOG_REGISTER_COMPONENT(ssam_fs)
