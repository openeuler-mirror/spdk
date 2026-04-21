/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021-2025 Huawei Technologies Co.
 *   All rights reserved.
 */

#ifndef SSAM_INTERNAL_H
#define SSAM_INTERNAL_H

#include "stdint.h"

#include <rte_vhost.h>
#include "ssam_driver/dpak_ssam.h"

#include "spdk_internal/thread.h"
#include "spdk/log.h"
#include "spdk/util.h"
#include "spdk/rpc.h"
#include "spdk/bdev.h"
#include "spdk/ssam.h"
#include "ssam_config.h"
#include "ssam_qos.h"

#define SPDK_SSAM_FEATURES ((1ULL << VHOST_F_LOG_ALL) | \
    (1ULL << VHOST_USER_F_PROTOCOL_FEATURES) | \
    (1ULL << VIRTIO_F_VERSION_1) | \
    (1ULL << VIRTIO_F_NOTIFY_ON_EMPTY) | \
    (1ULL << VIRTIO_RING_F_EVENT_IDX) | \
    (1ULL << VIRTIO_RING_F_INDIRECT_DESC) | \
    (1ULL << VIRTIO_F_RING_PACKED))

#define VIRITO_DEFAULT_QUEUE_SIZE        256

#define SPDK_SSAM_VQ_MAX_SUBMISSIONS     16
#define SPDK_SSAM_MAX_VQUEUES            32
#define SPDK_SSAM_MAX_VQ_SIZE            256
#define SPDK_SSAM_VF_DEFAULTE_VQUEUES    1
#define SPDK_SSAM_BLK_MAX_VQ_SIZE        32
#define SSAM_JSON_DEFAULT_QUEUES_NUM     16

/* ssam not support config vq size so far */
#define SPDK_SSAM_DEFAULT_VQ_SIZE        SPDK_SSAM_MAX_VQ_SIZE
#define SPDK_SSAM_DEFAULT_VQUEUES        16
#define SPDK_SSAM_IOVS_MAX               32
#define SPDK_SSAM_MAX_SEG_SIZE           (32 * 1024)

#define SPDK_INVALID_GFUNC_ID            UINT16_MAX
#define SPDK_INVALID_CORE_ID             UINT16_MAX
#define SPDK_INVALID_VQUEUE_NUM          UINT16_MAX
#define SPDK_INVALID_ID                  UINT16_MAX

#define SSAM_PF_MAX_NUM                  32
#define SPDK_SSAM_SCSI_CTRLR_MAX_DEVS    255
#define SSAM_VIRTIO_SCSI_LUN_ID          0x400001
#define SPDK_SSAM_SCSI_DEFAULT_VQUEUES   128
#define SSAM_MAX_SESSION_PER_DEV         UINT16_MAX
#define SSAM_DEFAULT_MEMPOOL_EXTRA_SIZE  0
#define SSAM_MAX_CORE_NUM                16
#define SSAM_MAX_CORE_NUM_WITH_LARGE_IO  10

#define SPDK_LIMIT_LOG_MAX_INTERNEL_IN_MS   3000
#define SPDK_CONVERT_MS_TO_US               1000

#define SPDK_SSAM_VIRTIO_BLK_DEFAULT_FEATURE     0x3f11001046
#define SPDK_SSAM_VIRTIO_SCSI_DEFAULT_FEATURE    0x3f11000007

#define SSAM_NORMAL_PROCESS 0
#define SSAM_HOT_SWAP_PROCESS 1

enum spdk_ssam_iostat_mode {
	SSAM_IOSTAT_NORMAL,
	SSAM_IOSTAT_SUM,
	SSAM_IOSTAT_DUMP_VQ,
	SSAM_IOSTAT_SPARSE,
};

typedef void (*spdk_ssam_session_io_wait_cb)(void *cb_arg);

typedef void (*ssam_io_crc_check_cb)(const char *context, void *arg);

struct spdk_ssam_session_io_wait {
	spdk_ssam_session_io_wait_cb cb_fn;
	void *cb_arg;
	TAILQ_ENTRY(spdk_ssam_session_io_wait) link;
};

typedef void (*spdk_ssam_session_io_wait_r_cb)(void *cb_arg);

struct spdk_ssam_session_io_wait_r {
	spdk_ssam_session_io_wait_r_cb cb_fn;
	void *cb_arg;
	TAILQ_ENTRY(spdk_ssam_session_io_wait_r) link;
};

struct spdk_ssam_virtqueue {
	void *tasks;
	struct spdk_ssam_session *smsession;
	uint32_t *index;
	int num;
	int use_num;
	int index_l;
	int index_r;
};

