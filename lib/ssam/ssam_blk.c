/*
 *   BSD LICENSE
 *   Copyright (c) Huawei Technologies Co., Ltd. 2021-2021. All rights reserved.
 *
 */

#include <rte_version.h>
#include <linux/virtio_blk.h>

#include "spdk/env.h"
#include "spdk/bdev.h"
#include "spdk/bdev_module.h"
#include "spdk/thread.h"
#include "spdk/likely.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/crc32.h"

#include "ssam_internal.h"

#define SESSION_STOP_POLLER_PERIOD  1000
#define ENQUEUE_TIMES_PER_IO        1000

#define IOV_HEADER_TAIL_NUM         2

#define SECTOR_SIZE                 512
#define ALIGNMENT_2M                (2048 * 1024)
#define SERIAL_STRING_LEN           128
#define SMSESSION_STOP_TIMEOUT      2       /* s */

/* Related to (SPDK_SSAM_IOVS_MAX * SPDK_SSAM_MAX_SEG_SIZE) */
#define PAYLOAD_SIZE_MAX               (2048U * 2048)

#define RETRY_TIMEOUT               120

/* Minimal set of features supported by every virtio-blk device */
#define SPDK_SSAM_BLK_FEATURES_BASE (SPDK_SSAM_FEATURES | \
        (1ULL << VIRTIO_BLK_F_SIZE_MAX) | (1ULL << VIRTIO_BLK_F_SEG_MAX) | \
        (1ULL << VIRTIO_BLK_F_GEOMETRY) | (1ULL << VIRTIO_BLK_F_BLK_SIZE) | \
        (1ULL << VIRTIO_BLK_F_TOPOLOGY) | (1ULL << VIRTIO_BLK_F_BARRIER)  | \
        (1ULL << VIRTIO_BLK_F_SCSI)     | (1ULL << VIRTIO_BLK_F_CONFIG_WCE) | \
        (1ULL << VIRTIO_BLK_F_MQ))

struct ssam_task_stat {
	uint64_t start_tsc;
	uint64_t dma_start_tsc;
	uint64_t dma_end_tsc;
	uint64_t bdev_start_tsc;
	uint64_t bdev_func_tsc;
	uint64_t bdev_end_tsc;
	uint64_t complete_start_tsc;
	uint64_t complete_end_tsc;
};

struct spdk_ssam_blk_task {
	/* Returned status of I/O processing, it can be VIRTIO_BLK_S_OK,
	 * VIRTIO_BLK_S_IOERR or VIRTIO_BLK_S_UNSUPP
	 */
	volatile uint8_t *status;

	/* Number of bytes processed successfully */
	uint32_t used_len;

	/* Records the amount of valid data in the struct iovec iovs array. */
	uint32_t iovcnt;
	struct ssam_iovec iovs;

	/* If set, the task is currently used for I/O processing. */
	bool used;

	/* For bdev io wait */
	struct spdk_bdev_io_wait_entry bdev_io_wait;
	struct spdk_ssam_session_io_wait session_io_wait;
	struct spdk_ssam_blk_session *bsmsession;

	/* Size of whole payload in bytes */
	uint32_t payload_size;

	/* ssam request data */
	struct ssam_request *io_req;

	uint16_t vq_idx;
	uint16_t req_idx;
	uint16_t task_idx;
	struct ssam_task_stat task_stat;
};

struct ssam_blk_stat {
	uint64_t count;
	uint64_t start_count;
	uint64_t total_tsc; /* pre_dma <- -> post_return */
	uint64_t dma_tsc;   /* pre_dma <- -> post_dma */
	uint64_t dma_count;
	uint64_t dma_complete_count;
	uint64_t bdev_tsc;   /* pre_bdev <- -> post_bdev */
	uint64_t bdev_submit_tsc;   /* <- spdk_bdev_xxx -> */
	uint64_t bdev_count;
	uint64_t bdev_complete_count;
	uint64_t complete_tsc;   /* pre_return <- -> post_return */
	uint64_t internel_tsc;  /* total_tsc - dma_tsc - bdev_tsc - complete_tsc */

	uint64_t complete_read_ios;     /* Number of successfully completed read requests */
	uint64_t err_read_ios;          /* Number of failed completed read requests */
	uint64_t complete_write_ios;    /* Number of successfully completed write requests */
	uint64_t err_write_ios;         /* Number of failed completed write requests */
	uint64_t flush_ios;             /* Total number of flush requests */
	uint64_t complete_flush_ios;    /* Number of successfully completed flush requests */
	uint64_t err_flush_ios;         /* Number of failed completed flush requests */
	uint64_t other_ios;
	uint64_t complete_other_ios;
	uint64_t err_other_ios;
	uint64_t fatal_ios;             /* Number of discarded requests */
	uint64_t io_retry;
};

struct spdk_ssam_blk_session {
	/* The parent session must be the very first field in this struct */
	struct spdk_ssam_session smsession;
	struct spdk_poller *stop_poller;
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *bdev_desc;
	struct spdk_io_channel *io_channel;

	/* volume id */
	char *serial;

	/* accumulated I/O statistics */
	struct spdk_bdev_io_stat stat;

	/* Current count of bdev operations for hot-restart. */
	int32_t bdev_count;

	/* poller for waiting bdev finish when hot-restart */
	struct spdk_poller *stop_bdev_poller;

	/* controller statistics. */
	struct ssam_blk_stat blk_stat;

	/* if set, all writes to the device will fail with
	 * VIRTIO_BLK_S_IOERR error code
	 */
	bool readonly;

	/* if set, indicate the session not have a bdev, all writes to the device
	 * will fail with VIRTIO_BLK_S_IOERR error code
	 */
	bool no_bdev;
};

struct ssam_blk_session_ctx {
	struct spdk_ssam_blk_session *bsmsession;
	void **user_ctx;
};

static const struct spdk_ssam_session_backend g_ssam_blk_session_backend;
static int ssam_blk_remove_session(struct spdk_ssam_session *smsession);
static void ssam_blk_request_worker(struct spdk_ssam_session *smsession, void *arg);
static void ssam_blk_destroy_bdev_device(struct spdk_ssam_session *smsession, void *args);
static void ssam_blk_response_worker(struct spdk_ssam_session *smsession, void *arg);
static void ssam_blk_no_data_request_worker(struct spdk_ssam_session *smsession);
static inline void ssam_request_queue_io(struct spdk_ssam_blk_task *task);
static void ssam_task_complete(struct spdk_ssam_blk_task *task, uint8_t status);
static void ssam_data_request_para(struct ssam_dma_request *dma_req,
				   struct spdk_ssam_blk_task *task, uint32_t type, uint8_t status);
static void ssam_blk_print_stuck_io_info(struct spdk_ssam_session *smsession);
static int ssam_process_blk_request(struct spdk_ssam_blk_task *task);
static void ssam_free_task_pool(struct spdk_ssam_blk_session *bsmsession);
static int ssam_blk_io_complete(struct spdk_ssam_dev *smdev, struct ssam_request *io_req,
				uint8_t status);
static void ssam_session_io_resubmit(void *arg);

static inline struct spdk_ssam_blk_session *
ssam_to_blk_session(struct spdk_ssam_session *smsession)
{
	return (struct spdk_ssam_blk_session *)smsession;
}

static void
ssam_blk_dump_info_json(struct spdk_ssam_session *smsession,
			struct spdk_json_write_ctx *w)
{
	struct spdk_ssam_blk_session *bsmsession = ssam_to_blk_session(smsession);

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "name", ssam_session_get_name(smsession));
	spdk_json_write_named_uint32(w, "function_id", (uint32_t)smsession->gfunc_id);
	spdk_json_write_named_uint32(w, "queues", (uint32_t)smsession->max_queues);

	spdk_json_write_named_object_begin(w, "block");
	spdk_json_write_named_bool(w, "readonly", bsmsession->readonly);
	spdk_json_write_name(w, "bdev");
	if (bsmsession->bdev != NULL) {
		spdk_json_write_string(w, spdk_bdev_get_name(bsmsession->bdev));
	} else {
		spdk_json_write_null(w);
	}
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

static void
ssam_dev_bdev_remove_cpl_cb(struct spdk_ssam_session *smsession, void **unnused)
{
	/* All sessions have been notified, time to close the bdev */
	struct spdk_ssam_blk_session *bsmsession = ssam_to_blk_session(smsession);

	if (bsmsession == NULL) {
		return;
	}

	if (bsmsession->bdev_desc != NULL) {
		spdk_bdev_close(bsmsession->bdev_desc);
		bsmsession->bdev_desc = NULL;
	}

	/* bdev not create by ssam blk, no need be freed here */
	bsmsession->bdev = NULL;
}

