/* -
 * GNU GPL LICENSE
 * Copyright (c) Huawei Technologies Co., Ltd. 2021-2022. All rights reserved.
 * This file contains code segment derived form libfuse
 * Original copyright notice:
 *   Copyright (C) 2001-2007 Miklos Szeredi <miklos@szeredi.hu> 
 *   This program can be distributed under the terms of the GNU GPL.
 */
#include <linux/virtio_fs.h>

#include "spdk/env.h"
#include "spdk/string.h"
#include "spdk/thread.h"
#include "spdk/ssam.h"
#include "spdk/likely.h"
#include "spdk_internal/fuse_dispatcher.h"

#include "ssam_config.h"
#include "ssam_fs_internal.h"

#define SESSION_STOP_POLLER_PERIOD      1000

struct ssam_fs_add_fsdev_ctx {
	struct ssam_fsdev_object *fsdev_obj;
	ssam_fs_add_fsdev_cpl_cb cb;
	void *cb_arg;
	uint16_t max_threads;
	struct spdk_ssam_send_event_flag send_event_flag;
};

struct lo_dirp {
	DIR *dp;
	struct dirent *entry;
	off_t offset;
};

struct ssam_fs_session_ctx {
	struct spdk_ssam_fs_session *fsmsession;
	void **user_ctx;
};

struct ssam_shm_ctx {
	struct ssam_fsdev_object *fsdev_obj;
	struct fuse_in_header *in;
	struct fuse_init_in *arg;
	struct fuse_out_header *rsp;
	struct fuse_init_out *out;
	char *in_buf;
	char *out_buf;
	struct iovec in_iov[2];
	struct iovec out_iov[2];
};

static void ssam_fs_dump_info_json(struct spdk_ssam_session *smsession,
				   struct spdk_json_write_ctx *w);
static void ssam_fs_response_worker(struct spdk_ssam_session *smsession, void *arg);
static void ssam_fs_write_config_json(struct spdk_ssam_session *smsession,
				      struct spdk_json_write_ctx *w);
static void ssam_fs_show_iostat_json(struct spdk_ssam_session *smsession, uint32_t id,
				     struct spdk_json_write_ctx *w);
static void ssam_fs_clear_iostat_json(struct spdk_ssam_session *smsession);

static int ssam_fs_reactor_loop_start(struct spdk_ssam_session *smsession, void **unused);
static void ssam_fs_fuse_disp_delete(void *cb_arg);
static void ssam_fs_fuse_req_done(void *cb_arg, int error);
static void ssam_free_data_session(struct ssam_fsdev_object *fsdev_obj, struct spdk_ssam_fs_session *fsmsession);

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
	.remove_self = NULL,
};

static struct spdk_ssam_fs_poller_ctx g_ssam_fs_poller_ctx = { 0 };
static struct ssam_fsdev_object fsdev_map[SSAM_HOSTEP_NUM_MAX] = { 0 };

static struct spdk_ssam_fs_session *ssam_to_fs_session(struct spdk_ssam_session *smsession)
{
	return (struct spdk_ssam_fs_session *)smsession;
}

static void
ssam_fs_fuse_dispatcher_delete_cpl(void *cb_arg, int error)
{
	struct ssam_fsdev_object *fsdev_obj = cb_arg;

	if (error) {
		SPDK_ERRLOG("ssam FUSE dispatcher deletion failed with %d. Retrying...\n", error);
		spdk_thread_send_msg(spdk_get_thread(), ssam_fs_fuse_disp_delete, fsdev_obj);
	} else {
		fsdev_obj->fuse_disp = NULL;
		SPDK_DEBUGLOG(ssam_fs, "FUSE dispatcher deleted.\n");
	}
}

static void
ssam_fs_fuse_disp_delete(void *cb_arg)
{
	struct ssam_fsdev_object *fsdev_obj = cb_arg;
	int ret;

	SPDK_DEBUGLOG(ssam_fs, "ssam initiating %s FUSE dispatcher deletion...\n",
		spdk_fuse_dispatcher_get_fsdev_name(fsdev_obj->fuse_disp));

	ret = spdk_fuse_dispatcher_delete(fsdev_obj->fuse_disp, ssam_fs_fuse_dispatcher_delete_cpl,
		fsdev_obj);
	if (ret) {
		SPDK_ERRLOG("ssam %s FUSE dispatcher deletion failed with %d. Retrying...\n",
			spdk_fuse_dispatcher_get_fsdev_name(fsdev_obj->fuse_disp), ret);
		spdk_thread_send_msg(spdk_get_thread(), ssam_fs_fuse_disp_delete, fsdev_obj);
	}
}

static void 
ssam_fs_stop_io_channel(void *ctx)
{
	struct spdk_ssam_session *smsession = ctx;
	struct spdk_ssam_fs_session *fsmsession = ssam_to_fs_session(smsession);

	spdk_put_io_channel(fsmsession->io_channel);
	fsmsession->io_channel = NULL;

	SPDK_DEBUGLOG(ssam_fs, "ssam fs smsession: %s io_channel is release.\n", smsession->name);
}

static void
ssam_fs_event_cb(struct ssam_fsdev_object *fsdev_obj)
{
	if (fsdev_obj->fuse_disp) {
		spdk_thread_send_msg(fsdev_obj->init_thread, ssam_fs_fuse_disp_delete, fsdev_obj);
	}

	for (int i = 0; i < SSAM_FS_LCORE_ID_MAX; i++) {
		struct spdk_ssam_session *smsession = fsdev_obj->smsession[i];
		if (smsession != NULL) {
			SPDK_NOTICELOG("session :%s removing.\n", smsession->name);

			struct spdk_ssam_fs_session *fsmsession = (struct spdk_ssam_fs_session *)smsession;
			if (fsmsession->io_channel) {
				spdk_thread_send_msg(smsession->thread, ssam_fs_stop_io_channel, smsession);
			}
		}
	}

	fsdev_obj->delete_flag = true;
}

