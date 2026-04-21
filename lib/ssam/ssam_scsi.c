/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021-2025 Huawei Technologies Co.
 *   All rights reserved.
 */

#include "linux/virtio_scsi.h"

#include "spdk/stdinc.h"

#include "spdk/likely.h"
#include "spdk/scsi_spec.h"
#include "spdk/env.h"
#include "spdk/scsi.h"
#include "spdk/ssam.h"
#include "spdk/string.h"
#include "spdk/bdev_module.h"

#include "ssam_internal.h"

#define SESSION_STOP_POLLER_PERIOD      1000
#define IOV_HEADER_TAIL_NUM             2
#define PAYLOAD_SIZE_MAX               (2048U * 2048)
#define VMIO_TYPE_VIRTIO_SCSI_CTRL      4
#define SSAM_SPDK_SCSI_DEV_MAX_LUN      1
#define SSAM_SENSE_DATE_LEN             32
#define PERF_STAT

/* Features supported by virtio-scsi lib. */
#define SPDK_SSAM_SCSI_FEATURES    (SPDK_SSAM_FEATURES | \
    (1ULL << VIRTIO_SCSI_F_INOUT) | \
    (1ULL << VIRTIO_SCSI_F_HOTPLUG) | \
    (1ULL << VIRTIO_SCSI_F_CHANGE) | \
    (1ULL << VIRTIO_SCSI_F_T10_PI))

/* Features that are specified in VIRTIO SCSI but currently not supported:
 * - Live migration not supported yet
 * - T10 PI
 */
#define SPDK_SSAM_SCSI_DISABLED_FEATURES    (SPDK_SSAM_DISABLED_FEATURES | \
    (1ULL << VIRTIO_SCSI_F_T10_PI))

/* ssam-user-scsi support protocol features */
#define SPDK_SSAM_SCSI_PROTOCOL_FEATURES    (1ULL << SSAM_USER_PROTOCOL_F_INFLIGHT_SHMFD)

enum spdk_scsi_dev_ssam_status {
	/* Target ID is empty. */
	SSAM_SCSI_DEV_EMPTY,

	/* Target is still being added. */
	SSAM_SCSI_DEV_ADDING,

	/* Target ID occupied. */
	SSAM_SCSI_DEV_PRESENT,

	/* Target ID is occupied but removal is in progress. */
	SSAM_SCSI_DEV_REMOVING,

	/* In session - device (SCSI target) seen but removed. */
	SSAM_SCSI_DEV_REMOVED,
};

struct ssam_scsi_stat {
	uint64_t count;
	uint64_t total_tsc; /* pre_dma <- -> post_return */
	uint64_t dma_tsc;   /* pre_dma <- -> post_dma */
	uint64_t bdev_tsc;   /* pre_bdev <- -> post_bdev */
	uint64_t bdev_submit_tsc;   /* <- spdk_bdev_xxx -> */
	uint64_t complete_tsc;   /* pre_return <- -> post_return */
	uint64_t internel_tsc;  /* total_tsc - dma_tsc - bdev_tsc - complete_tsc */

	uint64_t complete_read_ios;     /* Number of successfully completed read requests */
	uint64_t err_read_ios;          /* Number of failed completed read requests */
	uint64_t complete_write_ios;    /* Number of successfully completed write requests */
	uint64_t err_write_ios;         /* Number of failed completed write requests */
	uint64_t flush_ios;             /* Total number of flush requests */
	uint64_t complete_flush_ios;    /* Number of successfully completed flush requests */
	uint64_t err_flush_ios;         /* Number of failed completed flush requests */
	uint64_t fatal_ios;
	uint64_t io_retry;

	uint64_t start_count;
	uint64_t dma_count;
	uint64_t dma_complete_count;
	uint64_t bdev_count;
	uint64_t bdev_complete_count;
};

struct spdk_scsi_dev_io_state {
	struct spdk_bdev_io_stat stat;
	uint64_t submit_tsc;
	struct ssam_scsi_stat scsi_stat;
};

/** Context for a SCSI target in a ssam device */
struct spdk_scsi_dev_ssam_state {
	struct spdk_scsi_dev_io_state *io_stat[SSAM_SPDK_SCSI_DEV_MAX_LUN];
	struct spdk_scsi_dev *dev;

	enum spdk_scsi_dev_ssam_status status;

	uint64_t flight_io;
};

struct ssam_scsi_tgt_hotplug_ctx {
	unsigned scsi_tgt_num;
};

struct spdk_ssam_scsi_session {
	struct spdk_ssam_session smsession;
	int ref;
	bool registered;
	struct spdk_poller *stop_poller;
	struct spdk_scsi_dev_ssam_state scsi_dev_state[SPDK_SSAM_SCSI_CTRLR_MAX_DEVS];
	char *dbdf;
};

struct ssam_scsi_session_ctx {
	struct spdk_ssam_scsi_session *ssmsession;
	void **user_ctx;
};

struct ssam_scsi_task_stat {
	uint64_t start_tsc;
	uint64_t dma_start_tsc;
	uint64_t dma_end_tsc;
	uint64_t bdev_start_tsc;
	uint64_t bdev_func_tsc;
	uint64_t bdev_end_tsc;
	uint64_t complete_start_tsc;
	uint64_t complete_end_tsc;
};

struct spdk_ssam_scsi_task {
	struct spdk_scsi_task scsi_task;
	/* Returned status of I/O processing, it can be VIRTIO_BLK_S_OK,
	 * VIRTIO_BLK_S_IOERR or VIRTIO_BLK_S_UNSUPP
	 */
	union {
		struct virtio_scsi_cmd_resp resp;
		struct virtio_scsi_ctrl_tmf_resp tmf_resp;
	};

	/* Number of bytes processed successfully */
	uint32_t used_len;

	/* Records the amount of valid data in the struct iovec iovs array. */
	uint32_t iovcnt;
	struct ssam_iovec iovs;

	/* If set, the task is currently used for I/O processing. */
	bool used;

	/* For bdev io wait */
	struct spdk_ssam_scsi_session *ssmsession;
	struct spdk_ssam_session_io_wait session_io_wait;

	/* ssam request data */
	struct ssam_request *io_req;

	uint16_t vq_idx;
	uint16_t task_idx;
	int32_t tgt_id;
	struct spdk_ssam_session *smsession;
	struct spdk_scsi_dev *scsi_dev;
	struct ssam_scsi_task_stat task_stat;
};

struct ssam_add_tgt_ev_ctx {
	char *bdev_name;
	int tgt_num;
};

static void ssam_scsi_request_worker(struct spdk_ssam_session *smsession, void *arg);
static void ssam_scsi_destroy_bdev_device(struct spdk_ssam_session *smsession, void *args);
static void ssam_scsi_response_worker(struct spdk_ssam_session *smsession, void *arg);
static int ssam_scsi_remove_session(struct spdk_ssam_session *smsession);
static void ssam_scsi_remove_self(struct spdk_ssam_session *smsession);
static void ssam_scsi_dump_info_json(struct spdk_ssam_session *smsession,
				     struct spdk_json_write_ctx *w);
static void ssam_scsi_write_config_json(struct spdk_ssam_session *smsession,
					struct spdk_json_write_ctx *w);
static int ssam_scsi_get_config(struct spdk_ssam_session *smsession, uint8_t *config,
				uint32_t len, uint16_t queues);
static void ssam_scsi_show_iostat_json(struct spdk_ssam_session *smsession,
				       struct spdk_ssam_show_iostat_args *args,
				       struct spdk_json_write_ctx *w);
static void ssam_scsi_clear_iostat_json(struct spdk_ssam_session *smsession);
static void ssam_scsi_print_stuck_io_info(struct spdk_ssam_session *smsession);
static void ssam_scsi_req_complete(struct spdk_ssam_dev *smdev, struct ssam_request *io_req,
				   uint8_t status);
static struct spdk_bdev *ssam_scsi_get_bdev(struct spdk_ssam_session *smsession, uint32_t id);

static void ssam_free_scsi_task_pool(struct spdk_ssam_scsi_session *ssmsession);
static int ssam_scsi_dev_hot_remove_tgt(struct spdk_ssam_session *smsession, void **_ctx);
static void ssam_scsi_process_io_task(struct spdk_ssam_session *smsession,
				      struct spdk_ssam_scsi_task *task);
static int ssam_scsi_task_iovs_memory_get(struct spdk_ssam_scsi_task *task, uint32_t payload_size);
static void ssam_scsi_submit_io_task(struct spdk_ssam_scsi_task *task);
static void ssam_scsi_destruct_tgt(struct spdk_ssam_scsi_session *ssmsession, int scsi_tgt_num);

static const struct spdk_ssam_session_backend g_ssam_scsi_session_backend = {
	.type = VIRTIO_TYPE_SCSI,
	.request_worker = ssam_scsi_request_worker,
	.destroy_bdev_device = ssam_scsi_destroy_bdev_device,
	.response_worker = ssam_scsi_response_worker,
	.remove_session = ssam_scsi_remove_session,
	.remove_self = ssam_scsi_remove_self,
	.print_stuck_io_info = ssam_scsi_print_stuck_io_info,
	.dump_info_json = ssam_scsi_dump_info_json,
	.write_config_json = ssam_scsi_write_config_json,
	.ssam_get_config = ssam_scsi_get_config,
	.show_iostat_json = ssam_scsi_show_iostat_json,
	.clear_iostat_json = ssam_scsi_clear_iostat_json,
	.get_bdev = ssam_scsi_get_bdev,
};

static void
ssam_scsi_task_stat_tick(uint64_t *tsc)
{
#ifdef PERF_STAT
	*tsc = spdk_get_ticks();
#endif
	return;
}

static void
ssam_scsi_stat_statistics(struct spdk_ssam_scsi_task *task)
{
#ifdef PERF_STAT
	if (task->scsi_task.lun == NULL || task->io_req->type == VMIO_TYPE_VIRTIO_SCSI_CTRL ||
	    task->task_stat.bdev_func_tsc == 0 || task->task_stat.bdev_end_tsc == 0) {
		return;
	}

	int32_t lun_id = spdk_scsi_lun_get_id(task->scsi_task.lun);
	struct ssam_scsi_stat *scsi_stat =
			&task->ssmsession->scsi_dev_state[task->tgt_id].io_stat[lun_id]->scsi_stat;

	uint64_t dma_tsc = task->task_stat.dma_end_tsc - task->task_stat.dma_start_tsc;
	uint64_t bdev_tsc = task->task_stat.bdev_end_tsc - task->task_stat.bdev_start_tsc;
	uint64_t bdev_submit_tsc = task->task_stat.bdev_func_tsc - task->task_stat.bdev_start_tsc;
	uint64_t complete_tsc = task->task_stat.complete_end_tsc - task->task_stat.complete_start_tsc;
	uint64_t total_tsc = task->task_stat.complete_end_tsc - task->task_stat.start_tsc;

	struct ssam_io_message *io_cmd = &task->io_req->req.cmd;
	if (io_cmd->writable) { /* read io */
		if (task->scsi_task.status == SPDK_SCSI_STATUS_GOOD) {
			scsi_stat->complete_read_ios++;
		} else {
			scsi_stat->err_read_ios++;
		}
	} else {
		if (task->scsi_task.status == SPDK_SCSI_STATUS_GOOD) {
			scsi_stat->complete_write_ios++;
		} else {
			scsi_stat->err_write_ios++;
		}
	}

	scsi_stat->dma_tsc += dma_tsc;
	scsi_stat->bdev_tsc += bdev_tsc;
	scsi_stat->bdev_submit_tsc += bdev_submit_tsc;
	scsi_stat->complete_tsc += complete_tsc;
	scsi_stat->total_tsc += total_tsc;
	scsi_stat->internel_tsc += total_tsc - complete_tsc - bdev_tsc - dma_tsc;
	scsi_stat->count += 1;
#endif
}

static uint32_t
ssam_scsi_tgtid_to_lunid(uint32_t tgt_id)
{
	return (((tgt_id) << 0x8) | SSAM_VIRTIO_SCSI_LUN_ID);
}