static void
ssam_blk_stop_cpl_cb(struct spdk_ssam_session *smsession, void **ctx)
{
	struct spdk_ssam_blk_session *bsmsession = ssam_to_blk_session(smsession);
	spdk_ssam_session_rsp_fn rsp_fn = smsession->rsp_fn;
	void *rsp_ctx = smsession->rsp_ctx;
	int rc;

	ssam_dev_bdev_remove_cpl_cb(smsession, NULL);
	rc = ssam_virtio_blk_resize(smsession->gfunc_id, 0);
	if (rc != 0) {
		SPDK_WARNLOG("%s: virtio blk resize failed when remove session.\n", smsession->name);
	}

	/* Can not umount function here, whenever the gfunc_id must be mounted to
	 * the dummy tid or to the specific tid
	 */

	SPDK_NOTICELOG("BLK controller %s deleted\n", smsession->name);

	if (smsession->name != NULL) {
		free(smsession->name);
		smsession->name = NULL;
	}

	ssam_set_session_be_freed(ctx);
	memset(bsmsession, 0, sizeof(*bsmsession));
	free(bsmsession);

	if (rsp_fn != NULL) {
		rsp_fn(rsp_ctx, 0);
		rsp_fn = NULL;
	}
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
ssam_blk_stat_statistics(struct spdk_ssam_blk_task *task, uint8_t status)
{
#ifdef PERF_STAT
	struct spdk_ssam_blk_session *bsmsession = task->bsmsession;
	uint64_t dma_tsc = task->task_stat.dma_end_tsc - task->task_stat.dma_start_tsc;
	uint64_t bdev_tsc = task->task_stat.bdev_end_tsc - task->task_stat.bdev_start_tsc;
	uint64_t bdev_submit_tsc = task->task_stat.bdev_func_tsc - task->task_stat.bdev_start_tsc;
	uint64_t complete_tsc = task->task_stat.complete_end_tsc - task->task_stat.complete_start_tsc;
	uint64_t total_tsc = task->task_stat.complete_end_tsc - task->task_stat.start_tsc;
	struct virtio_blk_outhdr *req = (struct virtio_blk_outhdr *)task->io_req->req.cmd.header;

	if (req->type == VIRTIO_BLK_T_IN) {   /* read */
		bsmsession->stat.read_latency_ticks += total_tsc;
		bsmsession->stat.bytes_read += task->payload_size;
		bsmsession->stat.num_read_ops++;
		if (status == VIRTIO_BLK_S_OK) {
			bsmsession->blk_stat.complete_read_ios++;
		} else {
			bsmsession->blk_stat.err_read_ios++;
		}
	} else if (req->type == VIRTIO_BLK_T_OUT) {    /* write */
		bsmsession->stat.write_latency_ticks += total_tsc;
		bsmsession->stat.bytes_written += task->payload_size;
		bsmsession->stat.num_write_ops++;
		if (status == VIRTIO_BLK_S_OK) {
			bsmsession->blk_stat.complete_write_ios++;
		} else {
			bsmsession->blk_stat.err_write_ios++;
		}
	} else if (req->type == VIRTIO_BLK_T_FLUSH) {   /* flush */
		bsmsession->blk_stat.flush_ios++;
		if (status == VIRTIO_BLK_S_OK) {
			bsmsession->blk_stat.complete_flush_ios++;
		} else {
			bsmsession->blk_stat.err_flush_ios++;
		}
	} else {
		bsmsession->blk_stat.other_ios++;
		if (status == VIRTIO_BLK_S_OK) {
			bsmsession->blk_stat.complete_other_ios++;
		} else {
			bsmsession->blk_stat.err_other_ios++;
		}
	}

	bsmsession->blk_stat.dma_tsc += dma_tsc;
	bsmsession->blk_stat.bdev_tsc += bdev_tsc;
	bsmsession->blk_stat.bdev_submit_tsc += bdev_submit_tsc;
	bsmsession->blk_stat.complete_tsc += complete_tsc;
	bsmsession->blk_stat.total_tsc += total_tsc;
	bsmsession->blk_stat.internel_tsc += total_tsc - complete_tsc - bdev_tsc - dma_tsc;
	bsmsession->blk_stat.count += 1;
#endif
}

static void
ssam_blk_configs(uint8_t *config, struct virtio_blk_config *blkcfg,
		 uint32_t len, struct spdk_bdev *bdev)
{
	uint32_t cfg_len;

	/* minimum I/O size in blocks */
	blkcfg->min_io_size = 1;

	if (bdev && spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_UNMAP)) {
		/* 32768 sectors is 16MiB, expressed in 512 Bytes */
		blkcfg->max_discard_sectors = 32768;
		blkcfg->max_discard_seg = 1;
		/* expressed in 512 Bytes sectors */
		blkcfg->discard_sector_alignment = blkcfg->blk_size / SECTOR_SIZE;
	}
	if (bdev && spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_WRITE_ZEROES)) {
		/* 32768 sectors is 16MiB, expressed in 512 Bytes */
		blkcfg->max_write_zeroes_sectors = 32768;
		blkcfg->max_write_zeroes_seg = 1;
	}

	cfg_len = sizeof(struct virtio_blk_config);
	memcpy(config, blkcfg, (unsigned long)spdk_min(len, cfg_len));
	if (len < cfg_len) {
		SPDK_NOTICELOG("Out config len %u < total config len %u\n", len, cfg_len);
	}

	return;
}

static int
ssam_blk_get_config(struct spdk_ssam_session *smsession, uint8_t *config,
		    uint32_t len, uint16_t queues)
{
	struct virtio_blk_config blkcfg;
	struct spdk_ssam_blk_session *bsmsession = NULL;
	struct spdk_bdev *bdev = NULL;
	uint32_t blk_size;
	uint64_t blkcnt;

	memset(&blkcfg, 0, sizeof(blkcfg));
	bsmsession = ssam_to_blk_session(smsession);
	if (bsmsession == NULL) {
		SPDK_ERRLOG("session is null.\n");
		return -1;
	}
	bdev = bsmsession->bdev;
	if (bdev == NULL) {
		return -1;
	}
	blk_size = spdk_bdev_get_block_size(bdev);
	blkcnt = spdk_bdev_get_num_blocks(bdev);
	/* ssam will use this configuration, this is the max capability of
	 * the ssam, configurations will be obtained through negotiation
	 * in the future.
	 */
	blkcfg.size_max = SPDK_SSAM_MAX_SEG_SIZE;
	blkcfg.seg_max = SPDK_SSAM_IOVS_MAX;

	if (blk_size == 0) {
		SPDK_ERRLOG("bdev's blk_size %u error.\n", blk_size);
		return -1;
	}
	if (blkcnt > (UINT64_MAX / blk_size)) {
		SPDK_ERRLOG("bdev's blkcnt %lu or blk_size %u out of range.\n",
			    blkcnt, blk_size);
		return -1;
	}
	blkcfg.blk_size = blk_size;
	/* expressed in 512 Bytes sectors */
	blkcfg.capacity = (blkcnt * blk_size) / 512;
	blkcfg.num_queues = 1; /* TODO: 1 change to queues after the VBS problem is fixed */
	ssam_blk_configs(config, &blkcfg, len, bdev);

	return 0;
}

static void
ssam_blk_write_config_json(struct spdk_ssam_session *smsession,
			   struct spdk_json_write_ctx *w)
{
	struct spdk_ssam_blk_session *bsmsession = ssam_to_blk_session(smsession);

	if (bsmsession == NULL || bsmsession->bdev == NULL) {
		return;
	}

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "method", "create_blk_controller");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "dev_name", spdk_bdev_get_name(bsmsession->bdev));
	char *gfunc_id = spdk_sprintf_alloc("%u", bsmsession->smsession.gfunc_id);
	if (gfunc_id == NULL) {
		SPDK_ERRLOG("alloc for gfunc_id failed\n");
	} else {
		spdk_json_write_named_string(w, "index", gfunc_id);
		free(gfunc_id);
	}
	spdk_json_write_named_bool(w, "readonly", bsmsession->readonly);
	if (bsmsession->serial != NULL) {
		spdk_json_write_named_string(w, "serial", bsmsession->serial);
	}
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

static void
ssam_blk_show_iostat_json(struct spdk_ssam_session *smsession, uint32_t id,
			  struct spdk_json_write_ctx *w)
{
	struct spdk_ssam_blk_session *bsmsession = ssam_to_blk_session(smsession);
	struct spdk_bdev *bdev = ssam_get_session_bdev(smsession);
	struct spdk_bdev_io_stat stat = {0};
	struct ssam_blk_stat blk_stat;
	uint64_t ticks_hz = spdk_get_ticks_hz();
	uint64_t poll_count = smsession->smdev->stat.poll_count;

	memcpy(&stat, &bsmsession->stat, sizeof(struct spdk_bdev_io_stat));  /* a little question, mutex */
	memcpy(&blk_stat, &bsmsession->blk_stat, sizeof(struct ssam_blk_stat));
	spdk_json_write_object_begin(w);

	spdk_json_write_named_uint32(w, "function_id", smsession->gfunc_id);
	if (smsession->smdev->stat.poll_count == 0) {
		poll_count = 1;
	}
	spdk_json_write_named_string_fmt(w, "poll_lat", "%.9f",
					 (float)smsession->smdev->stat.poll_tsc / poll_count / ticks_hz);
	spdk_json_write_named_string(w, "bdev_name", (bdev == NULL) ? "" : spdk_bdev_get_name(bdev));
	spdk_json_write_named_uint64(w, "bytes_read", stat.bytes_read);
	spdk_json_write_named_uint64(w, "num_read_ops", stat.num_read_ops);
	spdk_json_write_named_uint64(w, "bytes_written", stat.bytes_written);
	spdk_json_write_named_uint64(w, "num_write_ops", stat.num_write_ops);
	spdk_json_write_named_uint64(w, "read_latency_ticks", stat.read_latency_ticks);
	spdk_json_write_named_uint64(w, "write_latency_ticks", stat.write_latency_ticks);
	spdk_json_write_named_uint64(w, "complete_read_ios", blk_stat.complete_read_ios);
	spdk_json_write_named_uint64(w, "err_read_ios", blk_stat.err_read_ios);
	spdk_json_write_named_uint64(w, "complete_write_ios", blk_stat.complete_write_ios);
	spdk_json_write_named_uint64(w, "err_write_ios", blk_stat.err_write_ios);
	spdk_json_write_named_uint64(w, "flush_ios", blk_stat.flush_ios);
	spdk_json_write_named_uint64(w, "complete_flush_ios", blk_stat.complete_flush_ios);
	spdk_json_write_named_uint64(w, "err_flush_ios", blk_stat.err_flush_ios);
	spdk_json_write_named_uint64(w, "other_ios", blk_stat.other_ios);
	spdk_json_write_named_uint64(w, "complete_other_ios", blk_stat.complete_other_ios);
	spdk_json_write_named_uint64(w, "err_other_ios", blk_stat.err_other_ios);

	spdk_json_write_named_uint64(w, "fatal_ios", blk_stat.fatal_ios);
	spdk_json_write_named_uint64(w, "io_retry", blk_stat.io_retry);
	spdk_json_write_named_object_begin(w, "counters");
	spdk_json_write_named_uint64(w, "start_count", blk_stat.start_count);
	spdk_json_write_named_uint64(w, "dma_count", blk_stat.dma_count);
	spdk_json_write_named_uint64(w, "dma_complete_count", blk_stat.dma_complete_count);
	spdk_json_write_named_uint64(w, "bdev_count", blk_stat.bdev_count);
	spdk_json_write_named_uint64(w, "bdev_complete_count", blk_stat.bdev_complete_count);
	spdk_json_write_object_end(w);
	spdk_json_write_named_object_begin(w, "details");
	spdk_json_write_named_uint64(w, "count", blk_stat.count);
	if (blk_stat.count == 0) {
		blk_stat.count = 1;
	}
	spdk_json_write_named_string_fmt(w, "total_lat", "%.9f",
					 (float)blk_stat.total_tsc / blk_stat.count / ticks_hz);
	spdk_json_write_named_string_fmt(w, "dma_lat", "%.9f",
					 (float)blk_stat.dma_tsc / blk_stat.count / ticks_hz);
	spdk_json_write_named_string_fmt(w, "bdev_lat", "%.9f",
					 (float)blk_stat.bdev_tsc / blk_stat.count / ticks_hz);
	spdk_json_write_named_string_fmt(w, "bdev_submit_lat", "%.9f",
					 (float)blk_stat.bdev_submit_tsc / blk_stat.count / ticks_hz);
	spdk_json_write_named_string_fmt(w, "complete_lat", "%.9f",
					 (float)blk_stat.complete_tsc / blk_stat.count / ticks_hz);
	spdk_json_write_named_string_fmt(w, "internal_lat", "%.9f",
					 (float)blk_stat.internel_tsc / blk_stat.count / ticks_hz);
	spdk_json_write_object_end(w);
	spdk_json_write_object_end(w);
}