struct spdk_ssam_show_iostat_args {
	/* vq_idx for blk; tgt_id for scsi */
	uint32_t id;
	enum spdk_ssam_iostat_mode mode;
};

struct spdk_ssam_session_backend {
	enum virtio_type type;
	int (*remove_session)(struct spdk_ssam_session *smsession);
	void (*remove_self)(struct spdk_ssam_session *smsession);
	void (*request_worker)(struct spdk_ssam_session *smsession, void *arg);
	void (*destroy_bdev_device)(struct spdk_ssam_session *smsession, void *args);
	void (*response_worker)(struct spdk_ssam_session *smsession, void *arg);
	void (*no_data_req_worker)(struct spdk_ssam_session *smsession);

	int (*ssam_get_config)(struct spdk_ssam_session *smsession,
			       uint8_t *config, uint32_t len, uint16_t queues);
	int (*ssam_set_config)(struct spdk_ssam_session *smsession,
			       uint8_t *config, uint32_t offset, uint32_t size, uint32_t flags);

	void (*print_stuck_io_info)(struct spdk_ssam_session *smsession);

	void (*dump_info_json)(struct spdk_ssam_session *smsession,
			       struct spdk_json_write_ctx *w);
	void (*write_config_json)(struct spdk_ssam_session *smsession,
				  struct spdk_json_write_ctx *w);
	void (*show_iostat_json)(struct spdk_ssam_session *smsession,
				 struct spdk_ssam_show_iostat_args *args,
				 struct spdk_json_write_ctx *w);
	void (*clear_iostat_json)(struct spdk_ssam_session *smsession);
	struct spdk_bdev *(*get_bdev)(struct spdk_ssam_session *smsession, uint32_t id);
};

struct spdk_ssam_session {
	/* Unique session name, format as ssam.tid.gfunc_id. */
	char *name;

	struct spdk_ssam_dev *smdev;

	/* Session poller thread, same as ssam dev poller thread */
	struct spdk_thread *thread;
	struct ssam_mempool *mp;
	const struct spdk_ssam_session_backend *backend;
	spdk_ssam_session_rsp_fn rsp_fn;
	void *rsp_ctx;
	struct spdk_ssam_virtqueue virtqueue[SPDK_SSAM_MAX_VQUEUES];

	/* Number of processing tasks, can not remove session when task_cnt > 0 */
	int task_cnt;

	/* Number of pending asynchronous operations */
	uint32_t pending_async_op_num;

	/* ssam global virtual function id */
	uint16_t gfunc_id;

	/* Depth of virtio-blk virtqueue */
	uint16_t queue_size;

	/* Number of virtio-blk virtqueue */
	uint16_t max_queues;
	bool started;
	bool initialized;

	/* spdk_ssam_session_fn process finish flag */
	bool async_done;

	bool registered;

	TAILQ_ENTRY(spdk_ssam_session) tailq;
};

struct ssam_iovs {
	struct iovec sges[SPDK_SSAM_IOVS_MAX];
};

struct ssam_iovec {
	struct ssam_iovs virt; /* virt's iov_base is virtual address */
	struct ssam_iovs phys; /* phys's iov_base is physical address */
};

struct ssam_stat {
	uint64_t poll_cur_tsc;
	uint64_t poll_tsc;
	uint64_t poll_count;
};

struct spdk_ssam_dev {
	/* ssam device name, format as ssam.tid */
	char *name;
	/* virtio type */
	enum virtio_type type;

	/* ssam device poller thread, same as session poller thread */
	struct spdk_thread *thread;
	struct spdk_poller *requestq_poller;
	struct spdk_poller *responseq_poller;
	struct spdk_poller *stop_poller;

	/* Store sessions of this dev, max number is SSAM_MAX_SESSION_PER_DEV */
	struct spdk_ssam_session **smsessions;

	TAILQ_ENTRY(spdk_ssam_dev) tailq;

	/* IO num that is on flight */
	uint64_t io_num;

	uint64_t discard_io_num;

	/* IO stuck ticks in dma process */
	uint64_t io_stuck_tsc;
	struct ssam_stat stat;

	uint64_t io_wait_cnt;
	uint64_t io_wait_r_cnt;

	/* Number of started and actively polled sessions */
	uint32_t active_session_num;

	/* Information of tid, indicate from which ssam queue to receive or send data */
	uint16_t tid;
	TAILQ_HEAD(, spdk_ssam_session_io_wait) io_wait_queue;
	TAILQ_HEAD(, spdk_ssam_session_io_wait_r) io_wait_queue_r;

	uint16_t io_request_num;
	uint16_t dma_response_num;
};

struct spdk_ssam_dma_cb {
	uint8_t status;
	uint8_t req_dir;
	uint16_t vq_idx;
	uint16_t task_idx;
	uint16_t gfunc_id;
};