static int
ssam_scsi_get_config(struct spdk_ssam_session *smsession, uint8_t *config,
		     uint32_t len, uint16_t queues)
{
	struct virtio_scsi_config scsi_cfg;
	scsi_cfg.num_queues = 0x80;
	scsi_cfg.seg_max = 0x6f;
	scsi_cfg.max_sectors = 0x1ff;
	scsi_cfg.cmd_per_lun = 0x80;
	scsi_cfg.event_info_size = 0;
	scsi_cfg.sense_size = 0x60;
	scsi_cfg.cdb_size = 0x20;
	scsi_cfg.max_channel = 0;
	scsi_cfg.max_target = SPDK_SSAM_SCSI_CTRLR_MAX_DEVS;
	scsi_cfg.max_lun = 0xff;

	memcpy(config, (void *)&scsi_cfg, sizeof(struct virtio_scsi_config));
	return 0;
}

static int
ssam_scsi_send_event(struct spdk_ssam_session *smsession, unsigned scsi_dev_num,
		     uint32_t event, uint32_t reason)
{
	struct virtio_scsi_event vscsi_event = {0};
	int ret;

	vscsi_event.event = event;
	vscsi_event.reason = reason;

	vscsi_event.lun[0] = 1;
	vscsi_event.lun[0x1] = (uint8_t)scsi_dev_num;
	vscsi_event.lun[0x2] = 0;
	vscsi_event.lun[0x3] = 0;
	memset(&vscsi_event.lun[0x4], 0, 0x4);

	ret = ssam_send_action(smsession->gfunc_id, SSAM_FUNCTION_ACTION_SCSI_EVENT,
			       (const void *)&vscsi_event, sizeof(struct virtio_scsi_event));
	if (ret < 0) {
		SPDK_ERRLOG("%s: SCSI target %d send event %u(reason %u) failed: %s.\n",
			    smsession->name, scsi_dev_num, event, reason, strerror(-ret));
	}
	return ret;
}

static void
ssam_scsi_stop_cpl_cb(struct spdk_ssam_session *smsession, void **ctx)
{
	struct spdk_ssam_scsi_session *ssmsession = (struct spdk_ssam_scsi_session *)smsession;
	spdk_ssam_session_rsp_fn rsp_fn = smsession->rsp_fn;
	void *rsp_ctx = smsession->rsp_ctx;

	SPDK_NOTICELOG("SCSI controller %s deleted\n", smsession->name);

	if (smsession->name != NULL) {
		free(smsession->name);
		smsession->name = NULL;
	}

	if (ssmsession->dbdf != NULL) {
		free(ssmsession->dbdf);
		ssmsession->dbdf = NULL;
	}

	ssam_set_session_be_freed(ctx);
	memset(ssmsession, 0, sizeof(*ssmsession));
	free(ssmsession);

	if (rsp_fn != NULL) {
		rsp_fn(rsp_ctx, 0);
		rsp_fn = NULL;
	}
}

static void
ssam_scsi_destroy_session(struct ssam_scsi_session_ctx *ctx)
{
	struct spdk_ssam_session *smsession = &ctx->ssmsession->smsession;
	struct spdk_ssam_scsi_session *ssmsession = ctx->ssmsession;

	if (smsession->task_cnt > 0) {
		return;
	}

	if (ssmsession->ref > 0) {
		return;
	}

	ssam_session_destroy(smsession);

	ssmsession->registered = false;
	spdk_poller_unregister(&ssmsession->stop_poller);
	ssam_free_scsi_task_pool(ssmsession);
	ssam_session_stop_done(&ssmsession->smsession, 0, ctx->user_ctx);
	free(ctx);

	return;
}

static int
ssam_scsi_destroy_session_poller_cb(void *arg)
{
	struct ssam_scsi_session_ctx *ctx = arg;

	if (ssam_trylock() != 0) {
		return SPDK_POLLER_BUSY;
	}

	ssam_scsi_destroy_session(ctx);

	ssam_unlock();

	return SPDK_POLLER_BUSY;
}

static int
ssam_scsi_stop_cb(struct spdk_ssam_session *smsession, void **ctx)
{
	struct spdk_ssam_scsi_session *ssmsession = (struct spdk_ssam_scsi_session *)smsession;
	struct ssam_scsi_session_ctx *_ctx =
		(struct ssam_scsi_session_ctx *)calloc(1, sizeof(struct ssam_scsi_session_ctx));

	if (_ctx == NULL) {
		SPDK_ERRLOG("%s: calloc scsi session ctx error.\n", smsession->name);
		return -ENOMEM;
	}

	_ctx->ssmsession = ssmsession;
	_ctx->user_ctx = ctx;

	ssmsession->stop_poller = SPDK_POLLER_REGISTER(ssam_scsi_destroy_session_poller_cb,
				  _ctx, SESSION_STOP_POLLER_PERIOD);
	if (ssmsession->stop_poller == NULL) {
		SPDK_ERRLOG("%s: ssam_destroy_session_poller_cb start failed.\n", smsession->name);
		ssam_session_stop_done(smsession, -EBUSY, ctx);
		free(_ctx);
		return -EBUSY;
	}

	return 0;
}

static int
ssam_scsi_stop(struct spdk_ssam_session *smsession)
{
	struct spdk_ssam_send_event_flag send_event_flag = {
		.need_async = true,
		.need_rsp = true,
	};
	return ssam_send_event_to_session(smsession, ssam_scsi_stop_cb, ssam_scsi_stop_cpl_cb,
					  send_event_flag, NULL);
}

/* sync interface for hot-remove */
static void
ssam_scsi_remove_self(struct spdk_ssam_session *smsession)
{
	struct spdk_ssam_scsi_session *ssmsession = (struct spdk_ssam_scsi_session *)smsession;
	/* no need error */
	if (ssmsession->ref > 0) {
		return;     /* still have targets */
	}

	SPDK_NOTICELOG("%s: is being freed\n", smsession->name);

	ssmsession->registered = false;
	ssam_free_scsi_task_pool(ssmsession);

	ssam_sessions_remove(smsession->smdev->smsessions, smsession);

	if (smsession->smdev->active_session_num > 0) {
		smsession->smdev->active_session_num--;
	}
	smsession->smdev = NULL;
	/* free smsession */
	free(smsession->name);
	free(ssmsession->dbdf);
	free(ssmsession);
}

/* async interface */
static int
ssam_scsi_remove_session(struct spdk_ssam_session *smsession)
{
	struct spdk_ssam_scsi_session *ssmsession = (struct spdk_ssam_scsi_session *)smsession;
	int ret;

	if (smsession->registered && ssmsession->ref != 0) {
		SPDK_ERRLOG("%s: SCSI target %d is still present.\n", smsession->name, ssmsession->ref);
		return -EBUSY;
	}

	ret = ssam_scsi_stop(smsession);
	if (ret != 0) {
		return ret;
	}

	return 0;
}

static struct spdk_scsi_dev *
ssam_scsi_dev_get_tgt(struct spdk_ssam_scsi_session *ssmsession, uint8_t num)
{
	if (ssmsession == NULL) {
		SPDK_ERRLOG("ssmsession is null.\n");
		return NULL;
	}
	if (num >= SPDK_SSAM_SCSI_CTRLR_MAX_DEVS) {
		SPDK_ERRLOG("%s: tgt num %u over %u.\n", ssmsession->smsession.name, num,
			    SPDK_SSAM_SCSI_CTRLR_MAX_DEVS);
		return NULL;
	}
	if (ssmsession->scsi_dev_state[num].status != SSAM_SCSI_DEV_PRESENT) {
		return NULL;
	}

	if (ssmsession->scsi_dev_state[num].dev == NULL) {
		SPDK_ERRLOG("%s: no tgt num %u device.\n", ssmsession->smsession.name, num);
		return NULL;
	}
	return ssmsession->scsi_dev_state[num].dev;
}

static void
ssam_scsi_dump_device_info(struct spdk_ssam_session *smsession, struct spdk_json_write_ctx *w)
{
	struct spdk_ssam_scsi_session *ssmsession = (struct spdk_ssam_scsi_session *)smsession;
	struct spdk_scsi_dev *sdev;
	struct spdk_scsi_lun *lun;
	int32_t tgt_id;

	spdk_json_write_named_array_begin(w, "scsi_targets");
	for (tgt_id = 0; tgt_id < SPDK_SSAM_SCSI_CTRLR_MAX_DEVS; tgt_id++) {
		sdev = ssam_scsi_dev_get_tgt(ssmsession, tgt_id);
		if (!sdev) {
			continue;
		}

		spdk_json_write_object_begin(w);

		spdk_json_write_named_uint32(w, "scsi_target_num", tgt_id);
		spdk_json_write_named_uint32(w, "id", spdk_scsi_dev_get_id(sdev));
		spdk_json_write_named_string(w, "target_name", spdk_scsi_dev_get_name(sdev));
		lun = spdk_scsi_dev_get_lun(sdev, 0);
		if (!lun) {
			continue;
		}
		spdk_json_write_named_string(w, "bdev_name", spdk_scsi_lun_get_bdev_name(lun));

		spdk_json_write_object_end(w);
	}

	spdk_json_write_array_end(w);
}

static void
ssam_scsi_dump_info_json(struct spdk_ssam_session *smsession, struct spdk_json_write_ctx *w)
{
	struct spdk_ssam_scsi_session *ssmsession = (struct spdk_ssam_scsi_session *)smsession;
	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "dbdf", ssmsession->dbdf);
	spdk_json_write_named_string(w, "name", ssam_session_get_name(smsession));
	spdk_json_write_named_uint32(w, "function_id", (uint32_t)smsession->gfunc_id);
	spdk_json_write_named_uint32(w, "queues", (uint32_t)smsession->max_queues);
	spdk_json_write_named_string(w, "ctrlr", ssam_dev_get_name(smsession->smdev));
	spdk_json_write_named_string_fmt(w, "cpumask", "0x%s",
					 spdk_cpuset_fmt(spdk_thread_get_cpumask(smsession->smdev->thread)));

	ssam_scsi_dump_device_info(smsession, w);

	spdk_json_write_object_end(w);
}

static void
ssam_scsi_write_config_json(struct spdk_ssam_session *smsession,
			    struct spdk_json_write_ctx *w)
{
	struct spdk_ssam_scsi_session *ssmsession = (struct spdk_ssam_scsi_session *)smsession;
	struct spdk_scsi_dev *sdev;
	struct spdk_scsi_lun *lun;
	int32_t tgt_id;

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "method", "create_scsi_controller");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "dbdf", ssmsession->dbdf);
	spdk_json_write_named_string(w, "name", smsession->name);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);

	for (tgt_id = 0; tgt_id < SPDK_SSAM_SCSI_CTRLR_MAX_DEVS; tgt_id++) {
		sdev = ssam_scsi_dev_get_tgt(ssmsession, tgt_id);
		if (!sdev) {
			continue;
		}

		lun = spdk_scsi_dev_get_lun(sdev, 0);
		if (!lun) {
			SPDK_ERRLOG("%s: no lun, continue.\n", smsession->name);
			continue;
		}

		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "method", "scsi_controller_add_target");

		spdk_json_write_named_object_begin(w, "params");
		spdk_json_write_named_string(w, "name", smsession->name);
		spdk_json_write_named_uint32(w, "scsi_tgt_num", tgt_id);

		spdk_json_write_named_string(w, "bdev_name", spdk_scsi_lun_get_bdev_name(lun));
		spdk_json_write_object_end(w);

		spdk_json_write_object_end(w);
	}
}

static void
ssam_scsi_show_tgt_iostat_json(struct spdk_ssam_scsi_session *ssmsession,
			       struct spdk_json_write_ctx *w, int32_t tgt_id, struct spdk_scsi_dev *sdev)
{
	struct spdk_scsi_dev_io_state *io_stat;
	struct spdk_scsi_lun *lun;
	struct ssam_scsi_stat scsi_stat;
	uint64_t ticks_hz = spdk_get_ticks_hz();
	uint64_t count;
	uint64_t poll_count;

	lun = spdk_scsi_dev_get_lun(sdev, 0);
	if (lun == NULL) {
		return;
	}

	io_stat = ssmsession->scsi_dev_state[tgt_id].io_stat[0];
	if (io_stat == NULL) {
		SPDK_ERRLOG("No scsi iostat, tgt_id %d\n", tgt_id);
		return;
	}

	spdk_json_write_object_begin(w);

	spdk_json_write_named_uint32(w, "scsi_dev_num", tgt_id);
	spdk_json_write_named_uint32(w, "id", spdk_scsi_dev_get_id(sdev));
	spdk_json_write_named_string(w, "target_name", spdk_scsi_dev_get_name(sdev));

	memcpy(&scsi_stat, &io_stat->scsi_stat, sizeof(struct ssam_scsi_stat));