static void
ssam_blk_clear_iostat_json(struct spdk_ssam_session *smsession)
{
	struct spdk_ssam_blk_session *bsmsession = ssam_to_blk_session(smsession);
	memset(&bsmsession->stat, 0, sizeof(struct spdk_bdev_io_stat) - sizeof(
		       uint64_t));  /* exclude ticks_rate */
	memset(&bsmsession->blk_stat, 0, sizeof(struct ssam_blk_stat));
}

static struct spdk_bdev *ssam_blk_get_bdev(struct spdk_ssam_session *smsession, uint32_t id)
{
	struct spdk_bdev *bdev = ssam_get_session_bdev(smsession);

	return bdev;
}

static const struct spdk_ssam_session_backend g_ssam_blk_session_backend = {
	.type = VIRTIO_TYPE_BLK,
	.remove_session = ssam_blk_remove_session,
	.request_worker = ssam_blk_request_worker,
	.destroy_bdev_device = ssam_blk_destroy_bdev_device,
	.response_worker = ssam_blk_response_worker,
	.no_data_req_worker = ssam_blk_no_data_request_worker,
	.ssam_get_config = ssam_blk_get_config,
	.print_stuck_io_info = ssam_blk_print_stuck_io_info,
	.dump_info_json = ssam_blk_dump_info_json,
	.write_config_json = ssam_blk_write_config_json,
	.show_iostat_json = ssam_blk_show_iostat_json,
	.clear_iostat_json = ssam_blk_clear_iostat_json,
	.get_bdev = ssam_blk_get_bdev,
	.remove_self = NULL,
};

static void
ssam_blk_io_crc_print(const char *context, void *arg)
{
	struct  spdk_ssam_blk_task *task = (struct spdk_ssam_blk_task *)arg;
	const struct virtio_blk_outhdr *req = (struct virtio_blk_outhdr *)task->io_req->req.cmd.header;
	uint32_t crc = spdk_crc32c_update(task->iovs.virt.sges[0].iov_base, task->iovs.virt.sges[0].iov_len, 0);

	SPDK_NOTICELOG("ssam crc blk %s: tid=%u gfunc_id=%u vqid=%u reqid=%u reqtype=%"PRIu32" rw=%u"
		" offset=%"PRIu64" length=%"PRIu32" crc=%"PRIu32".\n",
		context,
		task->bsmsession->smsession.smdev->tid,
		task->bsmsession->smsession.gfunc_id,
		task->io_req->req.cmd.virtio.vq_idx,
		task->io_req->req.cmd.virtio.req_idx,
		req->type,
		task->io_req->req.cmd.writable,
		req->sector * SECTOR_SIZE,
		task->payload_size,
		crc);
}

/* Clean Smsession */
static int
ssam_destroy_poller_cb(void *arg)
{
	struct spdk_ssam_blk_session *bsmsession = (struct spdk_ssam_blk_session *)arg;
	struct spdk_ssam_session *smsession = &bsmsession->smsession;
	struct spdk_ssam_dev *smdev = smsession->smdev;

	SPDK_NOTICELOG("%s: remaining %u tasks\n", smsession->name, smsession->task_cnt);

	/* stop poller */
	spdk_poller_unregister(&bsmsession->stop_bdev_poller);

	/* remove session */
	ssam_sessions_remove(smdev->smsessions, smsession);
	smdev->active_session_num--;
	smsession->smdev = NULL;

	/* put ioChannle */
	if (bsmsession->io_channel != NULL) {
		spdk_put_io_channel(bsmsession->io_channel);
		bsmsession->io_channel = NULL;
	}

	/* close bdev device, last step, async */
	ssam_send_dev_destroy_msg(smsession, NULL);

	/* free smsession not here, but after close bdev device; */
	/* see ssam_blk_destroy_bdev_device() */

	return SPDK_POLLER_BUSY;
}

static int
ssam_session_bdev_remove_cb(struct spdk_ssam_session *smsession, void **ctx)
{
	struct spdk_ssam_blk_session *bsmsession = ssam_to_blk_session(smsession);
	int rc = 0;

	/* smsession already removed */
	if (!smsession->started) {
		return 0;
	} else {
		smsession->started = false;
	}

	bsmsession->stop_bdev_poller = SPDK_POLLER_REGISTER(ssam_destroy_poller_cb,
				       bsmsession, 0);

	rc = ssam_virtio_blk_resize(smsession->gfunc_id, 0);
	if (rc != 0) {
		SPDK_WARNLOG("%s: virtio blk resize failed when remove session.\n", smsession->name);
	}

	ssam_set_session_be_freed(ctx);
	ssam_send_event_async_done(ctx);

	return 0;
}

static void
ssam_bdev_remove_cb(void *remove_ctx)
{
	struct spdk_ssam_session *smsession = remove_ctx;
	struct spdk_ssam_send_event_flag send_event_flag = {
		.need_async = false,
		.need_rsp = true,
	};

	SPDK_WARNLOG("%s: hot-removing bdev - all further requests will be stucked.\n",
		     smsession->name);

	ssam_send_event_to_session(smsession, ssam_session_bdev_remove_cb,
				   NULL, send_event_flag, NULL);
}

static void
ssam_session_bdev_resize_cb(struct spdk_ssam_session *smsession, void **ctx)
{
	struct spdk_ssam_blk_session *bsmsession = ssam_to_blk_session(smsession);
	int rc;

	rc = ssam_virtio_blk_resize(smsession->gfunc_id, bsmsession->bdev->blockcnt);
	if (rc != 0) {
		SPDK_WARNLOG("%s: virtio blk resize failed.\n", smsession->name);
	}
}

static void
ssam_blk_resize_cb(void *resize_ctx)
{
	struct spdk_ssam_session *smsession = resize_ctx;
	struct spdk_ssam_send_event_flag send_event_flag = {
		.need_async = false,
		.need_rsp = true,
	};

	ssam_send_event_to_session(smsession, NULL, ssam_session_bdev_resize_cb, send_event_flag, NULL);
}

static void
ssam_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
		   void *event_ctx)
{
	SPDK_DEBUGLOG(ssam_blk, "Bdev event: type %d, name %s\n",
		      type, bdev->name);

	switch (type) {
	case SPDK_BDEV_EVENT_REMOVE:
		SPDK_NOTICELOG("bdev name (%s) received event(SPDK_BDEV_EVENT_REMOVE)\n",
			       bdev->name);
		ssam_bdev_remove_cb(event_ctx);
		break;
	case SPDK_BDEV_EVENT_RESIZE:
		SPDK_NOTICELOG("bdev name (%s) received event(SPDK_BDEV_EVENT_RESIZE)\n",
			       bdev->name);
		ssam_blk_resize_cb(event_ctx);
		break;
	default:
		SPDK_NOTICELOG("Unsupported bdev event: type %d\n", type);
		break;
	}
}