struct spdk_ssam_send_event_flag {
	bool need_async;
	bool need_rsp;
};

/**
 * Remove a session from sessions array.
 *
 * \param smsessions sessions array.
 * \param smsession the session to be removed.
 */
void ssam_sessions_remove(struct spdk_ssam_session **smsessions,
			  struct spdk_ssam_session *smsession);

/**
 * Check out whether sessions is empty or not.
 *
 * \param smsessions sessions array.
 * \return true indicate sessions is empty or false not empty.
 */
bool ssam_sessions_empty(struct spdk_ssam_session **smsessions);

/**
 * Get next session in sessions array, begin with current session.
 *
 * \param smsessions sessions array.
 * \param smsession the begin session.
 * \return the next session found or null not found.
 */
struct spdk_ssam_session *ssam_sessions_next(struct spdk_ssam_session **smsessions,
		struct spdk_ssam_session *smsession);

/**
 * Insert io wait task to session.
 *
 * \param smsession the session that io wait insert to.
 * \param io_wait the io wait to be insert.
 */
void ssam_session_insert_io_wait(struct spdk_ssam_session *smsession,
				 struct spdk_ssam_session_io_wait *io_wait);

/**
 * Insert io wait compilete or dma task to smdev.
 *
 * \param smdev the smdev that io wait insert to.
 * \param io_wait_r the io wait to be insert.
 */
void ssam_session_insert_io_wait_r(struct spdk_ssam_dev *smdev,
				   struct spdk_ssam_session_io_wait_r *io_wait_r);

/**
 * Remove session from sessions and then stop session dev poller.
 *
 * \param smsession the session that to be removed.
 */
void ssam_session_destroy(struct spdk_ssam_session *smsession);

/**
 * Show a ssam device info in json format.
 *
 * \param smdev ssam device.
 * \param gfunc_id ssam global vf id.
 * \param arg user-provided parameter.
 */
void ssam_dump_info_json(struct spdk_ssam_dev *smdev, uint16_t gfunc_id,
			 struct spdk_json_write_ctx *w);

/**
 * Get a ssam device name.
 *
 * \param smdev ssam device.
 * \return ssam device name or NULL
 */
const char *ssam_dev_get_name(const struct spdk_ssam_dev *smdev);

/**
 * Get a ssam session name.
 *
 * \param smdev smsession session.
 * \return ssam session name or NULL
 */
const char *ssam_session_get_name(const struct spdk_ssam_session *smsession);

/**
 * Call a function of the provided ssam session.
 * The function will be called on this session's thread.
 *
 * \param smsession ssam session.
 * \param fn function to call on each session's thread
 * \param cpl_fn function to be called at the end of the ssam management thread.
 * Optional, can be NULL.
 * \param send_event_flag whether an asynchronous operation or response is required
 * \param ctx additional argument to the both callbacks
 * \return error code
 */
int ssam_send_event_to_session(struct spdk_ssam_session *smsession, spdk_ssam_session_fn fn,
			       spdk_ssam_session_cpl_fn cpl_fn, struct spdk_ssam_send_event_flag send_event_flag, void *ctx);

/**
 * Finish a blocking ssam_send_event_to_session() call and finally
 * start the session. This must be called on the target lcore, which
 * will now receive all session-related messages (e.g. from
 * ssam_send_event_to_session()).
 *
 * Must be called under the global ssam lock.
 *
 * \param smsession ssam session
 * \param response return code
 */
void ssam_session_start_done(struct spdk_ssam_session *smsession, int response);

/**
 * Finish a blocking ssam_send_event_to_session() call and finally
 * stop the session. This must be called on the session's lcore which
 * used to receive all session-related messages (e.g. from
 * ssam_send_event_to_session()). After this call, the session-
 * related messages will be once again processed by any arbitrary thread.
 *
 * Must be called under the global ssam lock.
 *
 * \param smsession ssam session
 * \param rsp return code
 * \param ctx user context
 */
void ssam_session_stop_done(struct spdk_ssam_session *smsession, int rsp, void **ctx);

/**
 * Set session be freed, so that not access session any more.
 *
 * \param ctx user context
 */
void ssam_set_session_be_freed(void **ctx);

/**
 * Find a ssam device in the global g_ssam_devices list by gfunc_id,
 * if find the ssam device, register a session to the existent ssam device
 * sessions list, if not find, first create a ssam device to the global
 * g_ssam_devices list, and then register a session to the new ssam device
 * sessions list.
 *
 * Must be called under the global ssam lock.
 *
 * \param info ssam session register info.
 * \param smsession ssam session created.
 * \return 0 for success or negative for failed.
 */