	spdk_json_write_named_int32(w, "id", spdk_scsi_lun_get_id(lun));
	spdk_json_write_named_string(w, "bdev_name", spdk_scsi_lun_get_bdev_name(lun));
	spdk_json_write_named_uint64(w, "bytes_read", io_stat->stat.bytes_read);
	spdk_json_write_named_uint64(w, "num_read_ops", io_stat->stat.num_read_ops);
	spdk_json_write_named_uint64(w, "bytes_written", io_stat->stat.bytes_written);
	spdk_json_write_named_uint64(w, "num_write_ops", io_stat->stat.num_write_ops);
	spdk_json_write_named_uint64(w, "read_latency_ticks", io_stat->stat.read_latency_ticks);
	spdk_json_write_named_uint64(w, "write_latency_ticks", io_stat->stat.write_latency_ticks);

	spdk_json_write_named_uint64(w, "complete_read_ios", scsi_stat.complete_read_ios);
	spdk_json_write_named_uint64(w, "err_read_ios", scsi_stat.err_read_ios);
	spdk_json_write_named_uint64(w, "complete_write_ios", scsi_stat.complete_write_ios);
	spdk_json_write_named_uint64(w, "err_write_ios", scsi_stat.err_write_ios);
	spdk_json_write_named_uint64(w, "flush_ios", scsi_stat.flush_ios);
	spdk_json_write_named_uint64(w, "complete_flush_ios", scsi_stat.complete_flush_ios);
	spdk_json_write_named_uint64(w, "err_flush_ios", scsi_stat.err_flush_ios);
	spdk_json_write_named_uint64(w, "fatal_ios", scsi_stat.fatal_ios);
	spdk_json_write_named_uint64(w, "io_retry", scsi_stat.io_retry);

	spdk_json_write_named_uint64(w, "start_count", scsi_stat.start_count);
	spdk_json_write_named_uint64(w, "dma_count", scsi_stat.dma_count);
	spdk_json_write_named_uint64(w, "dma_complete_count", scsi_stat.dma_complete_count);
	spdk_json_write_named_uint64(w, "bdev_count", scsi_stat.bdev_count);
	spdk_json_write_named_uint64(w, "bdev_complete_count", scsi_stat.bdev_complete_count);
	spdk_json_write_named_uint64(w, "flight_io", ssmsession->scsi_dev_state[tgt_id].flight_io);

	if (scsi_stat.count == 0) {
		count = 1;
	} else {
		count = scsi_stat.count;
	}

	if (ssmsession->smsession.smdev->stat.poll_count == 0) {
		poll_count = 1;
	} else {
		poll_count = ssmsession->smsession.smdev->stat.poll_count;
	}

	spdk_json_write_named_string_fmt(w, "poll_lat", "%.9f",
					 (float)ssmsession->smsession.smdev->stat.poll_tsc / poll_count / ticks_hz);
	spdk_json_write_named_string_fmt(w, "total_lat", "%.9f",
					 (float)scsi_stat.total_tsc / count / ticks_hz);
	spdk_json_write_named_string_fmt(w, "dma_lat", "%.9f", (float)scsi_stat.dma_tsc / count / ticks_hz);
	spdk_json_write_named_string_fmt(w, "bdev_lat", "%.9f",
					 (float)scsi_stat.bdev_tsc / count / ticks_hz);
	spdk_json_write_named_string_fmt(w, "bdev_submit_lat", "%.9f",
					 (float)scsi_stat.bdev_submit_tsc / count / ticks_hz);
	spdk_json_write_named_string_fmt(w, "complete_lat", "%.9f",
					 (float)scsi_stat.complete_tsc / count / ticks_hz);
	spdk_json_write_named_string_fmt(w, "internal_lat", "%.9f",
					 (float)scsi_stat.internel_tsc / count / ticks_hz);

	spdk_json_write_object_end(w);
}

static void
ssam_scsi_show_iostat_json(struct spdk_ssam_session *smsession,
			   struct spdk_ssam_show_iostat_args *args,
			   struct spdk_json_write_ctx *w)
{
	struct spdk_ssam_scsi_session *ssmsession = (struct spdk_ssam_scsi_session *)smsession;
	struct spdk_scsi_dev *sdev;
	int32_t tgt_id;

	if (args->id != SPDK_INVALID_ID) {
		sdev = ssam_scsi_dev_get_tgt(ssmsession, args->id);
		if (sdev != NULL) {
			ssam_scsi_show_tgt_iostat_json(ssmsession, w, args->id, sdev);
		} else {
			spdk_json_write_object_begin(w);
			spdk_json_write_object_end(w);
		}
		return;
	}

	spdk_json_write_object_begin(w);

	spdk_json_write_named_uint32(w, "function_id", smsession->gfunc_id);

	spdk_json_write_named_array_begin(w, "scsi_target");

	for (tgt_id = 0; tgt_id < SPDK_SSAM_SCSI_CTRLR_MAX_DEVS; tgt_id++) {
		sdev = ssam_scsi_dev_get_tgt(ssmsession, tgt_id);
		if (!sdev) {
			continue;
		}
		ssam_scsi_show_tgt_iostat_json(ssmsession, w, tgt_id, sdev);
	}

	spdk_json_write_array_end(w);
	spdk_json_write_object_end(w);
}

static void
ssam_scsi_clear_iostat_json(struct spdk_ssam_session *smsession)
{
	struct spdk_ssam_scsi_session *ssmsession = (struct spdk_ssam_scsi_session *)smsession;
	struct spdk_scsi_dev_io_state *io_stat;
	int32_t tgt_id;
	int32_t lun_id;
	for (tgt_id = 0; tgt_id < SPDK_SSAM_SCSI_CTRLR_MAX_DEVS; tgt_id++) {
		for (lun_id = 0; lun_id < SSAM_SPDK_SCSI_DEV_MAX_LUN; lun_id++) {
			io_stat = ssmsession->scsi_dev_state[tgt_id].io_stat[lun_id];
			if (io_stat == NULL) {
				continue;
			}
			memset(io_stat, 0, sizeof(struct spdk_scsi_dev_io_state));
		}
	}
	return;
}

static struct spdk_bdev *
ssam_scsi_get_bdev(struct spdk_ssam_session *smsession, uint32_t tgt_id)
{
	struct spdk_ssam_scsi_session *ssmsession = (struct spdk_ssam_scsi_session *)smsession;
	struct spdk_scsi_dev *scsi_dev;
	struct spdk_scsi_lun *scsi_lun = NULL;
	const char *bdev_name = NULL;
	if (tgt_id >= SPDK_SSAM_SCSI_CTRLR_MAX_DEVS) {
		SPDK_ERRLOG("%s: tgt %d invalid\n", smsession->name, tgt_id);
		return NULL;
	}
	if (ssmsession->scsi_dev_state[tgt_id].dev == NULL) {
		SPDK_ERRLOG("%s: tgt %d not be created\n", smsession->name, tgt_id);
		return NULL;
	}

	scsi_dev = ssmsession->scsi_dev_state[tgt_id].dev;
	/* lun id use 0 */
	scsi_lun = spdk_scsi_dev_get_lun(scsi_dev, 0);
	if (scsi_lun == NULL) {
		return NULL;
	}
	bdev_name = spdk_scsi_lun_get_bdev_name(scsi_lun);
	if (bdev_name == NULL) {
		return NULL;
	}
	return spdk_bdev_get_by_name(bdev_name);
}

static int
ssam_scsi_iostat_construct(struct spdk_ssam_scsi_session *ssmsession, int32_t tgt_id,
			   int *lun_id_list, int num_luns)
{
	struct spdk_scsi_dev_io_state *io_stat;
	int32_t lun_id;
	int i;

	for (i = 0; i < num_luns; i++) {
		lun_id = lun_id_list[i];
		io_stat = ssmsession->scsi_dev_state[tgt_id].io_stat[lun_id];
		if (io_stat != NULL) {
			SPDK_ERRLOG("io_stat with tgt %d lun %d already exist\n", tgt_id, lun_id);
			return -EEXIST;
		}

		io_stat = calloc(1, sizeof(*io_stat));
		if (io_stat == NULL) {
			SPDK_ERRLOG("Could not allocate io_stat for tgt %d lun %d\n", tgt_id, lun_id);
			return -ENOMEM;
		}
		ssmsession->scsi_dev_state[tgt_id].io_stat[lun_id] = io_stat;
	}

	return 0;
}

static void
ssam_scsi_iostat_destruct(struct spdk_scsi_dev_ssam_state *state)
{
	int32_t lun_id;

	for (lun_id = 0; lun_id < SSAM_SPDK_SCSI_DEV_MAX_LUN; lun_id++) {
		if (state->io_stat[lun_id] != NULL) {
			free(state->io_stat[lun_id]);
			state->io_stat[lun_id] = NULL;
		}
	}

	return;
}

static void
ssam_remove_scsi_tgt(struct spdk_ssam_scsi_session *ssmsession, unsigned scsi_tgt_num)
{
	struct spdk_scsi_dev_ssam_state *state = &ssmsession->scsi_dev_state[scsi_tgt_num];
	struct spdk_ssam_session *smsession = &ssmsession->smsession;
	spdk_ssam_session_rsp_fn rsp_fn = smsession->rsp_fn;
	void *rsp_ctx = smsession->rsp_ctx;

	smsession->rsp_fn = NULL;
	smsession->rsp_ctx = NULL;

	/* delete scsi port */
	spdk_scsi_dev_delete_port(state->dev, 0);

	/* destruct scsi dev */
	spdk_scsi_dev_destruct(state->dev, NULL, NULL);
	state->dev = NULL;

	/* free iostat */
	ssam_scsi_iostat_destruct(state);
	state->status = SSAM_SCSI_DEV_EMPTY;

	/* ref-- */
	if (ssmsession->ref > 0) {
		ssmsession->ref--;
	} else {
		SPDK_ERRLOG("%s: ref internel error\n", smsession->name);
	}
	if (rsp_fn != NULL) {
		rsp_fn(rsp_ctx, 0);
		rsp_fn = NULL;
	}
	SPDK_NOTICELOG("%s: target %u is removed\n", smsession->name, scsi_tgt_num);
}

static int
ssam_scsi_get_payload_size(struct ssam_request *io_req, uint32_t *payload_size)
{
	struct ssam_io_message *io_cmd = &io_req->req.cmd;
	uint32_t payload = 0;
	uint32_t first_vec;
	uint32_t end_vec;
	uint32_t loop;

	if (io_cmd->writable) { /* read io */
		/* FROM_DEV: [req][resp][write_buf]...[write_buf ]*, write_buf start at index 2 */
		first_vec = 2;
		end_vec = io_cmd->iovcnt - 1;
	} else { /* write io */
		first_vec = 1;
		/* TO_DEV: [req][read_buf]...[read_buf][resp], read_buf last index is iovnt-2 */
		end_vec = io_cmd->iovcnt - 2;
	}

	for (loop = first_vec; loop <= end_vec; loop++) {
		if (spdk_unlikely((UINT32_MAX - io_cmd->iovs[loop].iov_len) < payload)) {
			SPDK_ERRLOG("payload size overflow\n");
			return -1;
		}
		payload += io_cmd->iovs[loop].iov_len;
	}

	if (spdk_unlikely(payload > PAYLOAD_SIZE_MAX)) {
		SPDK_ERRLOG("payload size larger than %u, payload_size = %u\n",
			    PAYLOAD_SIZE_MAX, payload);
		return -1;
	}

	*payload_size = payload;

	return 0;
}

static void
ssam_session_io_resubmit(void *arg)
{
	struct spdk_ssam_scsi_task *task = (struct spdk_ssam_scsi_task *)arg;
	struct spdk_ssam_session *smsession = &task->ssmsession->smsession;
	uint32_t payload_size = task->scsi_task.transfer_len;
	int rc;

	rc = ssam_scsi_task_iovs_memory_get(task, payload_size);
	if (rc != 0) {
		ssam_session_insert_io_wait(smsession, &task->session_io_wait);
		return;
	}
	ssam_scsi_process_io_task(smsession, task);
}

static void
ssam_scsi_task_init(struct spdk_ssam_scsi_task *task)
{
	memset(&task->scsi_task, 0, sizeof(struct spdk_scsi_task));

	task->used = true;
	task->iovcnt = 0;
	task->io_req = NULL;
	task->session_io_wait.cb_fn = ssam_session_io_resubmit;
	task->session_io_wait.cb_arg = task;
}