static void
ssam_free_task_pool(struct spdk_ssam_blk_session *bsmsession)
{
	struct spdk_ssam_session *smsession = &bsmsession->smsession;
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
ssam_alloc_task_pool(struct spdk_ssam_blk_session *bsmsession)
{
	struct spdk_ssam_session *smsession = &bsmsession->smsession;
	struct spdk_ssam_virtqueue *vq = NULL;
	struct spdk_ssam_blk_task *task = NULL;
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
		vq->tasks = spdk_zmalloc(sizeof(struct spdk_ssam_blk_task) * task_cnt,
					 SPDK_CACHE_LINE_SIZE, NULL,
					 SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
		vq->index = spdk_zmalloc(sizeof(uint32_t) * task_cnt,
					 SPDK_CACHE_LINE_SIZE, NULL,
					 SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
		if (vq->tasks == NULL || vq->index == NULL) {
			SPDK_ERRLOG("%s: failed to allocate %"PRIu32" tasks for virtqueue %"PRIu16"\n",
				    smsession->name, task_cnt, i);
			ssam_free_task_pool(bsmsession);
			return -ENOMEM;
		}
		for (j = 0; j < task_cnt; j++) {
			task = &((struct spdk_ssam_blk_task *)vq->tasks)[j];
			task->bsmsession = bsmsession;
			task->task_idx = j;
			vq->index[j] = j;
		}
	}

	return 0;
}

static void
ssam_blk_print_stuck_io_info(struct spdk_ssam_session *smsession)
{
	struct spdk_ssam_blk_task *tasks;
	struct spdk_ssam_blk_task *task;
	int i, j;

	for (i = 0; i < smsession->max_queues; i++) {
		for (j = 0; j < smsession->queue_size; j++) {
			tasks = (struct spdk_ssam_blk_task *)smsession->virtqueue[i].tasks;
			task = &tasks[j];
			if (task == NULL) {
				continue;
			}
			if (task->used) {
				SPDK_INFOLOG(ssam_blk, "%s: stuck io payload_size %u, vq_idx %u, req_idx %u\n",
					     smsession->name, task->payload_size, task->vq_idx, task->req_idx);
			}
		}
	}
}

static uint16_t
get_req_idx(struct spdk_ssam_blk_task *task)
{
	return task->io_req->req.cmd.virtio.req_idx;
}

static void
ssam_blk_task_init(struct spdk_ssam_blk_task *task)
{
	task->used = true;
	task->iovcnt = 0;
	task->io_req = NULL;
	task->payload_size = 0;
	memset(&task->task_stat, 0, sizeof(task->task_stat));
	ssam_task_stat_tick(&task->task_stat.start_tsc);
}

static void
ssam_blk_task_finish(struct spdk_ssam_blk_task *task)
{
	struct spdk_ssam_session *smsession = &task->bsmsession->smsession;
	struct spdk_ssam_virtqueue *vq = &smsession->virtqueue[task->vq_idx];

	if (smsession->task_cnt == 0) {
		SPDK_ERRLOG("smsession %s: task internel error\n", smsession->name);
		return;
	}

	task->io_req = NULL;
	task->payload_size = 0;

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
ssam_blk_io_complete(struct spdk_ssam_dev *smdev, struct ssam_request *io_req, uint8_t status)
{
	struct ssam_io_response io_resp;
	struct ssam_virtio_res *virtio_res = (struct ssam_virtio_res *)&io_resp.data;
	struct ssam_io_message *io_cmd = &io_req->req.cmd;
	struct iovec io_vec;
	uint8_t res_status = status;
	int rc;

	if (status != VIRTIO_BLK_S_OK) {
		SPDK_ERRLOG("ssam io complete return error tid=%u gfunc_id:%u.\n", smdev->tid, io_req->gfunc_id);
	}

	memset(&io_resp, 0, sizeof(io_resp));
	io_resp.gfunc_id = io_req->gfunc_id;
	io_resp.iocb_id = io_req->iocb_id;
	io_resp.status = io_req->status;
	io_resp.req = io_req;
	io_resp.flr_seq = io_req->flr_seq;

	virtio_res->iovs = &io_vec;
	virtio_res->iovs->iov_base = io_cmd->iovs[io_cmd->iovcnt - 1].iov_base;
	virtio_res->iovs->iov_len = io_cmd->iovs[io_cmd->iovcnt - 1].iov_len;
	virtio_res->iovcnt = 1;
	virtio_res->rsp = &res_status;
	virtio_res->rsp_len = sizeof(res_status);

	rc = ssam_io_complete(smdev->tid, &io_resp);
	if (rc != 0) {
		return rc;
	}

	ssam_dev_io_dec(smdev);
	return 0;
}

struct ssam_task_complete_arg {
	struct spdk_ssam_blk_task *task;
	uint8_t status;
};

static void
ssam_task_complete_cb(void *arg)
{
	struct ssam_task_complete_arg *cb_arg = (struct ssam_task_complete_arg *)arg;
	struct spdk_ssam_session *smsession = &cb_arg->task->bsmsession->smsession;
	struct spdk_ssam_blk_task *task = cb_arg->task;
	int rc = ssam_blk_io_complete(smsession->smdev, task->io_req, cb_arg->status);
	if (rc != 0) {
		struct spdk_ssam_session_io_wait_r *io_wait_r =
			calloc(1, sizeof(struct spdk_ssam_session_io_wait_r));
		if (io_wait_r == NULL) {
			SPDK_ERRLOG("calloc for io_wait_r failed\n");
			sleep(1);
			raise(SIGTERM);
		}
		io_wait_r->cb_fn = ssam_task_complete_cb;
		io_wait_r->cb_arg = cb_arg;
		ssam_session_insert_io_wait_r(smsession->smdev, io_wait_r);
		return;
	}
	ssam_task_stat_tick(&task->task_stat.complete_end_tsc);
	ssam_blk_stat_statistics(task, cb_arg->status);
	ssam_blk_task_finish(task);
	free(cb_arg);
	cb_arg = NULL;
}

static void
ssam_task_complete(struct spdk_ssam_blk_task *task, uint8_t status)
{
	struct spdk_ssam_session *smsession = &task->bsmsession->smsession;
	if (status != VIRTIO_BLK_S_OK) {
		SPDK_ERRLOG("ssam task return error tid=%u gfunc_id:%u.\n",
			    smsession->smdev->tid, task->io_req->gfunc_id);
	}
	SPDK_INFOLOG(ssam_blk_data, "handled io tid=%u gfunc_id=%u rw=%u vqid=%u reqid=%u status=%u.\n",
		     smsession->smdev->tid, smsession->gfunc_id, task->io_req->req.cmd.writable,
		     task->io_req->req.cmd.virtio.vq_idx, task->io_req->req.cmd.virtio.req_idx, status);
	ssam_task_stat_tick(&task->task_stat.complete_start_tsc);
	int rc = ssam_blk_io_complete(smsession->smdev, task->io_req, status);
	if (rc != 0) {
		struct spdk_ssam_session_io_wait_r *io_wait_r =
			calloc(1, sizeof(struct spdk_ssam_session_io_wait_r));
		struct ssam_task_complete_arg *cb_arg =
			calloc(1, sizeof(struct ssam_task_complete_arg));
		if (io_wait_r == NULL || cb_arg == NULL) {
			SPDK_ERRLOG("calloc for io_wait_r failed\n");
			sleep(1);
			raise(SIGTERM);
		}
		cb_arg->status = status;
		cb_arg->task = task;
		io_wait_r->cb_fn = ssam_task_complete_cb;
		io_wait_r->cb_arg = cb_arg;
		ssam_session_insert_io_wait_r(smsession->smdev, io_wait_r);
		return;
	}
	ssam_task_stat_tick(&task->task_stat.complete_end_tsc);
	ssam_blk_stat_statistics(task, status);
	ssam_blk_task_finish(task);
}

struct ssam_blk_dma_data_request_arg {
	struct spdk_ssam_dev *smdev;
	struct spdk_ssam_blk_task *task;
	struct ssam_dma_request dma_req;
};

static void
ssam_blk_dma_data_request_cb(void *arg)
{
	struct ssam_blk_dma_data_request_arg *cb_arg = (struct ssam_blk_dma_data_request_arg *)arg;
	int ret = ssam_dma_data_request(cb_arg->smdev->tid, &cb_arg->dma_req);
	if (ret == -ENOMEM || ret == -EIO) {
		struct spdk_ssam_session_io_wait_r *io_wait_r =
			calloc(1, sizeof(struct spdk_ssam_session_io_wait_r));
		if (io_wait_r == NULL) {
			SPDK_ERRLOG("calloc for io_wait_r failed\n");
			sleep(1);
			raise(SIGTERM);
		}
		io_wait_r->cb_fn = ssam_blk_dma_data_request_cb;
		io_wait_r->cb_arg = cb_arg;
		ssam_session_insert_io_wait_r(cb_arg->smdev, io_wait_r);
		return;
	}
	if (ret < 0) {
		SPDK_ERRLOG("%s: ssam dma data request failed:%s\n",
			    cb_arg->task->bsmsession->smsession.name, spdk_strerror(-ret));
		ssam_task_complete(cb_arg->task, VIRTIO_BLK_S_IOERR);
	}
	free(cb_arg);
	cb_arg = NULL;
}

static void
ssam_res_dma_process(struct spdk_ssam_session *smsession,
		     struct spdk_ssam_blk_task *task, uint32_t type, uint8_t status)
{
	struct ssam_dma_request dma_req = {0};
	uint16_t tid = smsession->smdev->tid;
	int ret;

	ssam_data_request_para(&dma_req, task, type, status);
	ssam_task_stat_tick(&task->task_stat.dma_start_tsc);
	task->bsmsession->blk_stat.dma_count++;
	ret = ssam_dma_data_request(tid, &dma_req);
	if (ret == -ENOMEM || ret == -EIO) {
		struct spdk_ssam_session_io_wait_r *io_wait_r =
			calloc(1, sizeof(struct spdk_ssam_session_io_wait_r));
		struct ssam_blk_dma_data_request_arg *cb_arg =
			calloc(1, sizeof(struct ssam_blk_dma_data_request_arg));
		if (io_wait_r == NULL || cb_arg == NULL) {
			SPDK_ERRLOG("calloc for io_wait_r failed\n");
			sleep(1);
			raise(SIGTERM);
		}
		cb_arg->smdev = smsession->smdev;
		cb_arg->dma_req = dma_req;
		cb_arg->task = task;
		io_wait_r->cb_fn = ssam_blk_dma_data_request_cb;
		io_wait_r->cb_arg = cb_arg;
		ssam_session_insert_io_wait_r(smsession->smdev, io_wait_r);
		return;
	}

	if (ret < 0) {
		SPDK_ERRLOG("%s: ssam dma data request failed:%s\n", smsession->name, spdk_strerror(-ret));
		ssam_task_complete(task, VIRTIO_BLK_S_IOERR);
	}
}

static void
ssam_blk_request_finish(bool success, struct spdk_ssam_blk_task *task)
{
	uint8_t res_status = success ? VIRTIO_BLK_S_OK : VIRTIO_BLK_S_IOERR;
	const struct virtio_blk_outhdr *req = NULL;
	struct spdk_ssam_session *smsession = &task->bsmsession->smsession;
	if (res_status != VIRTIO_BLK_S_OK) {
		SPDK_ERRLOG("request finish return error gfunc_id=%u.\n", smsession->gfunc_id);
	}

	req = (struct virtio_blk_outhdr *)task->io_req->req.cmd.header;
	switch (req->type) {
	case VIRTIO_BLK_T_IN:
	case VIRTIO_BLK_T_GET_ID:
		ssam_io_crc_check_log("read bdev complete", ssam_blk_io_crc_print, task);

		ssam_res_dma_process(smsession, task, SSAM_REQUEST_DATA_STORE, res_status);
		break;

	case VIRTIO_BLK_T_OUT:
	case VIRTIO_BLK_T_DISCARD:
	case VIRTIO_BLK_T_WRITE_ZEROES:
	case VIRTIO_BLK_T_FLUSH:
		ssam_task_complete(task, res_status);
		break;

	default:
		ssam_task_complete(task, VIRTIO_BLK_S_IOERR);
		SPDK_ERRLOG("Not supported request type '%"PRIu32"'.\n", req->type);
		break;
	}
}

static void
ssam_blk_req_complete_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_ssam_blk_task *task = cb_arg;

	if (spdk_unlikely(spdk_get_shutdown_sig_received())) {
		/*
		 * In the hot restart process, when this callback is triggered,
		 * the task and bdev_io memory may have been released.
		 * Therefore, task and bdev_io are not released in this scenario.
		 */
		return;
	}

	/* Second part start of read and write */
	SPDK_INFOLOG(ssam_blk_data,
		     "backend io finish tid=%u gfunc_id=%u rw=%u vqid=%u reqid=%u success=%d.\n",
		     task->bsmsession->smsession.smdev->tid, task->bsmsession->smsession.gfunc_id,
		     task->io_req->req.cmd.writable, task->io_req->req.cmd.virtio.vq_idx,
		     task->io_req->req.cmd.virtio.req_idx,
		     success);
	task->bsmsession->bdev_count--;
	task->bsmsession->blk_stat.bdev_complete_count++;
	ssam_task_stat_tick(&task->task_stat.bdev_end_tsc);

	spdk_bdev_free_io(bdev_io);
	ssam_blk_request_finish(success, task);
}

static int
ssam_request_rc_process(int rc, struct spdk_ssam_blk_task *task)
{
	if (rc == 0) {
		return rc;
	}

	if (rc == -ENOMEM) {
		SPDK_WARNLOG("No memory, start to queue io.\n");
		ssam_request_queue_io(task);
	} else {
		SPDK_ERRLOG("IO error, gfunc_id=%u.\n", task->bsmsession->smsession.gfunc_id);
		ssam_task_complete(task, VIRTIO_BLK_S_IOERR);
		return -1;
	}

	return rc;
}

static bool
ssam_is_req_sector_err(uint64_t sector)
{
	if (sector > (UINT64_MAX / SECTOR_SIZE)) {
		SPDK_ERRLOG("req sector out of range, need less or equal than %lu, actually %lu\n",
			    (UINT64_MAX / SECTOR_SIZE), sector);
		return true;
	}

	return false;
}

static int
ssam_virtio_read_write_process(struct spdk_ssam_blk_task *task,
			       const struct virtio_blk_outhdr *req)
{
	struct spdk_ssam_blk_session *bsmsession = task->bsmsession;
	struct ssam_io_message *io_cmd = NULL;
	uint32_t payload_size = task->payload_size;
	int rc;

	io_cmd = &task->io_req->req.cmd;

	if (ssam_is_req_sector_err(req->sector)) {
		SPDK_ERRLOG("rw check sector error, gfunc_id=%u.\n", bsmsession->smsession.gfunc_id);
		ssam_task_complete(task, VIRTIO_BLK_S_IOERR);
		return -1;
	}

	if (spdk_unlikely(payload_size == 0 || (payload_size & (SECTOR_SIZE - 1)) != 0)) {
		SPDK_ERRLOG("%s - passed IO buffer is not multiple of 512 Bytes (req_idx = %"PRIu16"), "
			    "payload_size = %u, iovcnt = %u.\n", req->type ? "WRITE" : "READ",
			    get_req_idx(task), payload_size, io_cmd->iovcnt);
		ssam_task_complete(task, VIRTIO_BLK_S_UNSUPP);
		return -1;
	}

	if (req->type == VIRTIO_BLK_T_IN) {
		bsmsession->bdev_count++;
		ssam_task_stat_tick(&task->task_stat.bdev_start_tsc);
		rc = spdk_bdev_readv(bsmsession->bdev_desc, bsmsession->io_channel,
				     task->iovs.virt.sges, task->iovcnt, req->sector * SECTOR_SIZE,
				     payload_size, ssam_blk_req_complete_cb, task);
		ssam_task_stat_tick(&task->task_stat.bdev_func_tsc);
	} else if (!bsmsession->readonly) {
		bsmsession->bdev_count++;
		ssam_task_stat_tick(&task->task_stat.bdev_start_tsc);
		rc = spdk_bdev_writev(bsmsession->bdev_desc, bsmsession->io_channel,
				      task->iovs.virt.sges, task->iovcnt, req->sector * SECTOR_SIZE,
				      payload_size, ssam_blk_req_complete_cb, task);
		ssam_task_stat_tick(&task->task_stat.bdev_func_tsc);

		ssam_io_crc_check_log("write bdev submit", ssam_blk_io_crc_print, task);
	} else {
		SPDK_DEBUGLOG(ssam_blk, "Device is in read-only mode!\n");
		rc = -1;
	}

	return ssam_request_rc_process(rc, task);
}

static int
ssam_virtio_discard_process(struct spdk_ssam_blk_task *task)
{
	int rc;
	struct spdk_ssam_blk_session *bsmsession = task->bsmsession;
	struct virtio_blk_discard_write_zeroes *desc = task->iovs.virt.sges[0].iov_base;

	if (ssam_is_req_sector_err(desc->sector)) {
		SPDK_ERRLOG("discard check sector error, gfunc_id=%u.\n", bsmsession->smsession.gfunc_id);
		ssam_task_complete(task, VIRTIO_BLK_S_IOERR);
		return -1;
	}

	if (task->payload_size != sizeof(*desc)) {
		SPDK_ERRLOG("Invalid discard payload size: %u\n", task->payload_size);
		ssam_task_complete(task, VIRTIO_BLK_S_IOERR);
		return -1;
	}

	if (desc->flags & VIRTIO_BLK_WRITE_ZEROES_FLAG_UNMAP) {
		SPDK_ERRLOG("UNMAP flag is only used for WRITE ZEROES command\n");
		ssam_task_complete(task, VIRTIO_BLK_S_UNSUPP);
		return -1;
	}
	bsmsession->bdev_count++;
	rc = spdk_bdev_unmap(bsmsession->bdev_desc, bsmsession->io_channel,
			     desc->sector * SECTOR_SIZE, desc->num_sectors * SECTOR_SIZE,
			     ssam_blk_req_complete_cb, task);

	return ssam_request_rc_process(rc, task);
}

static int
ssam_virtio_write_zeroes_process(struct spdk_ssam_blk_task *task)
{
	int rc;
	struct spdk_ssam_blk_session *bsmsession = task->bsmsession;
	struct virtio_blk_discard_write_zeroes *desc = task->iovs.virt.sges[0].iov_base;

	if (ssam_is_req_sector_err(desc->sector)) {
		SPDK_ERRLOG("write zeros check sector error, gfunc_id=%u.\n", bsmsession->smsession.gfunc_id);
		ssam_task_complete(task, VIRTIO_BLK_S_IOERR);
		return -1;
	}

	if (task->payload_size != sizeof(*desc)) {
		SPDK_NOTICELOG("Invalid write zeroes payload size: %u\n", task->payload_size);
		ssam_task_complete(task, VIRTIO_BLK_S_IOERR);
		return -1;
	}

	if (desc->flags & VIRTIO_BLK_WRITE_ZEROES_FLAG_UNMAP) {
		SPDK_WARNLOG("Ignore the unmap flag for WRITE ZEROES from %"PRIx64", len %"PRIx64"\n",
			     (uint64_t)desc->sector * SECTOR_SIZE, (uint64_t)desc->num_sectors * SECTOR_SIZE);
	}
	bsmsession->bdev_count++;
	rc = spdk_bdev_write_zeroes(bsmsession->bdev_desc, bsmsession->io_channel,
				    desc->sector * SECTOR_SIZE, desc->num_sectors * SECTOR_SIZE, ssam_blk_req_complete_cb, task);

	return ssam_request_rc_process(rc, task);
}

static int
ssam_virtio_flush_process(struct spdk_ssam_blk_task *task,
			  const struct virtio_blk_outhdr *req)
{
	int rc;
	struct spdk_ssam_blk_session *bsmsession = task->bsmsession;
	uint64_t blockcnt = spdk_bdev_get_num_blocks(bsmsession->bdev);
	uint32_t blocklen = spdk_bdev_get_block_size(bsmsession->bdev);
	uint64_t flush_bytes;

	if (blocklen == 0) {
		SPDK_ERRLOG("bdev's blocklen %u error.\n", blocklen);
		ssam_task_complete(task, VIRTIO_BLK_S_IOERR);
		return -1;
	}
	if (req->sector != 0) {
		SPDK_ERRLOG("sector must be zero for flush command\n");
		ssam_task_complete(task, VIRTIO_BLK_S_IOERR);
		return -1;
	}

	if (blockcnt > (UINT64_MAX / blocklen)) {
		SPDK_ERRLOG("bdev's blockcnt %lu or blocklen %u out of range.\n",
			    blockcnt, blocklen);
		ssam_task_complete(task, VIRTIO_BLK_S_IOERR);
		return -1;
	}
	flush_bytes = blockcnt * blocklen;
	bsmsession->bdev_count++;
	rc = spdk_bdev_flush(bsmsession->bdev_desc, bsmsession->io_channel,
			     0, flush_bytes, ssam_blk_req_complete_cb, task);

	return ssam_request_rc_process(rc, task);
}

static int
ssam_virtio_get_id_process(struct spdk_ssam_blk_task *task)
{
	uint32_t used_length;
	struct spdk_ssam_blk_session *bsmsession = task->bsmsession;

	if (task->iovcnt == 0 || task->payload_size == 0) {
		SPDK_ERRLOG("check task param error, gfunc_id=%u.\n", bsmsession->smsession.gfunc_id);
		ssam_task_complete(task, VIRTIO_BLK_S_UNSUPP);
		return -1;
	}

	used_length = spdk_min((size_t)VIRTIO_BLK_ID_BYTES, task->iovs.virt.sges[0].iov_len);
	if (bsmsession->serial == NULL) {
		spdk_strcpy_pad(task->iovs.virt.sges[0].iov_base, spdk_bdev_get_product_name(bsmsession->bdev),
				used_length, ' ');
	} else {
		spdk_strcpy_pad(task->iovs.virt.sges[0].iov_base, bsmsession->serial,
				used_length, ' ');
	}
	bsmsession->blk_stat.bdev_complete_count++;
	ssam_blk_request_finish(true, task);

	return 0;
}

static int
ssam_io_process(struct spdk_ssam_blk_task *task, const struct virtio_blk_outhdr *req)
{
	int rc;
	SPDK_INFOLOG(ssam_blk_data,
		     "backend io start tid=%u gfunc_id=%u reqtype=%d rw=%u vqid=%u reqid=%u offset=%llu length=%u.\n",
		     task->bsmsession->smsession.smdev->tid, task->bsmsession->smsession.gfunc_id, req->type,
		     task->io_req->req.cmd.writable, task->io_req->req.cmd.virtio.vq_idx,
		     task->io_req->req.cmd.virtio.req_idx,
		     req->sector * SECTOR_SIZE, task->payload_size);
	task->bsmsession->blk_stat.bdev_count++;
	switch (req->type) {
	case VIRTIO_BLK_T_IN:
	case VIRTIO_BLK_T_OUT:
		rc = ssam_virtio_read_write_process(task, req);
		break;
	case VIRTIO_BLK_T_DISCARD:
		rc = ssam_virtio_discard_process(task);
		break;
	case VIRTIO_BLK_T_WRITE_ZEROES:
		rc = ssam_virtio_write_zeroes_process(task);
		break;
	case VIRTIO_BLK_T_FLUSH:
		rc = ssam_virtio_flush_process(task, req);
		break;
	case VIRTIO_BLK_T_GET_ID:
		rc = ssam_virtio_get_id_process(task);
		break;
	default:
		SPDK_ERRLOG("Not supported request type '%"PRIu32"'.\n", req->type);
		ssam_task_complete(task, VIRTIO_BLK_S_UNSUPP);
		return -1;
	}

	return rc;
}

static int
ssam_process_blk_request(struct spdk_ssam_blk_task *task)
{
	int ret;
	struct iovec *iov = NULL;
	const struct virtio_blk_outhdr *req = NULL;
	struct ssam_io_message *io_cmd = NULL;

	io_cmd = &task->io_req->req.cmd;
	/* get req header */
	if (spdk_unlikely(io_cmd->iovcnt == 0)) {
		SPDK_ERRLOG("iovcnt is 0 (req_idx = %"PRIu16").\n", get_req_idx(task));
		ssam_task_complete(task, VIRTIO_BLK_S_IOERR);
		return -1;
	}
	if (spdk_unlikely(io_cmd->iovs[0].iov_len != sizeof(*req))) {
		SPDK_ERRLOG("First descriptor size is %zu but expected %zu (req_idx = %"PRIu16").\n",
			    io_cmd->iovs[0].iov_len, sizeof(*req), get_req_idx(task));
		ssam_task_complete(task, VIRTIO_BLK_S_UNSUPP);
		return -1;
	}

	req = (struct virtio_blk_outhdr *)io_cmd->header;
	/* get req tail */
	iov = &io_cmd->iovs[io_cmd->iovcnt - 1];
	if (spdk_unlikely(iov->iov_len != 1)) {
		SPDK_ERRLOG("Last descriptor size is %zu but expected %d (req_idx = %"PRIu16").\n",
			    iov->iov_len, 1, get_req_idx(task));
		ssam_task_complete(task, VIRTIO_BLK_S_UNSUPP);
		return -1;
	}

	ret = ssam_io_process(task, req);
	if (ret < 0) {
		SPDK_ERRLOG("ssam io process failed(%d)\n", ret);
		return ret;
	}

	return 0;
}

static int
ssam_get_payload_size(struct ssam_request *io_req, uint32_t *payload_size)
{
	struct ssam_io_message *io_cmd = &io_req->req.cmd;
	uint32_t payload = 0;
	uint32_t i;

	if (spdk_unlikely(io_cmd->iovcnt == 0)) {
		SPDK_ERRLOG("iovcnt is 0\n");
		return -1;
	}
	for (i = 1; i < io_cmd->iovcnt - 1; i++) {
		if (spdk_unlikely((UINT32_MAX - io_cmd->iovs[i].iov_len) < payload)) {
			SPDK_ERRLOG("payload size overflow\n");
			return -1;
		}
		payload += io_cmd->iovs[i].iov_len;
	}

	if (spdk_unlikely(payload > PAYLOAD_SIZE_MAX)) {
		SPDK_ERRLOG("payload size larger than %u, payload_size = %u\n",
			    PAYLOAD_SIZE_MAX, payload);
		return -1;
	}

	*payload_size = payload;

	return 0;
}

static int
ssam_task_iovs_memory_get(struct spdk_ssam_blk_task *task)
{
	struct ssam_mempool *mp = task->bsmsession->smsession.mp;
	void *buffer = NULL;
	uint64_t phys_addr = 0;

	if (task->payload_size == 0) {
		/* request type of VIRTIO_BLK_T_FLUSH does not have payload */
		task->iovs.virt.sges[0].iov_base = NULL;
		return 0;
	}

	task->iovs.virt.sges[0].iov_base = NULL;
	task->iovs.phys.sges[0].iov_base = NULL;
	task->iovs.virt.sges[0].iov_len = task->payload_size;
	task->iovs.phys.sges[0].iov_len = task->payload_size;
	task->iovcnt = 1;

	buffer = ssam_mempool_alloc(mp, task->payload_size, &phys_addr);
	if (spdk_unlikely(buffer == NULL)) {
		return -ENOMEM;
	}

	/* ssam request max IO size is PAYLOAD_SIZE_MAX, only use one iov to save data */
	task->iovs.virt.sges[0].iov_base = buffer;
	task->iovs.phys.sges[0].iov_base = (void *)phys_addr;

	return 0;
}

static void
ssam_data_request_para(struct ssam_dma_request *dma_req, struct spdk_ssam_blk_task *task,
		       uint32_t type, uint8_t status)
{
	struct ssam_io_message *io_cmd = NULL;
	struct spdk_ssam_dma_cb dma_cb = {
		.status = status,
		.req_dir = type,
		.gfunc_id = task->io_req->gfunc_id,
		.vq_idx = task->vq_idx,
		.task_idx = task->task_idx
	};

	io_cmd = &task->io_req->req.cmd;
	dma_req->cb = (void *) * (uint64_t *)&dma_cb;
	dma_req->gfunc_id = task->io_req->gfunc_id;
	dma_req->flr_seq = task->io_req->flr_seq;
	dma_req->direction = type;
	dma_req->data_len = task->payload_size;
	if (type == SSAM_REQUEST_DATA_STORE) {
		dma_req->src = task->iovs.phys.sges;
		dma_req->src_num = task->iovcnt;
		dma_req->dst = &io_cmd->iovs[1];
		/* dma data iovs does not contain header and tail */
		dma_req->dst_num = io_cmd->iovcnt - IOV_HEADER_TAIL_NUM;
	} else if (type == SSAM_REQUEST_DATA_LOAD) {
		dma_req->src = &io_cmd->iovs[1];
		/* dma data iovs does not contain header and tail */
		dma_req->src_num = io_cmd->iovcnt - IOV_HEADER_TAIL_NUM;
		dma_req->dst = task->iovs.phys.sges;
		dma_req->dst_num = task->iovcnt;
	}
}

static void
ssam_request_dma_process(struct spdk_ssam_session *smsession, struct spdk_ssam_blk_task *task)
{
	struct virtio_blk_outhdr *req = NULL;
	int ret;

	req = (struct virtio_blk_outhdr *)task->io_req->req.cmd.header;
	SPDK_INFOLOG(ssam_blk_data,
		     "request dma request io tid=%u gfunc_id=%u reqtype=%d rw=%u vqid=%u reqid=%u.\n",
		     smsession->smdev->tid, smsession->gfunc_id, req->type, task->io_req->req.cmd.writable,
		     task->io_req->req.cmd.virtio.vq_idx, task->io_req->req.cmd.virtio.req_idx);

	switch (req->type) {
	case VIRTIO_BLK_T_IN:
	case VIRTIO_BLK_T_GET_ID:
	case VIRTIO_BLK_T_FLUSH:
		ret = ssam_process_blk_request(task);
		if (ret < 0) {
			SPDK_ERRLOG("====== Task: req_idx %u failed ======\n", task->req_idx);
		}
		break;

	case VIRTIO_BLK_T_OUT:
	case VIRTIO_BLK_T_DISCARD:
	case VIRTIO_BLK_T_WRITE_ZEROES:
		/* dma request: Host -> ipu */
		ssam_res_dma_process(smsession, task, SSAM_REQUEST_DATA_LOAD, 0);
		break;

	default:
		ssam_task_complete(task, VIRTIO_BLK_S_IOERR);
		SPDK_ERRLOG("Not supported request type '%"PRIu32"'.\n", req->type);
	}
}

struct ssam_blk_io_complete_arg {
	struct spdk_ssam_dev *smdev;
	struct ssam_request *io_req;
};

static void
ssam_blk_io_complete_cb(void *arg)
{
	struct ssam_blk_io_complete_arg *cb_arg = (struct ssam_blk_io_complete_arg *)arg;
	int rc = ssam_blk_io_complete(cb_arg->smdev, cb_arg->io_req, VIRTIO_BLK_S_IOERR);
	if (rc != 0) {
		struct spdk_ssam_session_io_wait_r *io_wait_r =
			calloc(1, sizeof(struct spdk_ssam_session_io_wait_r));
		if (io_wait_r == NULL) {
			SPDK_ERRLOG("calloc for io_wait_r failed\n");
			sleep(1);
			raise(SIGTERM);
		}
		io_wait_r->cb_fn = ssam_blk_io_complete_cb;
		io_wait_r->cb_arg = cb_arg;
		ssam_session_insert_io_wait_r(cb_arg->smdev, io_wait_r);
		return;
	}
	free(cb_arg);
	cb_arg = NULL;
}

static void
ssam_process_blk_task(struct spdk_ssam_session *smsession, struct ssam_request *io_req,
		      uint16_t vq_idx, uint16_t req_idx, uint32_t payload_size)
{
	int rc;
	struct spdk_ssam_blk_task *task = NULL;
	struct spdk_ssam_virtqueue *vq = &smsession->virtqueue[vq_idx];

	if (spdk_unlikely(vq->use_num >= vq->num)) {
		SPDK_ERRLOG("Session:%s vq(%hu) task_cnt(%u) limit(%u).\n", smsession->name, vq_idx, vq->use_num,
			    vq->num);
		goto blk_task_err;
	}

	uint32_t index = vq->index[vq->index_r];
	if (spdk_unlikely(index >= (uint32_t)vq->num)) {
		SPDK_ERRLOG("%s: vq(%u) desc_idx %u >= vq_nentries %u.\n",
				smsession->name, vq_idx, index, vq->num);
		goto blk_task_err;
	}
	task = &((struct spdk_ssam_blk_task *)vq->tasks)[index];
	if (spdk_unlikely(task->used)) {
		SPDK_ERRLOG("%s: vq(%u) task with idx %u is already pending.\n", smsession->name, vq_idx, index);
		goto blk_task_err;
	}

	smsession->task_cnt++;
	vq->index_r = (vq->index_r + 1) & 0xFF;
	vq->use_num++;

	ssam_blk_task_init(task);
	task->io_req = io_req;
	task->vq_idx = vq_idx;
	task->req_idx = req_idx;
	task->payload_size = payload_size;
	task->session_io_wait.cb_fn = ssam_session_io_resubmit;
	task->session_io_wait.cb_arg = task;

	rc = ssam_task_iovs_memory_get(task);
	if (rc != 0) {
		ssam_session_insert_io_wait(smsession, &task->session_io_wait);
		return;
	}

	ssam_request_dma_process(smsession, task);
	return;

blk_task_err:
	rc = ssam_blk_io_complete(smsession->smdev, io_req, VIRTIO_BLK_S_IOERR);
	if (rc != 0) {
		struct spdk_ssam_session_io_wait_r *io_wait_r =
			calloc(1, sizeof(struct spdk_ssam_session_io_wait_r));
		struct ssam_blk_io_complete_arg *cb_arg =
			calloc(1, sizeof(struct ssam_blk_io_complete_arg));
		if (io_wait_r == NULL || cb_arg == NULL) {
			SPDK_ERRLOG("calloc for io_wait_r failed\n");
			if (io_wait_r != NULL) {
				free(io_wait_r);
			}
			if (cb_arg != NULL) {
				free(cb_arg);
			}
			sleep(1);
			raise(SIGTERM);
		}
		cb_arg->smdev = smsession->smdev;
		cb_arg->io_req = io_req;
		io_wait_r->cb_fn = ssam_blk_io_complete_cb;
		io_wait_r->cb_arg = cb_arg;
		ssam_session_insert_io_wait_r(smsession->smdev, io_wait_r);
	}
	return;
}

static void
ssam_process_vq(struct spdk_ssam_session *smsession, struct ssam_request *io_req)
{
	struct ssam_io_message *io_cmd = &io_req->req.cmd;
	uint16_t vq_idx = io_cmd->virtio.vq_idx;
	uint16_t req_idx = io_cmd->virtio.req_idx;
	uint32_t payload_size = 0;
	int rc;

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

	rc = ssam_get_payload_size(io_req, &payload_size);
	if (rc != 0) {
		goto err;
	}

	ssam_process_blk_task(smsession, io_req, vq_idx, req_idx, payload_size);
	return;

err:
	rc = ssam_blk_io_complete(smsession->smdev, io_req, VIRTIO_BLK_S_IOERR);
	if (rc != 0) {
		struct spdk_ssam_session_io_wait_r *io_wait_r =
			calloc(1, sizeof(struct spdk_ssam_session_io_wait_r));
		struct ssam_blk_io_complete_arg *cb_arg =
			calloc(1, sizeof(struct ssam_blk_io_complete_arg));
		if (io_wait_r == NULL || cb_arg == NULL) {
			SPDK_ERRLOG("calloc for io_wait_r failed\n");
			sleep(1);
			raise(SIGTERM);
		}
		cb_arg->smdev = smsession->smdev;
		cb_arg->io_req = io_req;
		io_wait_r->cb_fn = ssam_blk_io_complete_cb;
		io_wait_r->cb_arg = cb_arg;
		ssam_session_insert_io_wait_r(smsession->smdev, io_wait_r);
	}
	return;
}

static void
ssam_no_bdev_put_io_channel(struct spdk_ssam_session *smsession)
{
	struct spdk_ssam_blk_session *bsmsession = ssam_to_blk_session(smsession);

	if (smsession->task_cnt == 0 && (bsmsession->io_channel != NULL)) {
		spdk_put_io_channel(bsmsession->io_channel);
		bsmsession->io_channel = NULL;
	}
}

struct ssam_no_bdev_process_vq_arg {
	struct spdk_ssam_session *smsession;
	struct ssam_request *io_req;
};

static void
ssam_no_bdev_process_vq_cb(void *arg)
{
	struct ssam_no_bdev_process_vq_arg *cb_arg = (struct ssam_no_bdev_process_vq_arg *)arg;
	int rc = ssam_blk_io_complete(cb_arg->smsession->smdev, cb_arg->io_req, VIRTIO_BLK_S_IOERR);
	if (rc != 0) {
		struct spdk_ssam_session_io_wait_r *io_wait_r =
			calloc(1, sizeof(struct spdk_ssam_session_io_wait_r));
		if (io_wait_r == NULL) {
			SPDK_ERRLOG("calloc for io_wait_r failed\n");
			sleep(1);
			raise(SIGTERM);
		}
		io_wait_r->cb_fn = ssam_no_bdev_process_vq_cb;
		io_wait_r->cb_arg = cb_arg;
		ssam_session_insert_io_wait_r(cb_arg->smsession->smdev, io_wait_r);
		return;
	}
	ssam_no_bdev_put_io_channel(cb_arg->smsession);
	free(cb_arg);
	cb_arg = NULL;
}

static void
ssam_no_bdev_process_vq(struct spdk_ssam_session *smsession, struct ssam_request *io_req)
{
	SPDK_ERRLOG("gfunc_id %u No bdev, aborting request, return EIO\n", io_req->gfunc_id);
	int rc = ssam_blk_io_complete(smsession->smdev, io_req, VIRTIO_BLK_S_IOERR);
	if (rc != 0) {
		struct spdk_ssam_session_io_wait_r *io_wait_r =
			calloc(1, sizeof(struct spdk_ssam_session_io_wait_r));
		struct ssam_no_bdev_process_vq_arg *cb_arg =
			calloc(1, sizeof(struct ssam_no_bdev_process_vq_arg));
		if (io_wait_r == NULL || cb_arg == NULL) {
			SPDK_ERRLOG("calloc for io_wait_r failed\n");
			sleep(1);
			raise(SIGTERM);
		}
		cb_arg->smsession = smsession;
		cb_arg->io_req = io_req;
		io_wait_r->cb_fn = ssam_no_bdev_process_vq_cb;
		io_wait_r->cb_arg = cb_arg;
		ssam_session_insert_io_wait_r(smsession->smdev, io_wait_r);
		return;
	}

	ssam_no_bdev_put_io_channel(smsession);
}

static void
ssam_blk_response_worker(struct spdk_ssam_session *smsession, void *arg)
{
	struct ssam_dma_rsp *dma_rsp = (struct ssam_dma_rsp *)arg;
	struct spdk_ssam_dma_cb *dma_cb = (struct spdk_ssam_dma_cb *)&dma_rsp->cb;
	struct spdk_ssam_blk_task *task = NULL;
	uint16_t vq_idx = dma_cb->vq_idx;
	uint16_t task_idx = dma_cb->task_idx;
	uint8_t req_dir = dma_cb->req_dir;

	if (vq_idx >= smsession->max_queues) {
		smsession->smdev->discard_io_num++;
		SPDK_ERRLOG("vq_idx out of range, need less than %u, actually %u\n",
			    smsession->max_queues, vq_idx);
		return;
	}

	if (spdk_unlikely(task_idx >= smsession->queue_size)) {
		smsession->smdev->discard_io_num++;
		SPDK_ERRLOG("%s: vq(%u) task_idx %u >= smsession->queue_size %u.\n",
				smsession->name, vq_idx, task_idx, smsession->queue_size);
		return;
	}
	task = &((struct spdk_ssam_blk_task *)smsession->virtqueue[vq_idx].tasks)[task_idx];
	if (dma_rsp->status != 0) {
		ssam_task_complete(task, VIRTIO_BLK_S_IOERR);
		SPDK_ERRLOG("dma data process failed!\n");
		return;
	}
	if (dma_rsp->last_flag == 0) {
		ssam_task_complete(task, VIRTIO_BLK_S_IOERR);
		SPDK_ERRLOG("last_flag should not equal 0!\n");
		return;
	}
	ssam_task_stat_tick(&task->task_stat.dma_end_tsc);
	task->bsmsession->blk_stat.dma_complete_count++;
	if (req_dir == SSAM_REQUEST_DATA_LOAD) {
		ssam_io_crc_check_log("write dma complete", ssam_blk_io_crc_print, task);
		/* Write data ready, start a request to backend */
		ssam_process_blk_request(task);
	} else if (req_dir == SSAM_REQUEST_DATA_STORE) {
		/* Data have been read by user, complete the task */
		ssam_task_complete(task, dma_cb->status);
	}
}

static int
ssam_blk_check_io_req(struct spdk_ssam_dev *smdev, struct ssam_request *io_req)
{
	struct ssam_io_message *io_cmd = NULL;
	uint16_t vq_idx;
	uint16_t req_idx;
	const struct virtio_blk_outhdr *req = NULL;

	if (io_req == NULL) {
		SPDK_ERRLOG("%s: received a NULL IO message\n", smdev->name);
		return -1;
	}

	io_cmd = &io_req->req.cmd;
	vq_idx = io_cmd->virtio.vq_idx;
	req_idx = io_cmd->virtio.req_idx;
	req = (struct virtio_blk_outhdr *)io_cmd->header;

	if (io_cmd->iovs == NULL) {
		SPDK_ERRLOG("%s: received an empty IO, vq_idx:%u, req_idx:%u\n",
			    smdev->name, vq_idx, req_idx);
		return -1;
	}

	if (io_cmd->iovcnt < IOV_HEADER_TAIL_NUM) {
		SPDK_ERRLOG("%s: iovcnt %u less than %d but expected not less than %d\n",
			    smdev->name, io_cmd->iovcnt, IOV_HEADER_TAIL_NUM, IOV_HEADER_TAIL_NUM);
		return -1;
	}

	if ((io_cmd->iovcnt == IOV_HEADER_TAIL_NUM) && (req->type != VIRTIO_BLK_T_FLUSH)) {
		SPDK_ERRLOG("%s: received an IO not contain valid data, iovcnt:%u, vq_idx:%u, "
			    "req_idx:%u, req_type:%u, req_ioprio:%u, req_sector:%llu\n",
			    smdev->name, io_cmd->iovcnt, vq_idx, req_idx, req->type, req->ioprio, req->sector);
		return -1;
	}

	if (io_cmd->iovcnt > (SPDK_SSAM_IOVS_MAX + IOV_HEADER_TAIL_NUM)) {
		SPDK_ERRLOG("%s: received too much IO, iovcnt:%u, vq_idx:%u, req_idx:%u\n",
			    smdev->name, io_cmd->iovcnt, vq_idx, req_idx);
		return -1;
	}

	return 0;
}

static void
ssam_blk_request_worker(struct spdk_ssam_session *smsession, void *arg)
{
	struct spdk_ssam_dev *smdev = smsession->smdev;
	struct spdk_ssam_blk_session *bsmsession = ssam_to_blk_session(smsession);
	struct ssam_request *io_req = (struct ssam_request *)arg;
	int ret;

	smdev->io_num++;
	bsmsession->blk_stat.start_count++;

	ret = ssam_blk_check_io_req(smdev, io_req);
	if (ret < 0) {
		smdev->discard_io_num++;
		return;
	}

	if (bsmsession->no_bdev || bsmsession->io_channel == NULL) {
		ssam_no_bdev_process_vq(smsession, io_req);
	} else {
		ssam_process_vq(smsession, io_req);
	}
}

static void
ssam_blk_no_data_request_worker(struct spdk_ssam_session *smsession)
{
	struct spdk_ssam_blk_session *bsmsession = NULL;

	bsmsession = ssam_to_blk_session(smsession);
	if (bsmsession->no_bdev) {
		ssam_no_bdev_put_io_channel(smsession);
	}
}

static void
ssam_blk_destroy_bdev_device(struct spdk_ssam_session *smsession, void *args)
{
	struct spdk_ssam_blk_session *bsmsession = ssam_to_blk_session(smsession);
	spdk_bdev_close(bsmsession->bdev_desc);

	/* free taskpool */
	ssam_free_task_pool(bsmsession);

	/* free */
	free(bsmsession);
}

static void
ssam_request_resubmit(void *arg)
{
	struct spdk_ssam_blk_task *task = (struct spdk_ssam_blk_task *)arg;
	int rc;

	rc = ssam_process_blk_request(task);
	if (rc == 0) {
		SPDK_DEBUGLOG(ssam_blk_data, "====== Task: req_idx = %"PRIu16" resubmitted ======\n",
			      get_req_idx(task));
	} else {
		SPDK_WARNLOG("====== Task: req_idx = %"PRIu16" failed ======\n", get_req_idx(task));
	}
}

static inline void
ssam_request_queue_io(struct spdk_ssam_blk_task *task)
{
	int rc;
	struct spdk_ssam_blk_session *bsmsession = task->bsmsession;

	task->bdev_io_wait.bdev = bsmsession->bdev;
	task->bdev_io_wait.cb_fn = ssam_request_resubmit;
	task->bdev_io_wait.cb_arg = task;

	rc = spdk_bdev_queue_io_wait(bsmsession->bdev, bsmsession->io_channel, &task->bdev_io_wait);
	if (rc != 0) {
		SPDK_ERRLOG("%s: failed to queue I/O, rc=%d\n", bsmsession->smsession.name, rc);
		ssam_task_complete(task, VIRTIO_BLK_S_IOERR);
	}
}

static void
ssam_session_io_resubmit(void *arg)
{
	struct spdk_ssam_blk_task *task = (struct spdk_ssam_blk_task *)arg;
	struct spdk_ssam_session *smsession = &task->bsmsession->smsession;
	int rc;

	rc = ssam_task_iovs_memory_get(task);
	if (rc != 0) {
		ssam_session_insert_io_wait(smsession, &task->session_io_wait);
		return;
	}
	ssam_request_dma_process(smsession, task);
}

static void
ssam_blk_start_post_cb(struct spdk_ssam_session *smsession, void **arg)
{
	struct spdk_ssam_blk_session *bsmsession = ssam_to_blk_session(smsession);
	int rc;

	smsession->started = true;
	rc = ssam_virtio_blk_resize(smsession->gfunc_id, bsmsession->bdev->blockcnt);
	if (rc != 0) {
		SPDK_WARNLOG("%s: virtio blk resize failed.\n", smsession->name);
	}

	rc = ssam_mount_normal(smsession, 0);
	if (rc != SSAM_MOUNT_OK) {
		SPDK_WARNLOG("%s: mount ssam volume failed\n", smsession->name);
	}

	/* Smdev poller is not created here, but is created in the initialization process. */
	SPDK_NOTICELOG("BLK controller %s created with bdev %s, queues %u\n",
		       smsession->name, spdk_bdev_get_name(bsmsession->bdev), smsession->max_queues);
}

static int
ssam_blk_start_cb(struct spdk_ssam_session *smsession, void **ctx)
{
	struct spdk_ssam_blk_session *bsmsession = ssam_to_blk_session(smsession);

	if (bsmsession->bdev == NULL) {
		SPDK_ERRLOG("%s: session not have a bdev.\n", smsession->name);
		return -ENODEV;
	}

	bsmsession->io_channel = spdk_bdev_get_io_channel(bsmsession->bdev_desc);
	if (bsmsession->io_channel == NULL) {
		ssam_free_task_pool(bsmsession);
		SPDK_ERRLOG("%s: I/O channel allocation failed\n", smsession->name);
		return -ENODEV;
	}

	ssam_session_start_done(smsession, 0);

	ssam_send_event_async_done(ctx);

	return 0;
}

static int
ssam_blk_start(struct spdk_ssam_session *smsession)
{
	struct spdk_ssam_blk_session *bsmsession = ssam_to_blk_session(smsession);
	struct spdk_ssam_send_event_flag send_event_flag = {
		.need_async = true,
		.need_rsp = true,
	};
	int rc = ssam_alloc_task_pool(bsmsession);
	if (rc != 0) {
		SPDK_ERRLOG("%s: failed to alloc task pool.\n", smsession->name);
		return rc;
	}
	return ssam_send_event_to_session(smsession, ssam_blk_start_cb, ssam_blk_start_post_cb,
					  send_event_flag, NULL);
}

static void
ssam_blk_destroy_session(struct ssam_blk_session_ctx *ctx)
{
	struct spdk_ssam_blk_session *bsmsession = ctx->bsmsession;
	struct spdk_ssam_session *smsession = &bsmsession->smsession;

	if (smsession->task_cnt > 0) {
		return;
	}

	/* If in ssam subsystem finish process, session registered flag will
	 * be set to false first, bdev will be removed in ssam_bdev_remove_cb()
	 * call back process, wating for the call back process finish first.
	 */
	if ((smsession->registered == false) && (bsmsession->bdev != NULL)) {
		return;
	}

	SPDK_NOTICELOG("%s: removing on lcore %d\n",
		       smsession->name, spdk_env_get_current_core());

	ssam_session_destroy(smsession);

	if (bsmsession->io_channel != NULL) {
		spdk_put_io_channel(bsmsession->io_channel);
		bsmsession->io_channel = NULL;
	}
	ssam_free_task_pool(bsmsession);

	if (bsmsession->serial != NULL) {
		free(bsmsession->serial);
	}
	spdk_poller_unregister(&bsmsession->stop_poller);

	ssam_session_stop_done(smsession, 0, ctx->user_ctx);
	free(ctx);

	return;
}

static int
ssam_destroy_session_poller_cb(void *arg)
{
	struct ssam_blk_session_ctx *ctx = arg;

	if (ssam_trylock() != 0) {
		return SPDK_POLLER_BUSY;
	}

	ssam_blk_destroy_session(ctx);

	ssam_unlock();

	return SPDK_POLLER_BUSY;
}

static int
ssam_blk_stop_cb(struct spdk_ssam_session *smsession, void **ctx)
{
	struct spdk_ssam_blk_session *bsmsession = ssam_to_blk_session(smsession);
	smsession->started = false;

	struct ssam_blk_session_ctx *_ctx =
		(struct ssam_blk_session_ctx *)calloc(1, sizeof(struct ssam_blk_session_ctx));

	if (_ctx == NULL) {
		SPDK_ERRLOG("%s: calloc blk session ctx error.\n", smsession->name);
		return -ENOMEM;
	}

	_ctx->bsmsession = bsmsession;
	_ctx->user_ctx = ctx;

	bsmsession->stop_poller = SPDK_POLLER_REGISTER(ssam_destroy_session_poller_cb,
				  _ctx, SESSION_STOP_POLLER_PERIOD);
	if (bsmsession->stop_poller == NULL) {
		SPDK_WARNLOG("%s: ssam_destroy_session_poller_cb start failed.\n", smsession->name);
		ssam_session_stop_done(smsession, -EBUSY, ctx);
		free(_ctx);
		return -EBUSY;
	}

	return 0;
}

static int
ssam_blk_stop(struct spdk_ssam_session *smsession)
{
	struct spdk_ssam_send_event_flag send_event_flag = {
		.need_async = true,
		.need_rsp = true,
	};
	return ssam_send_event_to_session(smsession, ssam_blk_stop_cb, ssam_blk_stop_cpl_cb,
					  send_event_flag, NULL);
}

static int
ssam_blk_remove_session(struct spdk_ssam_session *smsession)
{
	SPDK_NOTICELOG("session gfunc_id=%u removing\n", smsession->gfunc_id);
	int ret = ssam_blk_stop(smsession);
	if ((ret != 0) && (smsession->registered == true)) {
		(void)ssam_remount_normal(smsession, 0);
		return ret;
	}

	return 0;
}

const char *
ssam_get_bdev_name_by_gfunc_id(uint16_t gfunc_id)
{
	struct spdk_ssam_session *smsession;
	struct spdk_ssam_blk_session *bsmsession = NULL;

	smsession = ssam_session_find(gfunc_id);
	if (smsession == NULL) {
		return NULL;
	}
	bsmsession = ssam_to_blk_session(smsession);

	return spdk_bdev_get_name(bsmsession->bdev);
}

struct spdk_bdev *
ssam_get_session_bdev(struct spdk_ssam_session *smsession)
{
	struct spdk_ssam_blk_session *bsmsession = ssam_to_blk_session(smsession);

	return bsmsession->bdev;
}

int
ssam_blk_construct(struct spdk_ssam_session_reg_info *info, const char *dev_name,
		   bool readonly, char *serial)
{
	struct spdk_ssam_session *smsession = NULL;
	struct spdk_ssam_blk_session *bsmsession = NULL;
	struct spdk_bdev *bdev = NULL;
	uint32_t session_ctx_size = sizeof(struct spdk_ssam_blk_session) -
				    sizeof(struct spdk_ssam_session);
	uint16_t tid;
	int ret = 0;
	int rc;

	ssam_lock();

	tid = ssam_get_tid();
	if (tid == SPDK_INVALID_TID) {
		ret = -EINVAL;
		goto out;
	}

	info->tid = tid;
	info->backend = &g_ssam_blk_session_backend;
	info->session_ctx_size = session_ctx_size;
	snprintf(info->type_name, SPDK_SESSION_TYPE_MAX_LEN, "%s", SPDK_SESSION_TYPE_BLK);
	ret = ssam_session_register(info, &smsession);
	if (ret != 0) {
		goto out;
	}

	bsmsession = ssam_to_blk_session(smsession);

	ret = spdk_bdev_open_ext(dev_name, true, ssam_bdev_event_cb, smsession,
				 &bsmsession->bdev_desc);
	if (ret != 0) {
		SPDK_ERRLOG("function id %d: could not open bdev, error:%s\n", info->gfunc_id, spdk_strerror(-ret));
		goto out;
	}
	bdev = spdk_bdev_desc_get_bdev(bsmsession->bdev_desc);
	bsmsession->bdev = bdev;
	bsmsession->readonly = readonly;

	if (serial == NULL) {
		SPDK_INFOLOG(ssam_blk, "function id %d: not set volume id.\n", info->gfunc_id);
	} else {
		bsmsession->serial = calloc(SERIAL_STRING_LEN, sizeof(char));
		if (!bsmsession->serial) {
			SPDK_ERRLOG("no memory for alloc.\n");
			goto out;
		}
		(void)snprintf(bsmsession->serial, SERIAL_STRING_LEN, "%s", serial);
	}

	ret = ssam_blk_start(smsession);
	if (ret != 0) {
		SPDK_ERRLOG("%s: start failed\n", smsession->name);
		goto out;
	}

	SPDK_INFOLOG(ssam_blk, "function id %d: using bdev '%s'\n", info->gfunc_id, dev_name);
out:
	if ((ret != 0) && (smsession != NULL) && (smsession->smdev != NULL)) {
		ssam_session_unreg_response_cb(smsession);
		if (bsmsession != NULL) {
			if (bsmsession->bdev_desc != NULL) {
				spdk_bdev_close(bsmsession->bdev_desc);
				bsmsession->bdev_desc = NULL;
			}
			if (bsmsession->serial != NULL) {
				free(bsmsession->serial);
				bsmsession->serial = NULL;
			}
		}
		rc = ssam_session_unregister(smsession);
		if (rc != 0) {
			SPDK_ERRLOG("function id %d: blk construct failed and session remove failed, ret=%d\n",
				    info->gfunc_id, ret);
		}
	}
	ssam_unlock();
	return ret;
}

SPDK_LOG_REGISTER_COMPONENT(ssam_blk)
SPDK_LOG_REGISTER_COMPONENT(ssam_blk_data)
