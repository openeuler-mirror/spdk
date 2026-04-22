/* -
 * GNU LGPLv2 LICENSE
 *
 * Copyright (c) Huawei Technologies Co., Ltd. 2021-2022. All rights reserved.
 * This file contains code segment derived form libfuse
 * Original copyright notice:
 *   Copyright (C) 2001-2007 Miklos Szeredi <miklos@szeredi.hu> 
 *   This program can be distributed under the terms of the GNU LGPLv2.
 */

#include "spdk/env.h"

#include "ssam_fs_internal.h"

static size_t pagesize;

static __attribute__((constructor)) void ssam_fuse_ll_init_pagesize(void)
{
	pagesize = getpagesize();
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

static void convert_stat(const struct stat *stbuf, struct fuse_attr *attr)
{
	attr->ino = stbuf->st_ino;
	attr->mode = stbuf->st_mode;
	attr->nlink = stbuf->st_nlink;
	attr->uid = stbuf->st_uid;
	attr->gid = stbuf->st_gid;
	attr->rdev = stbuf->st_rdev;
	attr->size = stbuf->st_size;
	attr->blksize = stbuf->st_blksize;
	attr->blocks = stbuf->st_blocks;
	attr->atime = stbuf->st_atime;
	attr->mtime = stbuf->st_mtime;
	attr->ctime = stbuf->st_ctime;
	attr->atimensec = ST_ATIM_NSEC(stbuf);
	attr->mtimensec = ST_MTIM_NSEC(stbuf);
	attr->ctimensec = ST_CTIM_NSEC(stbuf);
}

static unsigned long calc_timeout_sec(double t)
{
	if (t > (double)ULONG_MAX) {
		return ULONG_MAX;
	} else if (t < 0.0) {
		return 0;
	} else {
		return (unsigned long)t;
	}
}

static unsigned int calc_timeout_nsec(double t)
{
	double f = t - (double)calc_timeout_sec(t);
	if (f < 0.0) {
		return 0;
	} else if (f >= 0.999999999) {
		return 999999999;
	} else {
		return (unsigned int)(f * 1.0e9);
	}
}

static void fill_entry(struct fuse_entry_out *arg, const struct fuse_entry_param *e)
{
	arg->nodeid = e->ino;
	arg->generation = e->generation;
	arg->entry_valid = calc_timeout_sec(e->entry_timeout);
	arg->entry_valid_nsec = calc_timeout_nsec(e->entry_timeout);
	arg->attr_valid = calc_timeout_sec(e->attr_timeout);
	arg->attr_valid_nsec = calc_timeout_nsec(e->attr_timeout);
	convert_stat(&e->attr, &arg->attr);
}

static void fill_open(struct fuse_open_out *arg, const struct fuse_file_info *f)
{
	arg->fh = f->fh;
	if (f->direct_io) {
		arg->open_flags |= FOPEN_DIRECT_IO;
	}
	if (f->keep_cache) {
		arg->open_flags |= FOPEN_KEEP_CACHE;
	}
	if (f->cache_readdir) {
		arg->open_flags |= FOPEN_CACHE_DIR;
	}
	if (f->nonseekable) {
		arg->open_flags |= FOPEN_NONSEEKABLE;
	}
	if (f->noflush) {
		arg->open_flags |= FOPEN_NOFLUSH;
	}
	if (f->parallel_direct_writes) {
		arg->open_flags |= FOPEN_PARALLEL_DIRECT_WRITES;
	}
}

static void list_del_req(struct fuse_req *req)
{
	struct fuse_req *prev = req->prev;
	struct fuse_req *next = req->next;
	prev->next = next;
	next->prev = prev;
}

static void ssam_fuse_chan_put(struct fuse_chan *ch)
{
	if (ch == NULL) {
		return;
	}
	pthread_mutex_lock(&ch->lock);
	ch->ctr--;
	if (!ch->ctr) {
		pthread_mutex_unlock(&ch->lock);
		close(ch->fd);
		pthread_mutex_destroy(&ch->lock);
		free(ch);
	} else {
		pthread_mutex_unlock(&ch->lock);
	}
}

static void destroy_req(fuse_req_t req)
{
	assert(req->ch == NULL);
	pthread_mutex_destroy(&req->lock);
	free(req);
}

static void ssam_fuse_free_req(fuse_req_t req)
{
	int ctr;
	struct fuse_session *se = req->se;

	pthread_mutex_lock(&se->lock);
	req->u.ni.func = NULL;
	req->u.ni.data = NULL;
	list_del_req(req);
	ctr = --req->ctr;
	ssam_fuse_chan_put(req->ch);
	req->ch = NULL;
	pthread_mutex_unlock(&se->lock);
	if (!ctr) {
		destroy_req(req);
	}
}

/* Send data. If *ch* is NULL, send via session master fd */
static int ssam_fuse_send_msg(struct fuse_session *se, struct fuse_chan *ch, struct iovec *iov,
			      int count)
{
	struct fuse_out_header *out = iov[0].iov_base;

	assert(se != NULL);
	out->len = iov_length(iov, count);
	if (se->debug) {
		if (out->unique == 0) {
			SPDK_INFOLOG(ssam_fs, "NOTIFY: code=%d length=%u\n", out->error, out->len);
		} else if (out->error) {
			SPDK_INFOLOG(ssam_fs, "unique: %llu, error: %i (%s), outsize: %i\n",
				     (unsigned long long)out->unique,
				     out->error, strerror(-out->error), out->len);
		} else {
			SPDK_INFOLOG(ssam_fs, "unique: %llu, success, outsize: %i\n", (unsigned long long)out->unique,
				     out->len);
		}
	}

	ssize_t res;
	if (se->io != NULL) {
		/* se->io->writev is never NULL if se->io is not NULL as
		specified by fuse_session_custom_io() */
		res = se->io->writev(ch ? ch->fd : se->fd, iov, count, se->userdata);
	} else {
		res = writev(ch ? ch->fd : se->fd, iov, count);
	}

	int err = errno;

	if (res == -1) {
		/* ENOENT means the operation was interrupted */
		if (!fuse_session_exited(se) && err != ENOENT) {
			SPDK_ERRLOG("fuse: writing device");
		}
		return -err;
	}

	return 0;
}

static int ssam_fuse_send_reply_iov_nofree(fuse_req_t req, int error, struct iovec *iov, int count)
{
	struct fuse_out_header out;

#if __GLIBC__ >= 2 && __GLIBC_MINOR__ >= 32
	const char *str = strerrordesc_np(error * -1);
	if ((str == NULL && error != 0) || error > 0) {
#else
	if (error <= -1000 || error > 0) {
#endif
		SPDK_ERRLOG("fuse: bad error value: %i\n", error);
		error = -ERANGE;
	}

	out.unique = req->unique;
	out.error = error;

	iov[0].iov_base = &out;
	iov[0].iov_len = sizeof(struct fuse_out_header);

	return ssam_fuse_send_msg(req->se, req->ch, iov, count);
}

static int ssam_fuse_send_reply_iov(fuse_req_t req, int error, struct iovec *iov, int count)
{
	int res;

	res = ssam_fuse_send_reply_iov_nofree(req, error, iov, count);
	ssam_fuse_free_req(req);
	return res;
}

static int ssam_fuse_send_reply(fuse_req_t req, int error, const void *arg, size_t argsize)
{
	struct iovec iov[2];
	int count = 1;
	if (argsize) {
		iov[1].iov_base = (void *)arg;
		iov[1].iov_len = argsize;
		count++;
	}
	return ssam_fuse_send_reply_iov(req, error, iov, count);
}

static int ssam_fuse_send_reply_ok(fuse_req_t req, const void *arg, size_t argsize)
{
	return ssam_fuse_send_reply(req, 0, arg, argsize);
}

int ssam_fuse_reply_entry(fuse_req_t req, const struct fuse_entry_param *e,
			  struct spdk_ssam_fs_session *fsmsession)
{
	struct fuse_entry_out *arg = (struct fuse_entry_out *)fsmsession->static_buf;
	size_t size = req->se->conn.proto_minor < 9 ? SSAM_FUSE_COMPAT_ENTRY_OUT_SIZE : sizeof(*arg);

	/* before ABI 7.4 e->ino == 0 was invalid, only ENOENT meant
	   negative entry */
	if (!e->ino && req->se->conn.proto_minor < 4) {
		return fuse_reply_err(req, ENOENT);
	}

	memset(arg, 0, sizeof(*arg));
	fill_entry(arg, e);
	return ssam_fuse_send_reply_ok(req, arg, size);
}

int ssam_fuse_reply_open(fuse_req_t req, const struct fuse_file_info *f,
			 struct spdk_ssam_fs_session *fsmsession)
{
	struct fuse_open_out *arg = (struct fuse_open_out *)fsmsession->static_buf;

	memset(arg, 0, sizeof(*arg));
	fill_open(arg, f);
	return ssam_fuse_send_reply_ok(req, arg, sizeof(*arg));
}

int ssam_fuse_reply_create(fuse_req_t req, const struct fuse_entry_param *e,
			   const struct fuse_file_info *f,
			   struct spdk_ssam_fs_session *fsmsession)
{
	size_t entrysize = req->se->conn.proto_minor < 9 ? SSAM_FUSE_COMPAT_ENTRY_OUT_SIZE : sizeof(
				   struct fuse_entry_out);
	struct fuse_entry_out *earg = (struct fuse_entry_out *) fsmsession->static_buf;
	struct fuse_open_out *oarg = (struct fuse_open_out *)(fsmsession->static_buf + entrysize);

	memset(fsmsession->static_buf, 0, sizeof(struct fuse_entry_out) + sizeof(struct fuse_open_out));
	fill_entry(earg, e);
	fill_open(oarg, f);
	return ssam_fuse_send_reply_ok(req, fsmsession->static_buf,
				       entrysize + sizeof(struct fuse_open_out));
}

int ssam_fuse_reply_attr(fuse_req_t req, const struct stat *attr, double attr_timeout,
			 struct spdk_ssam_fs_session *fsmsession)
{
	struct fuse_attr_out *arg = (struct fuse_attr_out *)fsmsession->static_buf;
	size_t size = req->se->conn.proto_minor < 9 ? SSAM_FUSE_COMPAT_ATTR_OUT_SIZE : sizeof(*arg);

	memset(arg, 0, sizeof(*arg));
	arg->attr_valid = calc_timeout_sec(attr_timeout);
	arg->attr_valid_nsec = calc_timeout_nsec(attr_timeout);
	convert_stat(attr, &arg->attr);

	return ssam_fuse_send_reply_ok(req, arg, size);
}

int ssam_fuse_reply_write(fuse_req_t req, size_t count, struct spdk_ssam_fs_session *fsmsession)
{
	struct fuse_write_out *arg = (struct fuse_write_out *)fsmsession->static_buf;

	memset(arg, 0, sizeof(*arg));
	arg->size = count;

	return ssam_fuse_send_reply_ok(req, arg, sizeof(*arg));
}

static void convert_statfs(const struct statvfs *stbuf, struct fuse_kstatfs *kstatfs)
{
	kstatfs->bsize = stbuf->f_bsize;
	kstatfs->frsize = stbuf->f_frsize;
	kstatfs->blocks = stbuf->f_blocks;
	kstatfs->bfree = stbuf->f_bfree;
	kstatfs->bavail = stbuf->f_bavail;
	kstatfs->files = stbuf->f_files;
	kstatfs->ffree = stbuf->f_ffree;
	kstatfs->namelen = stbuf->f_namemax;
}

int ssam_fuse_reply_statfs(fuse_req_t req, const struct statvfs *stbuf,
			   struct spdk_ssam_fs_session *fsmsession)
{
	struct fuse_statfs_out *arg = (struct fuse_statfs_out *)fsmsession->static_buf;
	size_t size = req->se->conn.proto_minor < 4 ? SSAM_FUSE_COMPAT_STATFS_SIZE : sizeof(*arg);

	memset(arg, 0, sizeof(*arg));
	convert_statfs(stbuf, &arg->st);

	return ssam_fuse_send_reply_ok(req, arg, size);
}

int ssam_fuse_reply_xattr(fuse_req_t req, size_t count, struct spdk_ssam_fs_session *fsmsession)
{
	struct fuse_getxattr_out *arg = (struct fuse_getxattr_out *)fsmsession->static_buf;

	memset(arg, 0, sizeof(*arg));
	arg->size = count;

	return ssam_fuse_send_reply_ok(req, arg, sizeof(*arg));
}

int ssam_fuse_reply_lseek(fuse_req_t req, off_t off, struct spdk_ssam_fs_session *fsmsession)
{
	struct fuse_lseek_out *arg = (struct fuse_lseek_out *)fsmsession->static_buf;

	memset(arg, 0, sizeof(*arg));
	arg->offset = off;

	return ssam_fuse_send_reply_ok(req, arg, sizeof(*arg));
}

static int ssam_fuse_send_data_iov_fallback(struct fuse_session *se, struct fuse_chan *ch,
		struct iovec *iov,
		int iov_count, struct fuse_bufvec *buf, size_t len, struct spdk_ssam_fs_session *fsmsession)
{
	struct fuse_bufvec mem_buf = FUSE_BUFVEC_INIT(len);
	int res;
	uint64_t phys_addr;

	/* Optimize common case */
	if (buf->count == 1 && buf->idx == 0 && buf->off == 0 && !(buf->buf[0].flags & (1 << 1))) {
		/* FIXME: also avoid memory copy if there are multiple buffers
		   but none of them contain an fd */

		iov[iov_count].iov_base = buf->buf[0].mem;
		iov[iov_count].iov_len = len;
		iov_count++;
		return ssam_fuse_send_msg(se, ch, iov, iov_count);
	}

	fsmsession->dynamic_buf = ssam_mempool_alloc(fsmsession->smsession.mp, len, &phys_addr);
	if (!fsmsession->dynamic_buf) {
		return -ENOMEM;
	}

	mem_buf.buf[0].mem = fsmsession->dynamic_buf;
	res = fuse_buf_copy(&mem_buf, buf, 0);
	if (res < 0) {
		ssam_mempool_free(fsmsession->smsession.mp, fsmsession->dynamic_buf);
		fsmsession->dynamic_buf = NULL;
		return -res;
	}
	len = res;

	iov[iov_count].iov_base = fsmsession->dynamic_buf;
	iov[iov_count].iov_len = len;
	iov_count++;
	res = ssam_fuse_send_msg(se, ch, iov, iov_count);

	return res;
}

static int ssam_fuse_send_data_iov(struct fuse_session *se, struct fuse_chan *ch, struct iovec *iov,
				   int iov_count,
				   struct fuse_bufvec *buf, unsigned int flags, struct spdk_ssam_fs_session *fsmsession)
{
	size_t len = fuse_buf_size(buf);
	(void)flags;

	return ssam_fuse_send_data_iov_fallback(se, ch, iov, iov_count, buf, len, fsmsession);
}

int ssam_fuse_reply_data(fuse_req_t req, struct fuse_bufvec *bufv, enum fuse_buf_copy_flags flags,
			 struct spdk_ssam_fs_session *fsmsession)
{
	struct iovec iov[2];
	struct fuse_out_header out;
	int res;

	iov[0].iov_base = &out;
	iov[0].iov_len = sizeof(struct fuse_out_header);

	out.unique = req->unique;
	out.error = 0;

	res = ssam_fuse_send_data_iov(req->se, req->ch, iov, 1, bufv, flags, fsmsession);
	if (res <= 0) {
		ssam_fuse_free_req(req);
		return res;
	} else {
		return fuse_reply_err(req, res);
	}
}