static void
ssam_scsi_task_dma_request_para(struct ssam_dma_request *data_request,
				struct spdk_ssam_scsi_task *task,
				uint32_t type, uint8_t status)
{
	struct spdk_scsi_task *scsi_task = &task->scsi_task;
	struct ssam_io_message *io_cmd = NULL;
	struct spdk_ssam_dma_cb dma_cb = {
		.status = status,
		.req_dir = type,
		.gfunc_id = task->io_req->gfunc_id,
		.vq_idx = task->vq_idx,
		.task_idx = task->task_idx
	};

	io_cmd = &task->io_req->req.cmd;
	data_request->cb = (void *) * (uint64_t *)&dma_cb;
	data_request->gfunc_id = task->io_req->gfunc_id;
	data_request->flr_seq = task->io_req->flr_seq;
	data_request->direction = type;
	data_request->data_len = scsi_task->transfer_len;
	if (type == SSAM_REQUEST_DATA_STORE) {
		data_request->src = task->iovs.phys.sges;
		data_request->src_num = task->iovcnt;
		/* FROM_DEV: [req][resp][write_buf]...[write_buf ]*, write_buf start at index 2 */
		data_request->dst = &io_cmd->iovs[2];
		/* dma data iovs does not contain header and tail */
		data_request->dst_num = io_cmd->iovcnt - IOV_HEADER_TAIL_NUM;
	} else if (type == SSAM_REQUEST_DATA_LOAD) {
		data_request->src = &io_cmd->iovs[1];
		/* dma data iovs does not contain header and tail */
		data_request->src_num = io_cmd->iovcnt - IOV_HEADER_TAIL_NUM;
		data_request->dst = task->iovs.phys.sges;
		data_request->dst_num = task->iovcnt;
	}
}

static void
ssam_scsi_task_finish(struct spdk_ssam_scsi_task *task)
{
	struct spdk_ssam_session *smsession = task->smsession;
	struct spdk_ssam_virtqueue *vq = &smsession->virtqueue[task->vq_idx];

	if (smsession->task_cnt == 0) {
		SPDK_ERRLOG("%s: task count internel error\n", smsession->name);
		return;
	}

	task->io_req = NULL;

	if (task->iovs.virt.sges[0].iov_base != NULL) {
		ssam_mempool_free(smsession->mp, task->iovs.virt.sges[0].iov_base);
		task->iovs.virt.sges[0].iov_base = NULL;
	}

	memset(&task->iovs, 0, sizeof(task->iovs));

	task->iovcnt = 0;
	smsession->task_cnt--;
	task->used = false;
	vq->index[vq->index_l] = task->task_idx;
	vq->index_l = (vq->index_l + 1) & 0xFF;
	vq->use_num--;
}

static int
ssam_scsi_io_complete(struct spdk_ssam_dev *smdev, struct ssam_request *io_req, void *rsp_buf,
		      uint32_t rsp_len)
{
	struct ssam_io_message *io_cmd = &io_req->req.cmd;
	struct ssam_virtio_res *virtio_res = NULL;
	struct ssam_io_response io_resp;
	struct iovec io_vec;
	int rc;

	memset(&io_resp, 0, sizeof(io_resp));
	io_resp.gfunc_id = io_req->gfunc_id;
	io_resp.iocb_id = io_req->iocb_id;
	io_resp.status = io_req->status;
	io_resp.req = io_req;
	io_resp.flr_seq = io_req->flr_seq;

	virtio_res = (struct ssam_virtio_res *)&io_resp.data;
	virtio_res->iovs = &io_vec;
	if (io_cmd->writable) { /* FROM_DEV: [req][resp][write_buf]...[write_buf ] */
		virtio_res->iovs->iov_base = io_cmd->iovs[1].iov_base;
		virtio_res->iovs->iov_len = io_cmd->iovs[1].iov_len;
	} else {    /* TO_DEV: [req][read_buf]...[read_buf][resp] */
		virtio_res->iovs->iov_base = io_cmd->iovs[io_cmd->iovcnt - 1].iov_base;
		virtio_res->iovs->iov_len = io_cmd->iovs[io_cmd->iovcnt - 1].iov_len;
	}
	virtio_res->iovcnt = 1;
	virtio_res->rsp = rsp_buf;
	virtio_res->rsp_len = rsp_len;

	rc = ssam_io_complete(smdev->tid, &io_resp);
	if (rc != 0) {
		return rc;
	}

	ssam_dev_io_dec(smdev);
	return 0;
}

struct ssam_scsi_req_complete_arg {
	struct spdk_ssam_dev *smdev;
	struct ssam_request *io_req;
	uint8_t status;
};

static void
ssam_scsi_req_complete_cb(void *arg)
{
	struct ssam_scsi_req_complete_arg *cb_arg = (struct ssam_scsi_req_complete_arg *)arg;
	struct virtio_scsi_cmd_resp resp = {0};
	struct virtio_scsi_ctrl_tmf_resp tmf_resp = {0};
	int rc;

	if (spdk_unlikely(cb_arg->io_req->type == VMIO_TYPE_VIRTIO_SCSI_CTRL)) {
		tmf_resp.response = cb_arg->status;
		rc = ssam_scsi_io_complete(cb_arg->smdev, cb_arg->io_req, &tmf_resp,
					   sizeof(struct virtio_scsi_ctrl_tmf_resp));
	} else {
		resp.response = cb_arg->status;
		rc = ssam_scsi_io_complete(cb_arg->smdev, cb_arg->io_req, &resp,
					   sizeof(struct virtio_scsi_cmd_resp));
	}

	if (rc != 0) {
		struct spdk_ssam_session_io_wait_r *io_wait_r =
			calloc(1, sizeof(struct spdk_ssam_session_io_wait_r));
		if (io_wait_r == NULL) {
			SPDK_ERRLOG("calloc for io_wait_r failed\n");
			sleep(1);
			raise(SIGTERM);
		}
		io_wait_r->cb_fn = ssam_scsi_req_complete_cb;
		io_wait_r->cb_arg = cb_arg;
		ssam_session_insert_io_wait_r(cb_arg->smdev, io_wait_r);
		return;
	}
	free(cb_arg);
	cb_arg = NULL;
}

static void
ssam_scsi_req_complete(struct spdk_ssam_dev *smdev, struct ssam_request *io_req, uint8_t status)
{
	struct virtio_scsi_cmd_resp resp = {0};
	struct virtio_scsi_ctrl_tmf_resp tmf_resp = {0};
	int rc;

	if (spdk_unlikely(io_req->type == VMIO_TYPE_VIRTIO_SCSI_CTRL)) {
		tmf_resp.response = status;
		rc = ssam_scsi_io_complete(smdev, io_req, &tmf_resp, sizeof(struct virtio_scsi_ctrl_tmf_resp));
	} else {
		resp.response = status;
		rc = ssam_scsi_io_complete(smdev, io_req, &resp, sizeof(struct virtio_scsi_cmd_resp));
	}

	if (rc != 0) {
		struct spdk_ssam_session_io_wait_r *io_wait_r =
			calloc(1, sizeof(struct spdk_ssam_session_io_wait_r));
		struct ssam_scsi_req_complete_arg *cb_arg =
			calloc(1, sizeof(struct ssam_scsi_req_complete_arg));
		if (io_wait_r == NULL || cb_arg == NULL) {
			SPDK_ERRLOG("calloc for io_wait_r failed\n");
			sleep(1);
			raise(SIGTERM);
		}
		cb_arg->smdev = smdev;
		cb_arg->io_req = io_req;
		cb_arg->status = status;
		io_wait_r->cb_fn = ssam_scsi_req_complete_cb;
		io_wait_r->cb_arg = cb_arg;
		ssam_session_insert_io_wait_r(smdev, io_wait_r);
	}
}

static void
ssam_scsi_task_put(struct spdk_ssam_scsi_task *task)
{
	memset(&task->resp, 0, sizeof(task->resp));
	if (task->io_req->type != VMIO_TYPE_VIRTIO_SCSI_CTRL) {
		task->ssmsession->scsi_dev_state[task->tgt_id].flight_io--;
	}
	spdk_scsi_task_put(&task->scsi_task);
}

static void
ssam_scsi_submit_completion_cb(void *arg)
{
	struct spdk_ssam_scsi_task *task = (struct spdk_ssam_scsi_task *)arg;
	struct spdk_ssam_session *smsession = task->smsession;
	int rc;

	if (spdk_unlikely(task->io_req->type == VMIO_TYPE_VIRTIO_SCSI_CTRL)) {
		rc = ssam_scsi_io_complete(smsession->smdev, task->io_req, &task->tmf_resp,
					   sizeof(struct virtio_scsi_ctrl_tmf_resp));
	} else {
		rc = ssam_scsi_io_complete(smsession->smdev, task->io_req, &task->resp,
					   sizeof(struct virtio_scsi_cmd_resp));
	}

	if (rc != 0) {
		struct spdk_ssam_session_io_wait_r *io_wait_r =
			calloc(1, sizeof(struct spdk_ssam_session_io_wait_r));
		if (io_wait_r == NULL) {
			SPDK_ERRLOG("calloc for io_wait_r failed\n");
			sleep(1);
			raise(SIGTERM);
		}
		io_wait_r->cb_fn = ssam_scsi_submit_completion_cb;
		io_wait_r->cb_arg = task;
		ssam_session_insert_io_wait_r(smsession->smdev, io_wait_r);
		return;
	}
	ssam_scsi_task_stat_tick(&task->task_stat.complete_end_tsc);
	ssam_scsi_stat_statistics(task);

	/* after spdk_task_construct called, put task */
	ssam_scsi_task_put(task);
}

static void
ssam_scsi_submit_completion(struct spdk_ssam_scsi_task *task)
{
	struct spdk_ssam_session *smsession = task->smsession;
	struct ssam_request *io_req = task->io_req;
	int rc;

	ssam_scsi_task_stat_tick(&task->task_stat.complete_start_tsc);
	if (spdk_unlikely(io_req->type == VMIO_TYPE_VIRTIO_SCSI_CTRL)) {
		rc = ssam_scsi_io_complete(smsession->smdev, io_req, &task->tmf_resp,
					   sizeof(struct virtio_scsi_ctrl_tmf_resp));
	} else {
		rc = ssam_scsi_io_complete(smsession->smdev, io_req, &task->resp,
					   sizeof(struct virtio_scsi_cmd_resp));
	}

	if (rc != 0) {
		struct spdk_ssam_session_io_wait_r *io_wait_r =
			calloc(1, sizeof(struct spdk_ssam_session_io_wait_r));
		if (io_wait_r == NULL) {
			SPDK_ERRLOG("calloc for io_wait_r failed\n");
			sleep(1);
			raise(SIGTERM);
		}
		io_wait_r->cb_fn = ssam_scsi_submit_completion_cb;
		io_wait_r->cb_arg = task;
		ssam_session_insert_io_wait_r(smsession->smdev, io_wait_r);
		return;
	}
	ssam_scsi_task_stat_tick(&task->task_stat.complete_end_tsc);
	ssam_scsi_stat_statistics(task);

	/* after spdk_task_construct called, put task */
	ssam_scsi_task_put(task);
}

struct ssam_scsi_dma_data_request_arg {
	struct spdk_ssam_dev *smdev;
	struct spdk_ssam_scsi_task *task;
	struct ssam_dma_request dma_req;
};

static void
ssam_scsi_dma_data_request_cb(void *arg)
{
	struct ssam_scsi_dma_data_request_arg *cb_arg = (struct ssam_scsi_dma_data_request_arg *)arg;
	int ret = ssam_dma_data_request(cb_arg->smdev->tid, &cb_arg->dma_req);
	if (ret == -ENOMEM || ret == -EIO) {
		struct spdk_ssam_session_io_wait_r *io_wait_r =
			calloc(1, sizeof(struct spdk_ssam_session_io_wait_r));
		if (io_wait_r == NULL) {
			SPDK_ERRLOG("calloc for io_wait_r failed\n");
			sleep(1);
			raise(SIGTERM);
		}
		io_wait_r->cb_fn = ssam_scsi_dma_data_request_cb;
		io_wait_r->cb_arg = cb_arg;
		ssam_session_insert_io_wait_r(cb_arg->smdev, io_wait_r);
		return;
	}
	if (ret < 0) {
		SPDK_ERRLOG("ssam dma data request failed(%d)\n", ret);
		cb_arg->task->resp.response = VIRTIO_SCSI_S_FAILURE;
		ssam_scsi_submit_completion(cb_arg->task);
	}
	free(cb_arg);
	cb_arg = NULL;
}