static void
ssam_fuse_disp_event_cb(enum spdk_fuse_dispatcher_event_type type,
	struct spdk_fuse_dispatcher *disp, void *event_ctx)
{
	struct ssam_fsdev_object *fsdev_obj = (struct ssam_fsdev_object *)event_ctx;

	SPDK_DEBUGLOG(ssam_fs, "ssam FUSE dispatcher event#%d arrived.\n", type);

	switch (type) {
		case SPDK_FUSE_DISP_EVENT_FSDEV_REMOVE:
			SPDK_NOTICELOG("ssam received SPDK_FUSE_DISP_EVENT_FSDEV_REMOVE.\n");
			ssam_fs_event_cb(fsdev_obj);

			break;
		default:
			SPDK_NOTICELOG("ssam virtio-fs unsupported event type %d.\n", type);
			break;
	}
}

static void
ssam_fuse_dispatcher_create_cpl(void *arg, struct spdk_fuse_dispatcher *disp)
{
	struct ssam_fs_add_fsdev_ctx *ctx = arg;

	if (!disp) {
		SPDK_ERRLOG("Failed to create ssam FUSE dispatcher.\n");
		ctx->cb(ctx->cb_arg, -EINVAL);
		free(ctx);
		return;
	}

	SPDK_DEBUGLOG(ssam_fs, "ssam FUSE dispatcher created successfully.\n");

	ctx->fsdev_obj->fuse_disp = disp;

	for (int i = 0; i < SSAM_FS_LCORE_ID_MAX; i++) {
		if (ctx->fsdev_obj->smsession[i] != NULL) {
			ssam_send_event_to_session(ctx->fsdev_obj->smsession[i], ssam_fs_reactor_loop_start, NULL, ctx->send_event_flag,
				NULL);
		}
	}

	ctx->cb(ctx->cb_arg, 0); /* rpc cmd callback */
	SPDK_NOTICELOG("fs controller gfunc_id: %u is created by %u threads.\n", ctx->fsdev_obj->gfunc_id, ctx->max_threads);

	free(ctx);
}

static inline void set_read_dynamic_len(struct spdk_ssam_fs_session *fsmsession, size_t *dynamic_len) {
	struct fuse_read_in *arg = (struct fuse_read_in *)fsmsession->in_iov[1].iov_base;

	if (arg->size) {
		*dynamic_len = arg->size;
		fsmsession->out_iovcnt = 2;
	} else {
		fsmsession->out_iovcnt = 1;
	}
}

static inline void set_getxattr_dynamic_len(struct spdk_ssam_fs_session *fsmsession, size_t *dynamic_len) {
	struct fuse_getxattr_in *arg = (struct fuse_getxattr_in *)fsmsession->in_iov[1].iov_base;

	if (arg->size) {
		*dynamic_len = arg->size;
	} else {
		*dynamic_len = sizeof(struct fuse_getxattr_out);
	}

	fsmsession->out_iovcnt = 2;
}

static void
ssam_out_iov_data_construct(struct spdk_ssam_fs_session *fsmsession)
{
	struct fuse_in_header *in = (struct fuse_in_header *)fsmsession->in_buf;
	size_t dynamic_len = 0;

	switch (in->opcode) {
		case FUSE_LOOKUP:
			dynamic_len = sizeof(struct fuse_entry_out);
			fsmsession->out_iovcnt = 2;
			break;
			
		case FUSE_FORGET:
			fsmsession->out_iovcnt = 1;
			break;
			
		case FUSE_GETATTR:
		case FUSE_SETATTR:
			dynamic_len = sizeof(struct fuse_attr_out);
			fsmsession->out_iovcnt = 2;
			break;
			
		case FUSE_READLINK:
			dynamic_len = PATH_MAX;
			fsmsession->out_iovcnt = 2;
			break;
			
		case FUSE_SYMLINK:
		case FUSE_MKNOD:
		case FUSE_MKDIR:
			dynamic_len = sizeof(struct fuse_entry_out);
			fsmsession->out_iovcnt = 2;
			break;

		case FUSE_UNLINK:
		case FUSE_RMDIR:
		case FUSE_RENAME:
			fsmsession->out_iovcnt = 1;
			break;

		case FUSE_LINK:
		case FUSE_OPEN:
			dynamic_len = sizeof(struct fuse_open_out);
			fsmsession->out_iovcnt = 2;
			break;

		case FUSE_READ:
			set_read_dynamic_len(fsmsession, &dynamic_len);
			break;
			
		case FUSE_WRITE:
			dynamic_len = sizeof(struct fuse_write_out);
			fsmsession->out_iovcnt = 2;
			break;
			
		case FUSE_STATFS:
			dynamic_len = sizeof(struct fuse_statfs_out);
			fsmsession->out_iovcnt = 2;
			break;

		case FUSE_RELEASE:
		case FUSE_FSYNC:
		case FUSE_SETXATTR:
			fsmsession->out_iovcnt = 1;
			break;

		case FUSE_GETXATTR:
		case FUSE_LISTXATTR:
			set_getxattr_dynamic_len(fsmsession, &dynamic_len);
			break;

		case FUSE_REMOVEXATTR:
		case FUSE_FLUSH:
			fsmsession->out_iovcnt = 1;
			break;

		case FUSE_INIT:
			dynamic_len = sizeof(struct fuse_init_out);
			fsmsession->out_iovcnt = 2;
			break;

		case FUSE_OPENDIR:
			dynamic_len = sizeof(struct fuse_open_out);
			fsmsession->out_iovcnt = 2;
			break;

		case FUSE_READDIR:
			set_read_dynamic_len(fsmsession, &dynamic_len);
			break;
			
		case FUSE_RELEASEDIR:
		case FUSE_FSYNCDIR:
		case FUSE_GETLK: /* not supported */
		case FUSE_SETLK:
		case FUSE_SETLKW:
			fsmsession->out_iovcnt = 1;
			break;

		case FUSE_CREATE:
			/* The 'proto_minor' attribute affects the size of the return value. */
			dynamic_len = sizeof(struct fuse_entry_out) + sizeof(struct fuse_open_out);
			fsmsession->out_iovcnt = 2;
			break;
		
		case FUSE_INTERRUPT:
		case FUSE_BMAP: /* not supported */
		case FUSE_DESTROY:
		case FUSE_IOCTL: /* not supported */
		case FUSE_POLL: /* not supported */
		case FUSE_NOTIFY_REPLY:
		case FUSE_BATCH_FORGET:
		case FUSE_FALLOCATE:
			fsmsession->out_iovcnt = 1;
			break;

		case FUSE_READDIRPLUS:
			set_read_dynamic_len(fsmsession, &dynamic_len);
			break;	

		case FUSE_RENAME2:
			fsmsession->out_iovcnt = 1;
			break;

		case FUSE_COPY_FILE_RANGE:
			dynamic_len = sizeof(struct fuse_write_out);
			fsmsession->out_iovcnt = 2;
			break;

		case FUSE_SETUPMAPPING: /* not supported */
		case FUSE_REMOVEMAPPING: /* not supported */
		case FUSE_SYNCFS: /* not supported */
			fsmsession->out_iovcnt = 1;
			break;

		case CUSE_INIT:
			dynamic_len = sizeof(struct cuse_init_out);
			fsmsession->out_iovcnt = 2;
			break;

		default:
			SPDK_ERRLOG("invalid optype in data construct. opcode = %d.\n", in->opcode);
			break;
	}

	/* The reason for the judgment on dynamic_len is that the number of output IOVs
	   such as READDIRPLUS opcode may be either 1 or 2. */
	if (fsmsession->out_iovcnt > 1 && dynamic_len != 0) {
		uint64_t phys_addr;

		fsmsession->dynamic_buf = ssam_mempool_alloc(fsmsession->smsession.mp, dynamic_len, &phys_addr);
		if (!fsmsession->dynamic_buf) {
			SPDK_ERRLOG("ssam get mempool alloc failed, opcode = %d.\n", in->opcode);
		}

		fsmsession->out_iov[1].iov_base = fsmsession->dynamic_buf;
		fsmsession->out_iov[1].iov_len = dynamic_len;
	}
}