int ssam_session_register(struct spdk_ssam_session_reg_info *info,
			  struct spdk_ssam_session **smsession);

/**
 * unregister smsession response call back function.
 *
 * \param smsession ssam session
\ */
void ssam_session_unreg_response_cb(struct spdk_ssam_session *smsession);

void ssam_dev_unregister(struct spdk_ssam_dev **dev);

void ssam_send_event_async_done(void **ctx);

void ssam_send_dev_destroy_msg(struct spdk_ssam_session *smsession, void *args);

/**
 * Get ssam config.
 *
 * \param smsession ssam session
 * \param config a memory region to store config.
 * \param len the input config param memory region length.
 * \return 0 success or -1 failed.
 */
int ssam_get_config(struct spdk_ssam_session *smsession, uint8_t *config,
		    uint32_t len, uint16_t queues);

/**
 * Mount gfunc_id volume to the ssam normal queue.
 *
 * \param smsession ssam session
 * \param lun_id lun id of gfunc_id.
 *
 * \return 0 success or not 0 failed.
 */
int ssam_mount_normal(struct spdk_ssam_session *smsession, uint32_t lun_id);

/**
 * Unmount function.
 *
 * \param smsession ssam session
 * \param lun_id lun id of gfunc_id.
 *
 * \return 0 success or not 0 failed.
 */
int ssam_umount_normal(struct spdk_ssam_session *smsession, uint32_t lun_id);

/**
 * Mount gfunc_id volume to the ssam normal queue again.
 *
 * \param smsession ssam session
 * \param lun_id lun id of gfunc_id.
 *
 * \return 0 success or not 0 failed.
 */
int ssam_remount_normal(struct spdk_ssam_session *smsession, uint32_t lun_id);

/**
 * Register worker poller to dev.
 *
 * \param smdev the dev that to be registered worker poller.
 * \return 0 success or not 0 failed.
 */
int ssam_dev_register_worker_poller(struct spdk_ssam_dev *smdev);

/**
 * Unregister worker poller for dev.
 *
 * \param smdev the dev that to be unregistered woker poller.
 */
void ssam_dev_unregister_worker_poller(struct spdk_ssam_dev *smdev);

/**
 * Get the differential value of the current tsc.
 *
 * \param tsc the current tsc.
 * \return the differential value.
 */
uint64_t ssam_get_diff_tsc(uint64_t tsc);

/**
 * Get the bdev name of the specific gfunc_id.
 *
 * \param gfunc_id ssam global vf id.
 *
 * \return the bdev name of gfunc_id
 */
const char *ssam_get_bdev_name_by_gfunc_id(uint16_t gfunc_id);

/**
 * Remove a ssam session. Remove a session associate to the unique gfunc_id,
 * then remove the ssam device if the device not have a session any more.
 *
 * Notice that this interface cannot be reentrant, so must call ssam_lock first.
 *
 * \param smsession ssam session
 *
 * \return 0 on success, negative errno on error.
 */
int ssam_session_unregister(struct spdk_ssam_session *smsession, bool blk_force_delete);

/**
 * Get ssam iostat.
 *
 * \param smsession ssam session
 * \param stat a memory region to store iostat.
 */
void spdk_ssam_get_iostat(struct spdk_ssam_session *smsession,
			  struct spdk_bdev_io_stat *stat);

/**
 * Decrease dev io num.
 *
 * \param smdev ssam device.
 */
void ssam_dev_io_dec(struct spdk_ssam_dev *smdev);

/**
 * Get ssam session bdev.
 *
 * \param smsession ssam session
 *
 * \return the session bdev.
 */
struct spdk_bdev *ssam_get_session_bdev(struct spdk_ssam_session *smsession);

/**
 * free memory with rte.
 *
 * \param smsession ssam session
 *
 * \return 0 on success.
 */
int ssam_free_ex(void *addr);

/**
 * Get elem info from memory addr.
 *
 * \param memory addr
 *
 */
int ssam_malloc_elem_from_addr(const void *data, unsigned long long *pg_size, int *socket_id);

/**
 * check device ready.
 *
 * \param role
 *
 */
int ssam_check_device_status(uint8_t role);

/*
 * Set io crc check status.
 *
 * \param crc_check flag
 * 
 */
void spdk_ssam_set_io_crc_check(bool crc_check);

/*
 * Ssam io crc check log print.
 *
 * \param context log print context
 * \param crc_check_cb io check crc process callback
 * \param arg callback arg
 * 
 */
void ssam_io_crc_check_log(const char *context, ssam_io_crc_check_cb crc_check_cb, void *arg);

#endif /* SSAM_INTERNAL_H */