static void
ssam_scsi_task_dma_request(struct spdk_ssam_scsi_task *task, enum data_request_dma_type data_dir)
{
	struct spdk_ssam_session *smsession = task->smsession;
	struct ssam_dma_request data_request = {0};
	int ret = 0;

	ssam_scsi_task_stat_tick(&task->task_stat.dma_start_tsc);
	task->ssmsession->scsi_dev_state[task->tgt_id].io_stat[0]->scsi_stat.dma_count++;

	switch (data_dir) {
	case SSAM_REQUEST_DATA_STORE:
		ssam_scsi_task_dma_request_para(&data_request, task, SSAM_REQUEST_DATA_STORE, 0);

		/* dma request: ipu -> Host */
		ret = ssam_dma_data_request(smsession->smdev->tid, &data_request);
		break;

	case SSAM_REQUEST_DATA_LOAD:
		ssam_scsi_task_dma_request_para(&data_request, task, SSAM_REQUEST_DATA_LOAD, 0);

		/* dma request: Host -> ipu */
		ret = ssam_dma_data_request(smsession->smdev->tid, &data_request);
		break;

	default:
		SPDK_ERRLOG("Invalid data dir: %u.\n", data_dir);
		break;
	}

	if (ret == -ENOMEM || ret == -EIO) {
		struct spdk_ssam_session_io_wait_r *io_wait_r =
			calloc(1, sizeof(struct spdk_ssam_session_io_wait_r));
		struct ssam_scsi_dma_data_request_arg *cb_arg =
			calloc(1, sizeof(struct ssam_scsi_dma_data_request_arg));
		if (io_wait_r == NULL || cb_arg == NULL) {
			SPDK_ERRLOG("calloc for io_wait_r failed\n");
			sleep(1);
			raise(SIGTERM);
		}
		cb_arg->smdev = smsession->smdev;
		cb_arg->dma_req = data_request;
		cb_arg->task = task;
		io_wait_r->cb_fn = ssam_scsi_dma_data_request_cb;
		io_wait_r->cb_arg = cb_arg;
		ssam_session_insert_io_wait_r(smsession->smdev, io_wait_r);
		return;
	}

	if (ret < 0) {
		SPDK_ERRLOG("ssam dma data request failed(%d)\n", ret);
		task->resp.response = VIRTIO_SCSI_S_FAILURE;
		ssam_scsi_submit_completion(task);
	}
}

static void
ssam_scsi_task_copy_resp(struct spdk_ssam_scsi_task *task)
{
	struct spdk_scsi_task *scsi_task = &task->scsi_task;

	if (spdk_unlikely(task->io_req->type == VMIO_TYPE_VIRTIO_SCSI_CTRL)) {
		task->tmf_resp.response = scsi_task->status;
	} else {
		task->resp.status = scsi_task->status;
		if (spdk_unlikely(scsi_task->sense_data_len > SSAM_SENSE_DATE_LEN)) {
			return;
		}
		if (scsi_task->status != SPDK_SCSI_STATUS_GOOD) {
			memcpy(task->resp.sense, scsi_task->sense_data, scsi_task->sense_data_len);
			task->resp.sense_len = scsi_task->sense_data_len;
		}

		if (scsi_task->transfer_len != scsi_task->length) {
			SPDK_ERRLOG("task transfer_len(%u) not equal length(%u), internel error.\n",
				    scsi_task->transfer_len, scsi_task->length);
		}

		task->resp.resid = scsi_task->length - scsi_task->data_transferred;
	}
}

static void
ssam_scsi_read_task_cpl_cb(struct spdk_scsi_task *scsi_task)
{
	if (spdk_unlikely(spdk_get_shutdown_sig_received())) {
		/*
		 * In the hot restart process, when this callback is triggered,
		 * the task and bdev_io memory may have been released.
		 * Therefore, task and bdev_io are not released in this scenario.
		 */
		return;
	}
	struct spdk_ssam_scsi_task *task = SPDK_CONTAINEROF(scsi_task, struct spdk_ssam_scsi_task,
					   scsi_task);
	int32_t tgt_id = task->tgt_id;
	int32_t lun_id = spdk_scsi_lun_get_id(scsi_task->lun);
	struct spdk_scsi_dev_io_state *io_stat = task->ssmsession->scsi_dev_state[tgt_id].io_stat[lun_id];

	/* Second part start of read */
	io_stat->submit_tsc = spdk_get_ticks();

	ssam_scsi_task_copy_resp(task);

	ssam_scsi_task_stat_tick(&task->task_stat.bdev_end_tsc);
	task->ssmsession->scsi_dev_state[task->tgt_id].io_stat[0]->scsi_stat.bdev_complete_count++;

	/* 1) Read request without data is no need to dma;
	   2) Read request failed just complete it.
	   */
	if (scsi_task->length == 0 || scsi_task->status != SPDK_SCSI_STATUS_GOOD) {
		ssam_scsi_submit_completion(task);
		return;
	}

	/* Dma data from IPU to HOST */
	ssam_scsi_task_dma_request(task, SSAM_REQUEST_DATA_STORE);

	return;
}

static void
ssam_scsi_write_task_cpl_cb(struct spdk_scsi_task *scsi_task)
{
	if (spdk_unlikely(spdk_get_shutdown_sig_received())) {
		/*
		 * In the hot restart process, when this callback is triggered,
		 * the task and bdev_io memory may have been released.
		 * Therefore, task and bdev_io are not released in this scenario.
		 */
		return;
	}
	struct spdk_ssam_scsi_task *task = SPDK_CONTAINEROF(scsi_task, struct spdk_ssam_scsi_task,
					   scsi_task);
	int32_t tgt_id = task->tgt_id;
	int32_t lun_id = spdk_scsi_lun_get_id(scsi_task->lun);
	struct spdk_scsi_dev_io_state *io_stat = task->ssmsession->scsi_dev_state[tgt_id].io_stat[lun_id];
	uint32_t payload_size = task->scsi_task.transfer_len;

	/* Second part start of write */
	io_stat->submit_tsc = spdk_get_ticks();

	/* copy result from spdk_scsi_task to spdk_ssam_scsi_task->resp */
	ssam_scsi_task_copy_resp(task);

	ssam_scsi_task_stat_tick(&task->task_stat.bdev_end_tsc);
	task->ssmsession->scsi_dev_state[task->tgt_id].io_stat[0]->scsi_stat.bdev_complete_count++;

	ssam_scsi_submit_completion(task);
	/* Second part end of write */
	io_stat->stat.write_latency_ticks += ssam_get_diff_tsc(io_stat->submit_tsc);
	io_stat->stat.bytes_written += payload_size;
	io_stat->stat.num_write_ops++;

	return;
}

static void
ssam_scsi_ctl_task_cpl_cb(struct spdk_scsi_task *scsi_task)
{
	struct spdk_ssam_scsi_task *task = SPDK_CONTAINEROF(scsi_task, struct spdk_ssam_scsi_task,
					   scsi_task);

	ssam_scsi_task_copy_resp(task);

	ssam_scsi_submit_completion(task);
}

static void
ssam_scsi_task_free_cb(struct spdk_scsi_task *scsi_task)
{
	struct spdk_ssam_scsi_task *task = SPDK_CONTAINEROF(scsi_task, struct spdk_ssam_scsi_task,
					   scsi_task);

	ssam_scsi_task_finish(task);
}

static int
ssam_scsi_task_init_target(struct spdk_ssam_scsi_task *task, const __u8 *lun)
{
	struct spdk_ssam_scsi_session *ssmsession = task->ssmsession;
	struct spdk_scsi_dev_ssam_state *state = NULL;
	int32_t lun_id = (((uint16_t)lun[2] << 8) | lun[3]) & 0x3FFF;
	int32_t tgt_id = lun[1];

	if (lun[0] != 1 || tgt_id >= SPDK_SSAM_SCSI_CTRLR_MAX_DEVS) {
		SPDK_ERRLOG("First byte must be 1 and second is target\n");
		ssmsession->smsession.smdev->discard_io_num++;
		return -1;
	}

	state = &ssmsession->scsi_dev_state[tgt_id];
	task->scsi_dev = state->dev;
	if (state->dev == NULL || state->status != SSAM_SCSI_DEV_PRESENT) {
		return -1;
	}

	task->tgt_id = tgt_id;
	task->scsi_task.target_port = spdk_scsi_dev_find_port_by_id(task->scsi_dev, 0);
	task->scsi_task.lun = spdk_scsi_dev_get_lun(state->dev, lun_id);
	if (task->scsi_task.lun == NULL) {
		SPDK_ERRLOG("Failed to init scsi task lun by lun_id(%d)\n", lun_id);
		return -1;
	}
	return 0;
}

static void
ssam_scsi_submit_io_task(struct spdk_ssam_scsi_task *task)
{
	task->resp.response = VIRTIO_SCSI_S_OK;

	ssam_scsi_task_stat_tick(&task->task_stat.bdev_start_tsc);
	spdk_scsi_dev_queue_task(task->scsi_dev, &task->scsi_task);
	task->ssmsession->scsi_dev_state[task->tgt_id].io_stat[0]->scsi_stat.bdev_count++;
	ssam_scsi_task_stat_tick(&task->task_stat.bdev_func_tsc);

	SPDK_DEBUGLOG(ssam_blk_data, "====== Task: task_idx %u submitted ======\n", task->task_idx);
}

static int
ssam_scsi_task_iovs_memory_get(struct spdk_ssam_scsi_task *task, uint32_t payload_size)
{
	struct ssam_mempool *mp = task->smsession->mp;
	void *buffer = NULL;
	uint64_t phys_addr = 0;
	uint32_t alloc_size;

	if (payload_size == 0) { /* A little strange */
		alloc_size = 1;   /* Alloc one iov at least */
	} else {
		alloc_size = payload_size;
	}

	buffer = ssam_mempool_alloc(mp, alloc_size, &phys_addr);
	if (spdk_unlikely(buffer == NULL)) {
		return -ENOMEM;
	}

	/* ssam request max IO size is PAYLOAD_SIZE_MAX, only use one iov to save data */
	task->iovs.virt.sges[0].iov_base = buffer;
	task->iovs.phys.sges[0].iov_base = (void *)phys_addr;
	task->iovs.virt.sges[0].iov_len = payload_size;
	task->iovs.phys.sges[0].iov_len = payload_size;
	task->iovcnt = 1;

	return 0;
}

static void
scsi_mgmt_task_submit(struct spdk_ssam_scsi_task *task, enum spdk_scsi_task_func func)
{
	task->tmf_resp.response = VIRTIO_SCSI_S_OK;
	task->scsi_task.function = func;
	spdk_scsi_dev_queue_mgmt_task(task->scsi_dev, &task->scsi_task);
}

static void
ssam_scsi_process_ctl_task(struct spdk_ssam_session *smsession, struct spdk_ssam_scsi_task *task)
{
	struct virtio_scsi_ctrl_tmf_req *ctrl_req = (struct virtio_scsi_ctrl_tmf_req *)
			task->io_req->req.cmd.header;
	int ret = 0;

	spdk_scsi_task_construct(&task->scsi_task, ssam_scsi_ctl_task_cpl_cb, ssam_scsi_task_free_cb);
	ret = ssam_scsi_task_init_target(task, ctrl_req->lun);
	if (ret < 0) {
		task->tmf_resp.response = VIRTIO_SCSI_S_FAILURE;
		ssam_scsi_submit_completion(task);
		return;
	}

	int32_t lun_id = spdk_scsi_lun_get_id(task->scsi_task.lun);
	struct spdk_scsi_dev_io_state *io_stat =
			task->ssmsession->scsi_dev_state[task->tgt_id].io_stat[lun_id];

	switch (ctrl_req->type) {
	case VIRTIO_SCSI_T_TMF:
		/* Check if we are processing a valid request */
		if (task->scsi_dev == NULL) {
			task->tmf_resp.response = VIRTIO_SCSI_S_BAD_TARGET;
			ssam_scsi_submit_completion(task);
			break;
		}

		switch (ctrl_req->subtype) {
		case VIRTIO_SCSI_T_TMF_LOGICAL_UNIT_RESET:
			/* Handle LUN reset */
			SPDK_DEBUGLOG(ssam_scsi, "%s: LUN reset\n", smsession->name);

			scsi_mgmt_task_submit(task, SPDK_SCSI_TASK_FUNC_LUN_RESET);
			return;
		default:
			task->tmf_resp.response = VIRTIO_SCSI_S_ABORTED;
			ssam_scsi_submit_completion(task);
			/* Unsupported command */
			SPDK_DEBUGLOG(ssam_scsi, "%s: unsupported TMF command %x\n",
				      smsession->name, ctrl_req->subtype);
			break;
		}
		break;

	case VIRTIO_SCSI_T_AN_QUERY:
	case VIRTIO_SCSI_T_AN_SUBSCRIBE:
		task->tmf_resp.response = VIRTIO_SCSI_S_ABORTED;
		ssam_scsi_submit_completion(task);
		break;

	default:
		SPDK_DEBUGLOG(ssam_scsi, "%s: Unsupported control command %x\n",
			      smsession->name, ctrl_req->type);
		io_stat->scsi_stat.fatal_ios++;
		break;
	}
}