static void ssam_fuse_dispatcher_process(struct spdk_ssam_fs_session *fsmsession)
{
	fsmsession->out_iov[0].iov_base = (void *)&fsmsession->iov_header;
	fsmsession->out_iov[0].iov_len = sizeof(fsmsession->iov_header);

	ssam_out_iov_data_construct(fsmsession);

	spdk_fuse_dispatcher_submit_request(fsmsession->fsdev_obj->fuse_disp, fsmsession->io_channel,
		fsmsession->in_iov, fsmsession->in_iovcnt, fsmsession->out_iov, fsmsession->out_iovcnt, ssam_fs_fuse_req_done, fsmsession);
}

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
		free(eml_queue);
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

	return UDAA_DMA_AGAIN;
}

static void
ssam_fuse_share_memory(struct spdk_ssam_fs_session *fsmsession)
{
	struct fuse_in_header *in = (struct fuse_in_header *)fsmsession->in_buf;
	char name[SHM_NAME] = {0};
	int shm_fd = 0;
	struct mount_info *info = NULL;

	if (in->opcode == SSAM_FUSE_OPCODE_INIT) {
		snprintf(name, sizeof(name), "shm_name%d", fsmsession->fsdev_obj->gfunc_id);
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
		snprintf(name, sizeof(name), "shm_name%d", fsmsession->fsdev_obj->gfunc_id);
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

	fsmsession->in_buf_used = false;
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

	fsmsession->in_buf_used = false;
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
		struct ssam_fsdev_object *fsdev_obj)
{
	struct ssam_request_poll_opt poll_opt = {
		.sge1_iov = ext,
		.queue_id = queue_id,
	};
	pthread_mutex_lock(&g_ssam_fs_poller_ctx.poll_mutex[fsdev_obj->gfunc_id]);
	*polled_num = ssam_request_poll_ext(tid, poll_num, vmio_req, &poll_opt);
	pthread_mutex_unlock(&g_ssam_fs_poller_ctx.poll_mutex[fsdev_obj->gfunc_id]);
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
		struct ssam_fsdev_object *fsdev_obj)
{
	struct ssam_request_poll_opt poll_opt = {
		.sge1_iov = ext,
		.queue_id = queue_id,
	};
	pthread_mutex_lock(&g_ssam_fs_poller_ctx.poll_mutex[fsdev_obj->gfunc_id]);
	*polled_num = ssam_request_poll_ext(tid, poll_num, vmio_req, &poll_opt);
	pthread_mutex_unlock(&g_ssam_fs_poller_ctx.poll_mutex[fsdev_obj->gfunc_id]);
	if ((*polled_num) < 0) {
		*in_len = *polled_num;
		return UDAA_ERROR_IO_FAILED;
	}
	if ((*polled_num) == 0) {
		return UDAA_ERROR_AGAIN;
	}

	return UDAA_SUCCESS;
}

static void 
ssam_fsdev_destroy_cb(void *cb_arg, int error)
{
	if (error != 0) {
		SPDK_WARNLOG("ssam fsdev destroy call failed.\n");
	}
	
	struct iovec *iov = (struct iovec *)cb_arg;

	free(iov[0].iov_base);
	free(iov[1].iov_base);
	free(iov);
}

static void
fsdev_destroy(struct spdk_ssam_fs_session *fsmsession)
{
	struct ssam_fsdev_object *fsdev_obj = (struct ssam_fsdev_object *)fsmsession->fsdev_obj;
	char name[SHM_NAME] = {0};
	int shm_fd = 0;
	struct mount_info *info = NULL;
	struct fuse_in_header *in = NULL;
	struct iovec *iov = NULL;

	shm_fd = shm_open(name, O_RDWR, 0600);
	if (shm_fd < 0 || fsdev_obj->have_shm == true) {
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

	in = (struct fuse_in_header *)calloc(1, sizeof(struct fuse_in_header));
	if (in == NULL) {
		munmap(info, SHM_SIZE);
		close(shm_fd);
		SPDK_ERRLOG("can not alloc in memory");
		return;
	}
	
	iov = (struct iovec *)calloc(2, sizeof(struct iovec));
	if (iov == NULL) {
		munmap(info, SHM_SIZE);
		close(shm_fd);
		free(in);
		SPDK_ERRLOG("can not alloc iov memory");
		return;
	}

	in->opcode = FUSE_DESTROY;
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
	
	iov[0].iov_base = (void *)in;
	iov[0].iov_len = sizeof(struct fuse_in_header);
	iov[1].iov_base = (void *)calloc(1, sizeof(struct fuse_out_header));
	iov[1].iov_len = sizeof(struct fuse_out_header);

	spdk_fuse_dispatcher_submit_request(fsmsession->fsdev_obj->fuse_disp, fsmsession->io_channel,
		&iov[0], 1, &iov[1], 1, ssam_fsdev_destroy_cb, iov);
}

static udaa_error_t udaa_eml_queue_progress_retrieve(struct udaa_emlq *emlq,
		struct udaa_eml_req *eml_req,
		int depth_idx, ssize_t *in_len, int is_blocking, unsigned tid,
		struct spdk_ssam_fs_session *fsmsession, unsigned lcore_id)
{
	int polled_num = 0;
	uint16_t queue_id = emlq->queue_id, poll_num = 1;
	size_t iov0_len;
	struct iovec *ext = &fsmsession->in_iov[1]; /* For sge1 pre-fetch (extension) */
	struct ssam_request **vmio_req = &(emlq->vmio_req[depth_idx]);
	udaa_error_t result;
	void *buf = eml_req->buf;
	size_t buf_len = eml_req->buf_len;
	int skip_sges = 0;
	int hdr_len;
	fsmsession->in_iovcnt = 0;
	udaa_eml_type_t func_eml_type = emlq->eml_type;

	result = udaa_get_hdr_len(func_eml_type, &hdr_len);
	if (result != UDAA_SUCCESS) {
		SPDK_ERRLOG("Failed to get hdr_len. udaa_error value: %d\n", result);
		return result;
	}

	fsmsession->in_iov[0].iov_base = buf;
	fsmsession->in_iov[0].iov_len = hdr_len;

	ext->iov_base = (uint8_t *)buf + hdr_len;
	ext->iov_len = buf_len - hdr_len;

	if (is_blocking) {
		result = udaa_poll_batch_blocking(&polled_num, tid, poll_num, vmio_req, ext, queue_id, in_len,
						  fsmsession->fsdev_obj);
		if (result != UDAA_SUCCESS) {
			if (result != UDAA_ERROR_AGAIN && result != UDAA_DMA_AGAIN) {
				SPDK_ERRLOG("Failed to poll request polled_num = %d. udaa_error value: %d\n", polled_num, result);
			}
			return result;
		}
	} else {
		result = udaa_poll_batch_non_blocking(&polled_num, tid, poll_num, vmio_req, ext, queue_id, in_len,
						      fsmsession->fsdev_obj);
		if (result != UDAA_SUCCESS) {
			if (result != UDAA_ERROR_AGAIN && result != UDAA_DMA_AGAIN) {
				SPDK_ERRLOG("Failed to poll request polled_num = %d. udaa_error value: %d\n", polled_num, result);
			}
			return result;
		}
	}
	fsmsession->in_buf_used = true;
	fsmsession->smsession.smdev->io_num++;

	/* user can mount tag again when reboot host without umount */
	if (fsmsession->fsdev_obj->flr_seq != UINT32_MAX && fsmsession->fsdev_obj->flr_seq != vmio_req[0]->flr_seq &&
	    fsmsession->fsdev_obj->mounted == true) {
		fsdev_destroy(fsmsession);
	}
	fsmsession->fsdev_obj->flr_seq = vmio_req[0]->flr_seq;

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
	fsmsession->in_iovcnt++;
	buf = (uint8_t *)buf + iov0_len + ext->iov_len;

	/* Fill Emulation request's type and buffers */
	eml_req->type = (int)func_eml_type;

	/* Get the rest of the request */
	if (in_num > 1) {
		fsmsession->in_iovcnt++;
		if (ext->iov_len == 0) {
			/* Couldn't prefetch sge1, need to get it with DMA */
			skip_sges = 1;
		} else {
			skip_sges = 2;
		}
		in_num -= skip_sges;
		if (in_num > 0) {
			result = vio_build_request(tid, vmio_req[0], buf, in_num, *in_len - iov0_len - ext->iov_len,
						   skip_sges,
						   fsmsession);
			if (result != UDAA_ERROR_AGAIN && result != UDAA_DMA_AGAIN) {
				SPDK_ERRLOG("vio_build_request failed: %d\n", result);
				*in_len = -1;
				return UDAA_ERROR_IO_FAILED;
			}
			fsmsession->in_len = *in_len;
			fsmsession->in_iov[2].iov_base = buf;
			fsmsession->in_iov[2].iov_len = *in_len - iov0_len - ext->iov_len;
			fsmsession->in_iovcnt++;
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
	struct fuse_in_header *in = (struct fuse_in_header *)fsmsession->in_buf;

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
			memcpy(fsmsession->dynamic_buf, iov[i].iov_base, sizeof(struct fuse_init_out));
			fsmsession->src_iov[i - 1].iov_base = (void *)spdk_vtophys(fsmsession->dynamic_buf, NULL);
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
	dma_req.dst = vmio_req[0]->req.cmd.iovs + sge_index + 1;
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

	if (fsmsession->dynamic_buf != NULL) {
		ssam_mempool_free(fsmsession->smsession.mp, fsmsession->dynamic_buf);
		fsmsession->dynamic_buf = NULL;
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

static ssize_t fuse_udaa_read(struct spdk_ssam_fs_session *fsmsession, struct ssam_fsdev_object *fsdev_obj)
{
	udaa_error_t result;
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

	eml_req = &fsdev_obj->fs_reqs[lcore_id];
	eml_req->buf = fsmsession->in_buf;
	eml_req->buf_len = fsmsession->in_buf_size;
	emlq = fsdev_obj->udaa_fs_queues[lcore_id];

	result = udaa_eml_queue_progress_retrieve(emlq, eml_req, depth_idx, &in_len, is_blocking,
			fsmsession->smsession.smdev->tid, fsmsession, lcore_id);
	if (result != UDAA_SUCCESS) {
		if (result == UDAA_ERROR_AGAIN) {
			return -EAGAIN;
		}
		if (result == UDAA_DMA_AGAIN) {
			return INT_MAX;
		}
		SPDK_ERRLOG("Failed to retrieve data from emulation queue. udaa_error value: %d\n", result);
	}

	return in_len;
}

static ssize_t 
fuse_udaa_writev(struct iovec *iov, int count, struct spdk_ssam_fs_session *fsmsession, struct ssam_fsdev_object *fsdev_obj)
{
	udaa_error_t result = UDAA_SUCCESS;
	int depth_idx = 0;
	ssize_t out_len = 0;
	struct udaa_eml_req *eml_req = NULL;
	struct udaa_emlq *emlq = NULL;
	unsigned lcore_id;
	lcore_id = rte_lcore_id();
	if (lcore_id >= SSAM_FS_LCORE_ID_MAX) {
		SPDK_ERRLOG("lcore_id is out of range. lcore_id: %d\n", lcore_id);
		return -1;
	}

	eml_req = &fsdev_obj->fs_reqs[lcore_id];
	emlq = fsdev_obj->udaa_fs_queues[lcore_id];

	struct fuse_in_header *in = fsmsession->in_iov[0].iov_base;

	if (in->opcode == FUSE_FORGET || in->opcode == FUSE_BATCH_FORGET) {
		result = udaa_eml_queue_empty_complete(0, emlq, fsmsession->smsession.smdev->tid, fsmsession);
		goto writev_end;
	}

	result = udaa_eml_queue_progress_response(emlq, eml_req, iov, count, depth_idx, &out_len,
		fsmsession->smsession.smdev->tid, fsmsession);

writev_end:

	if (result != UDAA_SUCCESS) {
		SPDK_ERRLOG("Failed to respond to the emulation queue. udaa_error value: %d\n", result);
	}

	return out_len;
}

static void
ssam_fs_fuse_req_done(void *cb_arg, int error)
{
	struct spdk_ssam_fs_session *fsmsession = (struct spdk_ssam_fs_session *)cb_arg;
	struct fuse_in_header *in = (struct fuse_in_header *)fsmsession->in_iov[0].iov_base;
	struct fuse_out_header *out = (struct fuse_out_header *)fsmsession->out_iov[0].iov_base;

	if (error != 0) {
		fsmsession->out_iovcnt = 1;
		out->unique = in->unique;
		out->error = error;
		out->len = sizeof(struct fuse_out_header);
	} else {
		if (out->len != sizeof(struct fuse_out_header)) {
			fsmsession->out_iov[1].iov_len = out->len - sizeof(struct fuse_out_header);
		}
	}

	fuse_udaa_writev(fsmsession->out_iov, fsmsession->out_iovcnt, fsmsession, fsmsession->fsdev_obj);
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
ssam_free_fs_session(struct spdk_ssam_fs_session *fsmsession)
{
	if (fsmsession->in_buf != NULL) {
		spdk_free(fsmsession->in_buf);
		fsmsession->in_buf = NULL;
		fsmsession->in_buf_size = 0;
	}

	if (fsmsession->smsession.name != NULL) {
		free(fsmsession->smsession.name);
		fsmsession->smsession.name = NULL;
	}

	if (fsmsession->dynamic_buf != NULL) {
		ssam_mempool_free(fsmsession->smsession.mp, fsmsession->dynamic_buf);
		fsmsession->dynamic_buf = NULL;
	}

	ssam_sessions_remove(fsmsession->smsession.smdev->smsessions, &fsmsession->smsession);
	fsmsession->smsession.smdev->active_session_num--;
	fsmsession->smsession.smdev = NULL;

	memset(fsmsession, 0, sizeof(*fsmsession));
	free(fsmsession);
	fsmsession = NULL;
}

static void
ssam_fs_dump_info_json(struct spdk_ssam_session *smsession, struct spdk_json_write_ctx *w)
{
	struct spdk_ssam_fs_session *fsmsession = (struct spdk_ssam_fs_session *)smsession;

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "name", ssam_session_get_name(smsession));
	spdk_json_write_named_uint32(w, "function_id", (uint32_t)smsession->gfunc_id);
	spdk_json_write_named_uint32(w, "queues", (uint32_t)smsession->max_queues);
	spdk_json_write_named_string(w, "dbdf", fsmsession->fsdev_obj->dbdf);
	spdk_json_write_named_string(w, "fsdev", spdk_fuse_dispatcher_get_fsdev_name(fsmsession->fsdev_obj->fuse_disp));
	spdk_json_write_named_uint32(w, "max_threads", fsmsession->fsdev_obj->num_queues);
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
	spdk_json_write_named_string(w, "dbdf", fsmsession->fsdev_obj->dbdf);
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

	if (fsmsession == NULL || fsmsession->need_write_config != true) {
		return;
	}

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "method", "fs_controller_create");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "name", ssam_session_get_name(smsession));
	spdk_json_write_named_uint32(w, "function_id", (uint32_t)smsession->gfunc_id);
	spdk_json_write_named_string(w, "dbdf", fsmsession->dbdf);
	spdk_json_write_named_uint32(w, "queues", (uint32_t)smsession->max_queues);
	spdk_json_write_named_string(w, "fsdev", spdk_fuse_dispatcher_get_fsdev_name(fsmsession->fsdev_obj->fuse_disp));
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

static void
ssam_free_fsdev_object(struct ssam_fsdev_object *fsdev_obj)
{
	for (int i = 0; i < SSAM_FS_LCORE_ID_MAX; i++) {
		if (fsdev_obj->smsession[i] != NULL) {
			ssam_free_fs_session((struct spdk_ssam_fs_session *)fsdev_obj->smsession[i]);
			fsdev_obj->smsession[i] = NULL;
		}
	}
	udaa_eml_queues_destroy(fsdev_obj->udaa_fs_queues, SSAM_FS_LCORE_ID_MAX);
	if (fsdev_obj->fs_reqs != NULL) {
		free(fsdev_obj->fs_reqs);
	}
	if (fsdev_obj->name != NULL) {
		free(fsdev_obj->name);
	}
	if (fsdev_obj->dbdf != NULL) {
		free(fsdev_obj->dbdf);
	}
	memset(fsdev_obj, 0, sizeof(*fsdev_obj));
}

static void
ssam_free_data_session(struct ssam_fsdev_object *fsdev_obj, struct spdk_ssam_fs_session *fsmsession)
{
	/* Wating for the fuse dispatcher delete resource call back process finish first. */
	if (fsmsession->fsdev_obj->fuse_disp != NULL || fsmsession->io_channel != NULL) {
		return;
	}

	pthread_mutex_lock(&fsdev_obj->exit_mutex);
	fsdev_obj->exit_num++;
	if (fsdev_obj->exit_num < fsdev_obj->num_queues) {
		if (fsmsession->fs_poller != NULL) {
			spdk_poller_unregister(&fsmsession->fs_poller);
			fsmsession->fs_poller = NULL;
		}
		pthread_mutex_unlock(&fsdev_obj->exit_mutex);
		return;
	}
	pthread_mutex_unlock(&fsdev_obj->exit_mutex);
	SPDK_NOTICELOG("fs controller %u is removed.\n", fsmsession->smsession.gfunc_id);
	if (fsmsession->fs_poller != NULL) {
		spdk_poller_unregister(&fsmsession->fs_poller);
		fsmsession->fs_poller = NULL;
	}

	if (fsdev_obj->rsp_fn != NULL) {
		fsdev_obj->rsp_fn(fsdev_obj->rsp_ctx, 0);
		fsdev_obj->rsp_fn = NULL;
	}

	ssam_free_fsdev_object(fsdev_obj);

	return;
}

static void
ssam_fs_response_worker(struct spdk_ssam_session *smsession, void *arg)
{	
	struct ssam_dma_rsp *dma_rsp = (struct ssam_dma_rsp *)arg;
	struct spdk_ssam_dma_cb *dma_cb = (struct spdk_ssam_dma_cb *)&dma_rsp->cb;
	struct spdk_ssam_fs_session *fsmsession = (struct spdk_ssam_fs_session *)smsession;
	unsigned lcore_id = rte_lcore_id();
	struct udaa_emlq *emlq = fsmsession->fsdev_obj->udaa_fs_queues[lcore_id];
	struct ssam_request **vmio_req = &(emlq->vmio_req[0]);

	if (dma_rsp->status != 0) {
		SPDK_ERRLOG("Response error status %d.\n", dma_rsp->status);
		return;
	}

	if (dma_cb->req_dir == 0) {/* read */
		ssam_fuse_dispatcher_process(fsmsession);
	} else { /* write */
		if (fsmsession->src_iov) {
			free(fsmsession->src_iov);
			fsmsession->src_iov = NULL;
		}
		ssam_task_stat_tick(&fsmsession->fs_stat.complete_end_tsc);
		vio_vmio_complete(smsession->smdev->tid, vmio_req[0], &fsmsession->iov_header,
			vmio_req[0]->req.cmd.writable, fsmsession);
		if (fsmsession->dynamic_buf != NULL) {
			ssam_mempool_free(fsmsession->smsession.mp, fsmsession->dynamic_buf);
			fsmsession->dynamic_buf = NULL;
		}
	}

	return;
}

static int
ssam_fs_session_loop(void *arg)
{
	struct spdk_ssam_session *smsession = (struct spdk_ssam_session *)arg;
	struct spdk_ssam_fs_session *fsmsession = (struct spdk_ssam_fs_session *)smsession;
	struct ssam_fsdev_object *fsdev_obj = fsmsession->fsdev_obj;
	int res = 0;

	if (fsmsession->in_buf_used == true) {
		return SPDK_POLLER_IDLE;
	}

	if (!fsdev_obj->delete_flag) {
		res = fuse_udaa_read(fsmsession, fsdev_obj);
		if (res == -EINTR || res == -EAGAIN) {
			return SPDK_POLLER_IDLE;
		}
		if (res == INT_MAX) {
			return  SPDK_POLLER_BUSY;
		}
		ssam_fuse_share_memory(fsmsession);
		if (res > 0) {
			ssam_fuse_dispatcher_process(fsmsession);
			return SPDK_POLLER_BUSY;
		} else {
			return SPDK_POLLER_IDLE;
		}
	}

	ssam_free_data_session(fsdev_obj, fsmsession);
	return SPDK_POLLER_BUSY;
}

static void
ssam_shm_remount_submit_cb(void *cb_arg, int error)
{
	struct ssam_shm_ctx *ctx = (struct ssam_shm_ctx *)cb_arg;

	if (error != 0) {
		SPDK_WARNLOG("fail to reset mount!\n");
	} else {
		ctx->fsdev_obj->have_shm = false;
	}

	free(ctx->in_buf);
	free(ctx->out_buf);
	free(ctx);
}

static int
ssam_fs_shm_ctx_load(struct mount_info *info, struct ssam_shm_ctx **ctx, struct ssam_fsdev_object *fsdev_obj)
{
	void *inarg = NULL;
	void *outarg = NULL;
	struct ssam_shm_ctx *_ctx;

	_ctx = malloc(sizeof(struct ssam_shm_ctx));
	if (_ctx == NULL) {
		SPDK_ERRLOG("can not malloc ctx.\n");
		return -1;
	}

	_ctx->fsdev_obj = fsdev_obj;
	_ctx->in_buf = malloc(sizeof(struct fuse_in_header) + sizeof(struct fuse_in_header));
	if (_ctx->in_buf == NULL) {
		SPDK_ERRLOG("can not malloc in_buf.\n");
		return -1;
	}

	_ctx->out_buf = malloc(sizeof(struct fuse_out_header) + sizeof(struct fuse_init_out));
	if (_ctx->out_buf == NULL) {
		SPDK_ERRLOG("can not malloc out_buf.\n");
		return -1;
	}

	_ctx->in = (struct fuse_in_header *)_ctx->in_buf;
	inarg = (void *) &_ctx->in[1];
	_ctx->arg = (struct fuse_init_in *)inarg;

	_ctx->rsp = (struct fuse_out_header *)_ctx->out_buf;
	outarg = (void *) &_ctx->rsp[1];
	_ctx->out = (struct fuse_init_out *)outarg;

	_ctx->in->opcode = info->opcode;
	_ctx->in->padding = info->padding;
	_ctx->in->gid = info->gid;
	_ctx->in->len = info->len;
	_ctx->in->pid = info->pid;
	_ctx->in->total_extlen = info->total_extlen;
	_ctx->in->uid = info->uid;
	_ctx->in->unique = info->unique;
	_ctx->in->nodeid = info->nodeid;

	_ctx->arg->major = MEM_MAJOR;
	_ctx->arg->minor = MEM_MINOR;

	_ctx->in_iov[0].iov_base = (void *)_ctx->in;
	_ctx->in_iov[0].iov_len = sizeof(struct fuse_in_header);
	_ctx->in_iov[1].iov_base = (void *)_ctx->arg;
	_ctx->in_iov[1].iov_len = sizeof(struct fuse_init_in);

	_ctx->out_iov[0].iov_base = (void *)_ctx->rsp;
	_ctx->out_iov[0].iov_len = sizeof(struct fuse_out_header);
	_ctx->out_iov[1].iov_base = (void *)_ctx->out;
	_ctx->out_iov[1].iov_len = sizeof(struct fuse_init_out);

	*ctx = _ctx;
	return 0;
}

static void
ssam_fs_read_shm_mem(struct spdk_ssam_fs_session *fsmsession)
{
	struct ssam_fsdev_object *fsdev_obj = (struct ssam_fsdev_object *)fsmsession->fsdev_obj;
	char name[SHM_NAME] = {0};
	int shm_fd = 0;
	struct mount_info *info = NULL;
	struct ssam_shm_ctx *ctx = NULL;

	snprintf(name, sizeof(name), "shm_name%d", fsmsession->smsession.gfunc_id);

	shm_fd = shm_open(name, O_RDWR, 0600);
	if (shm_fd < 0 || fsdev_obj->have_shm == true) {
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
		SPDK_ERRLOG("can not read mmap.\n");
		return;
	}

	if (ssam_fs_shm_ctx_load(info, &ctx, fsdev_obj) != 0) {
		close(shm_fd);
		SPDK_ERRLOG("fs shm ctx failed.\n");
		return;
	}

	munmap(info, SHM_SIZE);
	close(shm_fd);

	fsdev_obj->have_shm = true;
	spdk_fuse_dispatcher_submit_request(fsmsession->fsdev_obj->fuse_disp, fsmsession->io_channel,
		ctx->in_iov, 2, ctx->out_iov, 2, ssam_shm_remount_submit_cb, ctx);
	SPDK_NOTICELOG("success reset mount.\n");
}

static int
ssam_fs_reactor_loop_start(struct spdk_ssam_session *smsession, void **unused) 
{
	struct spdk_ssam_fs_session *fsmsession = (struct spdk_ssam_fs_session *)smsession;
	struct ssam_fsdev_object *fsdev_obj = (struct ssam_fsdev_object *)fsmsession->fsdev_obj;

	fsmsession->io_channel = spdk_fuse_dispatcher_get_io_channel(fsmsession->fsdev_obj->fuse_disp);

	pthread_mutex_lock(&fsdev_obj->mutex);
	if (fsdev_obj->load_shm_flag == false) {
		ssam_fs_read_shm_mem(fsmsession);
		fsdev_obj->load_shm_flag = true;
	}
	pthread_mutex_unlock(&fsdev_obj->mutex);

	if (fsmsession->fs_poller == NULL) {
		fsmsession->fs_poller = SPDK_POLLER_REGISTER(ssam_fs_session_loop, smsession, 0);
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
ssam_create_fs_session(struct ssam_fs_construct_info *info, struct ssam_fsdev_object *fsdev_obj)
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
				SPDK_ERRLOG("failed to register session.\n");
				return ret;
			}
			fsmsession = (struct spdk_ssam_fs_session *)smsession;
			fsmsession->fsdev_obj = fsdev_obj;
			fsmsession->in_buf_used = false;
			fsmsession->dynamic_buf = NULL;
			fsmsession->need_write_config = (max_threads == 0);
			fsmsession->in_buf = spdk_zmalloc(SSAM_FUSE_MAX_MAX_PAGES * getpagesize() + SSAM_BUFFER_HEADER_SIZE,
				SSAM_FS_DEFAULT_ALIGN, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
			if (!fsmsession->in_buf) {
				SPDK_ERRLOG("malloc_buf in_buf spdk_zmalloc() failed.\n");
				return -ENOMEM;
			}
			fsmsession->in_buf_size = SSAM_FUSE_MAX_MAX_PAGES * getpagesize() + SSAM_BUFFER_HEADER_SIZE;

			ssam_session_start_done(smsession, 0);
			fsdev_obj->smsession[smsession->smdev->lcore_id] = smsession;
			max_threads++;
		}
	}
	
	info->max_threads = max_threads;
	return 0;
}

static int
ssam_init_fs_data(struct ssam_fs_construct_info *info, struct ssam_fsdev_object *fsdev_obj)
{
	pthread_mutex_init(&fsdev_obj->mutex, NULL);
	pthread_mutex_init(&fsdev_obj->exit_mutex, NULL);
	
	fsdev_obj->gfunc_id = info->gfunc_id;
	fsdev_obj->exit_num = 0;
	fsdev_obj->mounted = false;
	fsdev_obj->num_queues = info->max_threads;
	fsdev_obj->used = true;
	fsdev_obj->flr_seq = UINT32_MAX;
	fsdev_obj->have_shm = false;
	fsdev_obj->load_shm_flag = false;
	fsdev_obj->delete_flag = false;
	fsdev_obj->rsp_fn = NULL;
	fsdev_obj->rsp_ctx = NULL;

	fsdev_obj->name = spdk_sprintf_alloc("%s", info->name);
	if (fsdev_obj->name == NULL) {
		SPDK_ERRLOG("snprintf cotroller name failed\n");
		return -ENOMEM;
	}

	fsdev_obj->dbdf = spdk_sprintf_alloc("%s", info->dbdf);
	if (fsdev_obj->dbdf == NULL) {
		SPDK_ERRLOG("snprintf cotroller dbdf failed\n");
		return -ENOMEM;
	}

	fsdev_obj->udaa_fs_queues = (struct udaa_emlq **)calloc(SSAM_FS_LCORE_ID_MAX, sizeof(struct udaa_emlq *));
	if (fsdev_obj->udaa_fs_queues == NULL) {
		SPDK_ERRLOG("failed to alloc udaa_fs_queues\n");
		return -ENOMEM;
	}

	fsdev_obj->fs_reqs = (struct udaa_eml_req *)calloc(SSAM_FS_LCORE_ID_MAX, sizeof(struct udaa_eml_req));
	if (fsdev_obj->fs_reqs == NULL) {
		SPDK_ERRLOG("failed to alloc fs_reqs\n");
		return -ENOMEM;
	}

	if (create_fs_device(fsdev_obj->udaa_fs_queues, SSAM_FS_LCORE_ID_MAX, info->gfunc_id) != UDAA_SUCCESS) {
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
		return SPDK_POLLER_IDLE;
	}
	if (polled_num <= 0) {
		return SPDK_POLLER_IDLE;
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
ssam_fs_construct(struct ssam_fs_construct_info *info, void *request,
	spdk_ssam_session_rsp_fn rpc_ssam_send_response_cb)
{
	struct ssam_fs_add_fsdev_ctx *ctx = NULL;
	int ret = -1;
	struct ssam_fsdev_object *fsdev_obj = &fsdev_map[info->gfunc_id];
	struct spdk_ssam_fs_session *fsmsession = NULL;

	ssam_lock();

	ret = ssam_check_contrller_info(info);
	if (ret != 0) {
		ssam_unlock();
		return ret;
	}

	ret = ssam_create_fs_session(info, fsdev_obj);
	if (ret != 0) {
		goto err_out;
	}

	ret = ssam_init_fs_data(info, fsdev_obj);
	if (ret != 0) {
		goto err_out;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		goto err_out;
	}

	fsdev_obj->init_thread = spdk_get_thread();

	ctx->fsdev_obj = fsdev_obj;
	ctx->cb = rpc_ssam_send_response_cb;
	ctx->cb_arg = request;
	ctx->max_threads = info->max_threads;
	ctx->send_event_flag.need_async = false;
	ctx->send_event_flag.need_rsp = false;

	ret = spdk_fuse_dispatcher_create(info->fsdev_name, ssam_fuse_disp_event_cb, fsdev_obj,
		ssam_fuse_dispatcher_create_cpl, ctx);
	if (ret) {
		SPDK_ERRLOG("Failed to create fs controller for %s (err=%d)\n", info->fsdev_name, ret);
		free(ctx);
		goto err_out;
	}

	ssam_update_virtio_device_used(info->gfunc_id, 1);

	if (g_ssam_fs_poller_ctx.pf_poller[info->gfunc_id] != NULL) {
		spdk_poller_unregister(&g_ssam_fs_poller_ctx.pf_poller[info->gfunc_id]);
		g_ssam_fs_poller_ctx.pf_poller[info->gfunc_id] = NULL;
	}

	ssam_unlock();
	return 0;

err_out:
	ssam_free_fsdev_object(fsdev_obj);
	if ((ret != 0) && (fsmsession != NULL) && (fsmsession->smsession.smdev != NULL)) {
		ssam_session_unreg_response_cb(&fsmsession->smsession);
		if (ssam_session_unregister(&fsmsession->smsession) != 0) {
			SPDK_ERRLOG("function id %d: fs construct failed and session remove failed, ret=%d\n",
				info->gfunc_id, ret);
		}
	}

	ssam_unlock();
	return ret;
}

int
ssam_fs_destory(char *name, bool force, void *request,
		spdk_ssam_session_rsp_fn rpc_ssam_send_response_cb)
{
	char shm_name[SHM_NAME] = {0};

	for (int i = 0; i < SSAM_HOSTEP_NUM_MAX; i++) {
		if (fsdev_map[i].name != NULL && strcmp(fsdev_map[i].name, name) == 0) {
			if (fsdev_map[i].mounted == true && force != true) {
				SPDK_ERRLOG("fs controller %u is busy\n", fsdev_map[i].gfunc_id);
				return -EBUSY;
			}
			SPDK_NOTICELOG("fs controller %u removing, the force flag is %d\n", fsdev_map[i].gfunc_id, force);
			snprintf(shm_name, sizeof(shm_name), "shm_name%d", fsdev_map[i].gfunc_id);
			shm_unlink(shm_name);

			fsdev_map[i].rsp_ctx = request;
			fsdev_map[i].rsp_fn = rpc_ssam_send_response_cb;
			g_ssam_fs_poller_ctx.pf_poller_ctx[fsdev_map[i].gfunc_id].restart_flag = false;
			g_ssam_fs_poller_ctx.pf_poller[fsdev_map[i].gfunc_id] = SPDK_POLLER_REGISTER(ssam_dev_io_scan_poller,
				&g_ssam_fs_poller_ctx.pf_poller_ctx[fsdev_map[i].gfunc_id], 0);
			ssam_update_virtio_device_used(fsdev_map[i].gfunc_id, 0);
			ssam_fs_event_cb(&fsdev_map[i]);
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
		if (fsdev_map[i].used == true && fsdev_map[i].flr_seq != *flr_map_p && fsdev_map[i].flr_seq != UINT32_MAX) {
			if (fsdev_map[i].mounted) {
				fsdev_destroy(ssam_to_fs_session(fsdev_map[i].smsession[0]));
				fsdev_map[i].mounted = false;
			}
			fsdev_map[i].flr_seq = *flr_map_p;
		}
		flr_map_p++;
	}

	return SPDK_POLLER_IDLE;
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