static void
ssam_scsi_io_task_construct(struct spdk_ssam_scsi_task *task)
{
	struct spdk_scsi_task *scsi_task = &task->scsi_task;
	struct ssam_io_message *io_cmd = &task->io_req->req.cmd;

	if (io_cmd->writable) { /* read io */
		spdk_scsi_task_construct(scsi_task, ssam_scsi_read_task_cpl_cb, ssam_scsi_task_free_cb);
	} else { /* write io */
		spdk_scsi_task_construct(scsi_task, ssam_scsi_write_task_cpl_cb, ssam_scsi_task_free_cb);
	}
}

static int32_t
ssam_scsi_io_task_setup(struct spdk_ssam_scsi_task *task)
{
	struct spdk_scsi_task *scsi_task = &task->scsi_task;
	struct ssam_io_message *io_cmd = &task->io_req->req.cmd;
	struct virtio_scsi_cmd_req *req = (struct virtio_scsi_cmd_req *)io_cmd->header;
	uint32_t payload_size;
	int ret;

	ssam_scsi_io_task_construct(task);

	ret = ssam_scsi_get_payload_size(task->io_req, &payload_size);
	if (ret != 0) {
		return ret;
	}

	ret = ssam_scsi_task_init_target(task, req->lun);
	if (ret < 0) {
		return ret;
	}

	scsi_task->dxfer_dir = (io_cmd->writable ? SPDK_SCSI_DIR_FROM_DEV : SPDK_SCSI_DIR_TO_DEV);
	scsi_task->iovs = task->iovs.virt.sges;
	scsi_task->cdb = req->cdb;
	scsi_task->transfer_len = payload_size;
	scsi_task->length = payload_size;

	ret = ssam_scsi_task_iovs_memory_get(task, payload_size);
	if (ret != 0) {
		return ret;
	}

	return 0;
}

static void
ssam_scsi_process_io_task(struct spdk_ssam_session *smsession, struct spdk_ssam_scsi_task *task)
{
	struct spdk_ssam_scsi_session *ssmsession = (struct spdk_ssam_scsi_session *)smsession;
	struct spdk_scsi_dev_io_state *io_stat;
	uint64_t cur_tsc;
	int32_t lun_id;

	ssmsession->scsi_dev_state[task->tgt_id].flight_io++;

	if (spdk_unlikely(task->scsi_task.lun == NULL)) {
		spdk_scsi_task_process_null_lun(&task->scsi_task);
		task->resp.response = VIRTIO_SCSI_S_OK;
		ssam_scsi_submit_completion(task);
		return;
	}

	lun_id = spdk_scsi_lun_get_id(task->scsi_task.lun);
	io_stat = ssmsession->scsi_dev_state[task->tgt_id].io_stat[lun_id];
	if (io_stat == NULL) {
		SPDK_ERRLOG("No io_stat with tgt %d lun %d\n", task->tgt_id, lun_id);
		task->resp.response = VIRTIO_SCSI_S_FAILURE;
		ssam_scsi_submit_completion(task);
		return;
	}
	/* First part start of read and write */
	cur_tsc = spdk_get_ticks();
	io_stat->submit_tsc = cur_tsc;
	memset(&task->task_stat, 0, sizeof(task->task_stat));
	task->task_stat.start_tsc = cur_tsc;
	io_stat->scsi_stat.start_count++;

	switch (task->scsi_task.dxfer_dir) {
	case SPDK_SCSI_DIR_FROM_DEV: /* read: read data from backend to ipu, then dma to host */
		ssam_scsi_submit_io_task(task);
		/* First part end of read */
		uint8_t rw_type = task->scsi_task.cdb[0];
		if (rw_type == SPDK_SBC_READ_6 || rw_type == SPDK_SBC_READ_10 ||
		    rw_type == SPDK_SBC_READ_12 || rw_type == SPDK_SBC_READ_16) {
			io_stat->stat.read_latency_ticks += ssam_get_diff_tsc(io_stat->submit_tsc);
			io_stat->stat.bytes_read += task->scsi_task.transfer_len;
			io_stat->stat.num_read_ops++;
		}
		break;

	case SPDK_SCSI_DIR_TO_DEV: /* write: dma data from host to ipu, then submit to backend */
		ssam_scsi_task_dma_request(task, SSAM_REQUEST_DATA_LOAD);
		break;

	default:
		SPDK_ERRLOG("scsi task dxfer dir error, dir is %u.\n", task->scsi_task.dxfer_dir);
		io_stat->scsi_stat.fatal_ios++;
		break;
	}
}

static void
ssam_scsi_pre_process_io_task(struct spdk_ssam_session *smsession, struct spdk_ssam_scsi_task *task)
{
	int ret;
	struct spdk_ssam_scsi_session *ssmsession = (struct spdk_ssam_scsi_session *)smsession;

	ret = ssam_scsi_io_task_setup(task);
	if (ret != 0) {
		if (ret == -ENOMEM) {
			ssam_session_insert_io_wait(smsession, &task->session_io_wait);
			return;
		}
		task->resp.response = VIRTIO_SCSI_S_FAILURE;
		ssmsession->scsi_dev_state[task->tgt_id].flight_io++;
		ssam_scsi_submit_completion(task);
		return;
	}

	ssam_scsi_process_io_task(smsession, task);
}

static void
ssam_scsi_process_request(struct spdk_ssam_session *smsession, struct ssam_request *io_req,
			  uint16_t vq_idx)
{
	struct spdk_ssam_scsi_task *task = NULL;
	struct spdk_ssam_virtqueue *vq = &smsession->virtqueue[vq_idx];

	if (spdk_unlikely(vq->use_num >= vq->num)) {
		SPDK_ERRLOG("Session:%s vq(%hu) task_cnt(%u) limit(%u).\n", smsession->name, vq_idx, vq->use_num,
			    vq->num);
		ssam_scsi_req_complete(smsession->smdev, io_req, VIRTIO_SCSI_S_FAILURE);
		return;
	}

	uint32_t index = vq->index[vq->index_r];
	task = &((struct spdk_ssam_scsi_task *)vq->tasks)[index];
	if (spdk_unlikely(task->used)) {
		SPDK_ERRLOG("%s: vq(%hu) task_idx(%hu) is already pending.\n", smsession->name, vq_idx,
			    task->task_idx);
		ssam_scsi_req_complete(smsession->smdev, io_req, VIRTIO_SCSI_S_FAILURE);
		return;
	}

	smsession->task_cnt++;
	vq->index_r = (vq->index_r + 1) & 0xFF;
	vq->use_num++;
	ssam_scsi_task_init(task);
	task->io_req = io_req;

	if (spdk_unlikely(io_req->type == VMIO_TYPE_VIRTIO_SCSI_CTRL)) {
		ssam_scsi_process_ctl_task(smsession, task);
	} else {
		ssam_scsi_pre_process_io_task(smsession, task);
	}

	return;
}

static void
ssam_scsi_request_worker(struct spdk_ssam_session *smsession, void *arg)
{
	struct ssam_request *io_req = (struct ssam_request *)arg;
	struct ssam_io_message *io_cmd = &io_req->req.cmd;
	struct spdk_ssam_dev *smdev = smsession->smdev;
	struct virtio_scsi_cmd_req *req = (struct virtio_scsi_cmd_req *)io_cmd->header;
	struct spdk_ssam_scsi_session *ssmsession = (struct spdk_ssam_scsi_session *)smsession;
	uint16_t vq_idx = io_cmd->virtio.vq_idx;
	uint32_t tgt_id = req->lun[1];

	smdev->io_num++;

	if (vq_idx >= smsession->max_queues) {
		SPDK_ERRLOG("vq_idx out of range, need less than %u, actually %u\n",
			    smsession->max_queues, vq_idx);
		goto err;
	}

	if (io_req->status != SSAM_IO_STATUS_OK) {
		SPDK_WARNLOG("%s: ssam request status invalid, but still process, status=%d\n",
			     smsession->name, io_req->status);
		goto err;
	}

	if (ssmsession->scsi_dev_state[tgt_id].status != SSAM_SCSI_DEV_PRESENT) {
		/* If dev has been deleted, return io err */
		goto err;
	}

	ssam_scsi_process_request(smsession, io_req, vq_idx);

	return;

err:
	ssam_scsi_req_complete(smsession->smdev, io_req, VIRTIO_SCSI_S_FAILURE);
	return;
}

static void
ssam_scsi_response_worker(struct spdk_ssam_session *smsession, void *arg)
{
	struct ssam_dma_rsp *dma_rsp = (struct ssam_dma_rsp *)arg;
	const struct spdk_ssam_dma_cb *dma_cb = (const struct spdk_ssam_dma_cb *)&dma_rsp->cb;
	struct spdk_ssam_scsi_task *task = NULL;
	uint16_t vq_idx = dma_cb->vq_idx;
	uint16_t task_idx = dma_cb->task_idx;
	uint8_t req_dir = dma_cb->req_dir;

	if (spdk_unlikely(vq_idx >= smsession->max_queues)) {
		smsession->smdev->discard_io_num++;
		SPDK_ERRLOG("vq_idx out of range, need less than %u, actually %u\n",
			    smsession->max_queues, vq_idx);
		return;
	}

	task = &((struct spdk_ssam_scsi_task *)smsession->virtqueue[vq_idx].tasks)[task_idx];
	if (dma_rsp->status != 0) {
		task->resp.response = VIRTIO_SCSI_S_FAILURE;
		ssam_scsi_submit_completion(task);
		SPDK_ERRLOG("dma data process failed!\n");
		return;
	}
	if (dma_rsp->last_flag == 0) {
		task->resp.response = VIRTIO_SCSI_S_FAILURE;
		ssam_scsi_submit_completion(task);
		SPDK_ERRLOG("last_flag should not equal 0!\n");
		return;
	}
	int32_t tgt_id = task->tgt_id;
	int32_t lun_id = spdk_scsi_lun_get_id(task->scsi_task.lun);
	struct spdk_scsi_dev_io_state *io_stat = task->ssmsession->scsi_dev_state[tgt_id].io_stat[lun_id];

	ssam_scsi_task_stat_tick(&task->task_stat.dma_end_tsc);
	task->ssmsession->scsi_dev_state[task->tgt_id].io_stat[0]->scsi_stat.dma_complete_count++;

	if (req_dir == SSAM_REQUEST_DATA_LOAD) {
		/* Write: write data ready, submit task to backend */
		ssam_scsi_submit_io_task(task);
		/* First part end of write */
		io_stat->stat.write_latency_ticks += ssam_get_diff_tsc(io_stat->submit_tsc);
	} else if (req_dir == SSAM_REQUEST_DATA_STORE) {
		/* Read: data have been read by user, complete the task */
		task->resp.response = VIRTIO_SCSI_S_OK;
		ssam_scsi_submit_completion(task);
		/* Second part end of read */
		io_stat->stat.read_latency_ticks += ssam_get_diff_tsc(io_stat->submit_tsc);
	} else {
		io_stat->scsi_stat.fatal_ios++;
	}
}

static void
ssam_scsi_destroy_bdev_device(struct spdk_ssam_session *smsession, void *args)
{
	unsigned scsi_tgt_num = (unsigned)(uintptr_t)(args);
	struct spdk_ssam_scsi_session *ssmsession = (struct spdk_ssam_scsi_session *)smsession;

	ssam_remove_scsi_tgt(ssmsession, scsi_tgt_num);
}

static void
ssam_free_scsi_task_pool(struct spdk_ssam_scsi_session *ssmsession)
{
	struct spdk_ssam_session *smsession = &ssmsession->smsession;
	struct spdk_ssam_virtqueue *vq = NULL;
	uint16_t max_queues = smsession->max_queues;
	uint16_t i;

	if (max_queues > SPDK_SSAM_MAX_VQUEUES) {
		return;
	}

	for (i = 0; i < max_queues; i++) {
		vq = &smsession->virtqueue[i];
		if (vq->tasks != NULL) {
			spdk_free(vq->tasks);
			vq->tasks = NULL;
		}

		if (vq->index != NULL) {
			spdk_free(vq->index);
			vq->index = NULL;
		}
	}
}

static int
ssam_alloc_scsi_task_pool(struct spdk_ssam_scsi_session *ssmsession)
{
	struct spdk_ssam_session *smsession = &ssmsession->smsession;
	struct spdk_ssam_virtqueue *vq = NULL;
	struct spdk_ssam_scsi_task *task = NULL;
	uint16_t max_queues = smsession->max_queues;
	uint32_t task_cnt = smsession->queue_size;
	uint16_t i;
	uint32_t j;

	if ((max_queues > SPDK_SSAM_MAX_VQUEUES) || (max_queues == 0)) {
		SPDK_ERRLOG("%s: max_queues %u invalid\n", smsession->name, max_queues);
		return -EINVAL;
	}

	if ((task_cnt == 0) || (task_cnt > SPDK_SSAM_MAX_VQ_SIZE)) {
		SPDK_ERRLOG("%s: virtuque size %u invalid\n", smsession->name, task_cnt);
		return -EINVAL;
	}

	for (i = 0; i < max_queues; i++) {
		vq = &smsession->virtqueue[i];
		vq->smsession = smsession;
		vq->num = task_cnt;
		vq->use_num = 0;
		vq->index_l = 0;
		vq->index_r = 0;
		vq->tasks = spdk_zmalloc(sizeof(struct spdk_ssam_scsi_task) * task_cnt,
					 SPDK_CACHE_LINE_SIZE, NULL,
					 SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
		vq->index = spdk_zmalloc(sizeof(uint32_t) * task_cnt,
					 SPDK_CACHE_LINE_SIZE, NULL,
					 SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
		if (vq->tasks == NULL || vq->index == NULL) {
			SPDK_ERRLOG("%s: failed to allocate %"PRIu32" tasks for virtqueue %"PRIu16"\n",
				    smsession->name, task_cnt, i);
			ssam_free_scsi_task_pool(ssmsession);
			return -ENOMEM;
		}

		for (j = 0; j < task_cnt; j++) {
			task = &((struct spdk_ssam_scsi_task *)vq->tasks)[j];
			task->ssmsession = ssmsession;
			task->smsession = &ssmsession->smsession;
			task->vq_idx = i;
			task->task_idx = j;
			vq->index[j] = j;
		}
	}

	return 0;
}

static void
ssam_scsi_print_stuck_io_info(struct spdk_ssam_session *smsession)
{
	struct spdk_ssam_scsi_task *tasks;
	struct spdk_ssam_scsi_task *task;
	int i, j;

	for (i = 0; i < smsession->max_queues; i++) {
		for (j = 0; j < smsession->queue_size; j++) {
			tasks = (struct spdk_ssam_scsi_task *)smsession->virtqueue[i].tasks;
			task = &tasks[j];
			if (task == NULL) {
				continue;
			}
			if (task->used) {
				SPDK_INFOLOG(ssam_scsi, "%s: stuck io payload_size %u, vq_idx %u, task_idx %u\n",
					     smsession->name, task->scsi_task.length, task->vq_idx, task->task_idx);
			}
		}
	}
}

static int
ssam_scsi_start_cb(struct spdk_ssam_session *smsession, void **unused)
{
	SPDK_NOTICELOG("SCSI controller %s created with queues %u\n",
		       smsession->name, smsession->max_queues);

	ssam_session_start_done(smsession, 0);

	return 0;
}

static int
ssam_scsi_start(struct spdk_ssam_session *smsession)
{
	struct spdk_ssam_scsi_session *ssmsession = (struct spdk_ssam_scsi_session *)smsession;
	struct spdk_ssam_send_event_flag send_event_flag = {
		.need_async = false,
		.need_rsp = true,
	};
	int rc = ssam_alloc_scsi_task_pool(ssmsession);
	if (rc != 0) {
		SPDK_ERRLOG("%s: failed to alloc task pool.\n", smsession->name);
		return rc;
	}
	return ssam_send_event_to_session(smsession, ssam_scsi_start_cb, NULL, send_event_flag, NULL);
}

static int
ssam_scsi_session_connect(struct spdk_ssam_session *smsession, uint16_t queues)
{
	uint16_t queue_cnt = queues;

	if (queue_cnt == 0) {
		queue_cnt = SPDK_SSAM_SCSI_DEFAULT_VQUEUES;
	}

	smsession->max_queues = queue_cnt;
	smsession->queue_size = SPDK_SSAM_DEFAULT_VQ_SIZE;

	return ssam_scsi_start(smsession);
}

int
ssam_scsi_construct(struct spdk_ssam_session_reg_info *info)
{
	struct spdk_ssam_session *smsession = NULL;
	struct spdk_ssam_scsi_session *ssmsession = NULL;
	uint32_t session_ctx_size = sizeof(struct spdk_ssam_scsi_session) - sizeof(
					    struct spdk_ssam_session);
	uint16_t tid;
	int rc = 0;

	ssam_lock();

	tid = ssam_get_tid();
	if (tid == SPDK_INVALID_TID) {
		ssam_unlock();
		return -EINVAL;
	}

	info->tid = tid;
	info->backend = &g_ssam_scsi_session_backend;
	info->session_ctx_size = session_ctx_size;
	snprintf(info->type_name, SPDK_SESSION_TYPE_MAX_LEN, "%s", SPDK_SESSION_TYPE_SCSI);
	rc = ssam_session_register(info, &smsession);
	if (rc != 0) {
		ssam_unlock();
		return rc;
	}
	smsession->started = true;

	ssmsession = (struct spdk_ssam_scsi_session *)smsession;
	ssmsession->registered = true;
	ssmsession->dbdf = strdup(info->dbdf);
	if (ssmsession->dbdf == NULL) {
		ssam_session_unregister(smsession);
		ssam_unlock();
		return -EINVAL;
	}

	rc = ssam_scsi_session_connect(smsession, info->queues);
	if (rc != 0) {
		ssam_session_unreg_response_cb(smsession);
		ssam_session_unregister(smsession);
		ssam_unlock();
		return -EINVAL;
	}

	ssam_unlock();

	return 0;
}

static int
ssam_get_scsi_tgt_num(struct spdk_ssam_scsi_session *ssmsession, int *scsi_tgt_num_out)
{
	int scsi_tgt_num = *scsi_tgt_num_out;
	if (scsi_tgt_num < 0) {
		for (scsi_tgt_num = 0; scsi_tgt_num < SPDK_SSAM_SCSI_CTRLR_MAX_DEVS; scsi_tgt_num++) {
			if (ssmsession->scsi_dev_state[scsi_tgt_num].dev == NULL) {
				break;
			}
		}

		if (scsi_tgt_num == SPDK_SSAM_SCSI_CTRLR_MAX_DEVS) {
			SPDK_ERRLOG("%s: all SCSI target slots are already in use.\n", ssmsession->smsession.name);
			return -ENOSPC;
		}
	} else {
		if (scsi_tgt_num >= SPDK_SSAM_SCSI_CTRLR_MAX_DEVS) {
			SPDK_ERRLOG("%s: SCSI target number is too big (got %d, max %d)\n",
				    ssmsession->smsession.name, scsi_tgt_num, SPDK_SSAM_SCSI_CTRLR_MAX_DEVS - 1);
			return -EINVAL;
		}
	}
	*scsi_tgt_num_out = scsi_tgt_num;
	return 0;
}

static int
ssam_scsi_dev_param_changed(struct spdk_ssam_scsi_session *ssmsession,
			    unsigned scsi_tgt_num)
{
	struct spdk_scsi_dev_ssam_state *state = &ssmsession->scsi_dev_state[scsi_tgt_num];

	if (state->dev == NULL) {
		return 0;
	}
	int rc = ssam_scsi_send_event(&ssmsession->smsession, scsi_tgt_num, VIRTIO_SCSI_T_PARAM_CHANGE,
				      0x2a | (0x09 << 0x8));
	if (rc != 0) {
		SPDK_ERRLOG("%s: tgt %d resize send action failed\n", ssmsession->smsession.name, scsi_tgt_num);
		return rc;
	}

	return 0;
}

static unsigned
ssam_get_scsi_dev_num(const struct spdk_ssam_scsi_session *ssmsession,
		      const struct spdk_scsi_lun *lun)
{
	const struct spdk_scsi_dev *scsi_dev;
	unsigned scsi_dev_num;

	scsi_dev = spdk_scsi_lun_get_dev(lun);
	for (scsi_dev_num = 0; scsi_dev_num < SPDK_SSAM_SCSI_CTRLR_MAX_DEVS; scsi_dev_num++) {
		if (ssmsession->scsi_dev_state[scsi_dev_num].dev == scsi_dev) {
			break;
		}
	}
	return scsi_dev_num;
}

static void
ssam_scsi_lun_resize(const struct spdk_scsi_lun *lun, void *arg)
{
	struct spdk_ssam_scsi_session *ssmsession = arg;
	unsigned scsi_dev_num;

	scsi_dev_num = ssam_get_scsi_dev_num(ssmsession, lun);
	if (scsi_dev_num == SPDK_SSAM_SCSI_CTRLR_MAX_DEVS) {
		/* The entire device has been already removed. */
		return;
	}

	(void)ssam_scsi_dev_param_changed(ssmsession, scsi_dev_num);
}

static void
ssam_scsi_lun_hotremove(const struct spdk_scsi_lun *lun, void *arg)
{
	struct ssam_scsi_tgt_hotplug_ctx *ctx;
	struct spdk_ssam_scsi_session *ssmsession = arg;
	struct spdk_ssam_send_event_flag send_event_flag = {
		.need_async = false,
		.need_rsp = false,
	};
	unsigned scsi_dev_num;

	scsi_dev_num = ssam_get_scsi_dev_num(ssmsession, lun);
	if (scsi_dev_num == SPDK_SSAM_SCSI_CTRLR_MAX_DEVS) {
		/* The entire device has been already removed. */
		return;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		SPDK_ERRLOG("calloc failed\n");
		return;
	}

	ctx->scsi_tgt_num = scsi_dev_num;
	ssam_send_event_to_session(&ssmsession->smsession, ssam_scsi_dev_hot_remove_tgt,
				   NULL, send_event_flag, ctx);
}

static int
ssam_scsi_session_add_tgt(struct spdk_ssam_session *smsession, void **ctx)
{
	struct ssam_add_tgt_ev_ctx *args = (struct ssam_add_tgt_ev_ctx *)(*ctx);
	unsigned scsi_tgt_num = args->tgt_num;
	struct spdk_ssam_scsi_session *ssmsession = (struct spdk_ssam_scsi_session *)smsession;
	int rc;

	rc = spdk_scsi_dev_allocate_io_channels(ssmsession->scsi_dev_state[scsi_tgt_num].dev);
	if (rc != 0) {
		SPDK_ERRLOG("%s: Couldn't allocate io channnel for SCSI target %u.\n",
			    smsession->name, scsi_tgt_num);
	}

	rc = ssam_scsi_send_event(smsession, scsi_tgt_num, VIRTIO_SCSI_T_TRANSPORT_RESET,
				  VIRTIO_SCSI_EVT_RESET_RESCAN);
	if (rc != 0) {
		SPDK_WARNLOG("%s: send event %d(reason %d) to target %hu failed, ret: %d, host maynot boot.\n",
			     smsession->name, VIRTIO_SCSI_T_TRANSPORT_RESET, VIRTIO_SCSI_EVT_RESET_RESCAN, scsi_tgt_num, rc);
		if (rc == -ENOSPC) {
			spdk_scsi_dev_free_io_channels(ssmsession->scsi_dev_state[scsi_tgt_num].dev);
			ssam_scsi_destruct_tgt(ssmsession, scsi_tgt_num);
			return rc;
		}
	}

	ssmsession->scsi_dev_state[scsi_tgt_num].status = SSAM_SCSI_DEV_PRESENT;
	ssmsession->scsi_dev_state[scsi_tgt_num].flight_io = 0;

	return 0;
}

static void
ssam_scsi_dev_add_tgt_cpl_cb(struct spdk_ssam_session *smsession, void **ctx)
{
	struct spdk_ssam_scsi_session *ssmsession = (struct spdk_ssam_scsi_session *)smsession;
	struct ssam_add_tgt_ev_ctx *args = (struct ssam_add_tgt_ev_ctx *)(*ctx);
	unsigned scsi_tgt_num = args->tgt_num;
	ssmsession->ref++;

	SPDK_NOTICELOG("SCSI controller %s target %u added with bdev %s\n",
		       smsession->name, scsi_tgt_num, args->bdev_name);

	free(args->bdev_name);
	args->bdev_name = NULL;
	free(args);
}

struct ssam_scsi_session_remove_tgt_arg {
	struct spdk_ssam_session *smsession;
	unsigned scsi_tgt_num;
};

static void
ssam_scsi_session_remove_tgt_cpl(struct spdk_ssam_session *smsession, void **_ctx)
{
	struct ssam_scsi_tgt_hotplug_ctx *ctx = *_ctx;
	unsigned scsi_tgt_num = ctx->scsi_tgt_num;
	int rc;
	rc = ssam_umount_normal(smsession, ssam_scsi_tgtid_to_lunid(scsi_tgt_num));
	if (rc != 0) {
		SPDK_ERRLOG("%s: function umount failed when remove scsi tgt:%s.\n",
			    smsession->name, strerror(-rc));
	}
	free(ctx);
}

static int
ssam_scsi_session_remove_tgt(struct spdk_ssam_session *smsession, void **_ctx)
{
	struct ssam_scsi_tgt_hotplug_ctx *ctx = *_ctx;
	unsigned scsi_tgt_num = ctx->scsi_tgt_num;
	struct spdk_ssam_scsi_session *ssmsession = (struct spdk_ssam_scsi_session *)smsession;
	struct spdk_scsi_dev_ssam_state *state = &ssmsession->scsi_dev_state[scsi_tgt_num];
	int rc = 0;

	if (state->status != SSAM_SCSI_DEV_PRESENT) {
		SPDK_WARNLOG("%s: SCSI target %u is not present, skip.\n", smsession->name, scsi_tgt_num);
		rc = -ENODEV;
		goto out;
	}

	if (ssmsession->scsi_dev_state[scsi_tgt_num].flight_io != 0) {
		SPDK_ERRLOG("%s: SCSI target %u is busy.\n", smsession->name, scsi_tgt_num);
		rc = -EBUSY;
		goto out;
	}

	state->status = SSAM_SCSI_DEV_REMOVING;

	SPDK_NOTICELOG("%s: target %d is removing\n", smsession->name, scsi_tgt_num);

	rc = ssam_scsi_send_event(smsession, scsi_tgt_num, VIRTIO_SCSI_T_TRANSPORT_RESET,
				  VIRTIO_SCSI_EVT_RESET_REMOVED);
	if (rc != 0) {
		SPDK_ERRLOG("%s: scsi send remove event failed\n", smsession->name);
		if (rc == -ENOSPC) {
			state->status = SSAM_SCSI_DEV_PRESENT;
			goto out;
		}
	}

	spdk_scsi_dev_free_io_channels(state->dev);

	ssam_send_dev_destroy_msg(smsession, (void *)(uintptr_t)scsi_tgt_num);

	/* free ctx see ssam_scsi_session_remove_tgt_cpl() */
	return rc;

out:
	free(ctx);

	return rc;
}

static int
ssam_scsi_construct_tgt(struct spdk_ssam_scsi_session *ssmsession, int scsi_tgt_num,
			const char *bdev_name)
{
	struct spdk_scsi_dev_ssam_state *state = NULL;
	char target_name[SPDK_SCSI_DEV_MAX_NAME] = {0};
	int lun_id_list[SSAM_SPDK_SCSI_DEV_MAX_LUN];
	const char *bdev_names_list[SSAM_SPDK_SCSI_DEV_MAX_LUN];
	int rc;

	state = &ssmsession->scsi_dev_state[scsi_tgt_num];
	if (state->dev != NULL) {
		SPDK_ERRLOG("%s: SCSI target %u already occupied\n", ssmsession->smsession.name, scsi_tgt_num);
		return -EEXIST;
	}

	(void)snprintf(target_name, sizeof(target_name), "Target %u", scsi_tgt_num);
	lun_id_list[0] = 0;
	bdev_names_list[0] = (char *)bdev_name;

	state->status = SSAM_SCSI_DEV_ADDING;
	rc = ssam_scsi_iostat_construct(ssmsession, scsi_tgt_num, lun_id_list, SSAM_SPDK_SCSI_DEV_MAX_LUN);
	if (rc != 0) {
		return rc;
	}

	state->dev = spdk_scsi_dev_construct_ext(target_name, bdev_names_list, lun_id_list,
			SSAM_SPDK_SCSI_DEV_MAX_LUN,
			SPDK_SPC_PROTOCOL_IDENTIFIER_SAS,
			ssam_scsi_lun_resize, ssmsession,
			ssam_scsi_lun_hotremove, ssmsession);
	if (state->dev == NULL) {
		SPDK_ERRLOG("%s: couldn't create SCSI target %u using bdev '%s'\n",
			    ssmsession->smsession.name, scsi_tgt_num, bdev_name);
		rc = -EINVAL;
		goto dev_fail;
	}

	rc = spdk_scsi_dev_add_port(state->dev, 0, "ssam");
	if (rc != 0) {
		goto port_fail;
	}

	return rc;

port_fail:
	spdk_scsi_dev_destruct(state->dev, NULL, NULL);

dev_fail:
	ssam_scsi_iostat_destruct(state);

	return rc;
}

static void
ssam_scsi_destruct_tgt(struct spdk_ssam_scsi_session *ssmsession, int scsi_tgt_num)
{
	struct spdk_scsi_dev_ssam_state *state = NULL;
	state = &ssmsession->scsi_dev_state[scsi_tgt_num];

	if (state->dev) {
		spdk_scsi_dev_delete_port(state->dev, 0);
		spdk_scsi_dev_destruct(state->dev, NULL, NULL);
		state->dev = NULL;
	}
	ssam_scsi_iostat_destruct(state);

	state->status = SSAM_SCSI_DEV_EMPTY;
}

int
ssam_scsi_dev_add_tgt(struct spdk_ssam_session *smsession, int scsi_tgt_num,
		      const char *bdev_name)
{
	int rc;
	struct spdk_ssam_scsi_session *ssmsession = (struct spdk_ssam_scsi_session *)smsession;
	struct ssam_add_tgt_ev_ctx *ctx = calloc(1, sizeof(struct ssam_add_tgt_ev_ctx));
	struct spdk_ssam_send_event_flag send_event_flag = {
		.need_async = false,
		.need_rsp = true,
	};

	if (ctx == NULL) {
		SPDK_ERRLOG("calloc ssam_add_tgt_ev_ctx failed\n");
		return -ENOMEM;
	}

	if (bdev_name == NULL) {
		SPDK_ERRLOG("No lun name specified\n");
		free(ctx);
		return -EINVAL;
	}

	ctx->bdev_name = spdk_sprintf_alloc("%s", bdev_name);
	if (ctx->bdev_name == NULL) {
		SPDK_ERRLOG("calloc ssam_add_tgt_ev_ctx bdev_name failed\n");
		free(ctx);
		return -ENOMEM;
	}

	rc = ssam_get_scsi_tgt_num(ssmsession, &scsi_tgt_num);
	if (rc < 0) {
		free(ctx->bdev_name);
		free(ctx);
		return rc;
	}

	rc = ssam_mount_normal(smsession, ssam_scsi_tgtid_to_lunid(scsi_tgt_num));
	if (rc != SSAM_MOUNT_OK) {
		SPDK_ERRLOG("%s: mount ssam volume failed, tgt id %d\n", smsession->name, scsi_tgt_num);
		free(ctx->bdev_name);
		free(ctx);
		return rc;
	}

	rc = ssam_scsi_construct_tgt(ssmsession, scsi_tgt_num, bdev_name);
	if (rc != 0) {
		free(ctx->bdev_name);
		free(ctx);
		return rc;
	}

	ctx->tgt_num = scsi_tgt_num;
	rc = ssam_send_event_to_session(&ssmsession->smsession, ssam_scsi_session_add_tgt,
					ssam_scsi_dev_add_tgt_cpl_cb, send_event_flag, (void *)ctx);
	if (rc != 0) {
		ssam_scsi_destruct_tgt(ssmsession, scsi_tgt_num);
		free(ctx->bdev_name);
		free(ctx);
		return rc;
	}

	SPDK_INFOLOG(ssam_scsi, "%s: added SCSI target %u using bdev '%s'\n",
		     ssmsession->smsession.name, scsi_tgt_num, bdev_name);

	return 0;
}

static int
ssam_scsi_dev_hot_remove_tgt(struct spdk_ssam_session *smsession, void **_ctx)
{
	int rc = 0;
	struct ssam_scsi_tgt_hotplug_ctx *ctx = *_ctx;
	struct spdk_ssam_scsi_session *ssmsession;
	ssmsession = (struct spdk_ssam_scsi_session *)smsession;
	unsigned scsi_tgt_num = ctx->scsi_tgt_num;
	if (!ssmsession) {
		SPDK_ERRLOG("invalid SCSI device");
		rc = -EINVAL;
		goto out;
	}

	struct spdk_scsi_dev_ssam_state *scsi_dev_state = &ssmsession->scsi_dev_state[scsi_tgt_num];
	if (scsi_dev_state->dev == NULL) {
		/* Nothing to do */
		SPDK_WARNLOG("%s: There is no need to remove scsi target\n", smsession->name);
		rc = -ENODEV;
		goto out;
	}

	if (scsi_dev_state->status != SSAM_SCSI_DEV_PRESENT) {
		SPDK_INFOLOG(ssam_scsi, "%s: SCSI target %u is being removed\n", smsession->name, scsi_tgt_num);
		rc = 0;
		goto out;
	}

	scsi_dev_state->status = SSAM_SCSI_DEV_REMOVING;

	SPDK_NOTICELOG("%s: target %d is hot removing\n", smsession->name, scsi_tgt_num);

	rc = ssam_scsi_send_event(smsession, scsi_tgt_num, VIRTIO_SCSI_T_TRANSPORT_RESET,
				  VIRTIO_SCSI_EVT_RESET_REMOVED);
	if (rc != 0) {
		SPDK_ERRLOG("%s: scsi send remove event failed\n", smsession->name);
		if (rc == -ENOSPC) {
			scsi_dev_state->status = SSAM_SCSI_DEV_PRESENT;
			goto out;
		}
	}

	spdk_scsi_dev_free_io_channels(scsi_dev_state->dev);

	ssam_send_dev_destroy_msg(smsession, (void *)(uintptr_t)scsi_tgt_num);

out:
	free(ctx);
	return rc;
}

int
ssam_scsi_dev_remove_tgt(struct spdk_ssam_session *smsession, unsigned scsi_tgt_num,
			 spdk_ssam_session_rsp_fn cb_fn, void *cb_arg)
{
	struct spdk_ssam_scsi_session *ssmsession;
	struct ssam_scsi_tgt_hotplug_ctx *ctx;
	struct spdk_ssam_send_event_flag send_event_flag = {
		.need_async = false,
		.need_rsp = true,
	};

	if (scsi_tgt_num >= SPDK_SSAM_SCSI_CTRLR_MAX_DEVS) {
		SPDK_ERRLOG("%s: invalid SCSI target number %d\n", smsession->name, scsi_tgt_num);
		return -EINVAL;
	}

	ssmsession = (struct spdk_ssam_scsi_session *)smsession;
	if (!ssmsession) {
		SPDK_ERRLOG("An invalid SCSI device that removing from a SCSI target.");
		return -EINVAL;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		SPDK_ERRLOG("calloc failed\n");
		return -ENOMEM;
	}

	ctx->scsi_tgt_num = scsi_tgt_num;

	ssam_send_event_to_session(smsession, ssam_scsi_session_remove_tgt,
				   ssam_scsi_session_remove_tgt_cpl, send_event_flag, ctx);

	return 0;
}

SPDK_LOG_REGISTER_COMPONENT(ssam_scsi)
