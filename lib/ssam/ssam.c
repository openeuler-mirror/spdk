/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021-2025 Huawei Technologies Co.
 *   All rights reserved.
 */

#include <linux/virtio_blk.h>
#include <linux/virtio_scsi.h>

#include "spdk/scsi_spec.h"
#include "spdk/scsi.h"
#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/likely.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/memory.h"
#include "spdk/barrier.h"
#include "spdk/bdev_module.h"
#include "spdk/bdev.h"
#include "spdk/endian.h"

#include "ssam_internal.h"

#define SSAM_PF_NUM_MAX_VAL              31
#define SSAM_PF_PLUS_VF_NUM_MAX_VAL      4096

#define INQUIRY_OFFSET(field) \
    offsetof(struct spdk_scsi_cdb_inquiry_data, field) + \
    sizeof(((struct spdk_scsi_cdb_inquiry_data *)0x0)->field)

#define IO_STUCK_TIMEOUT            120
#define SEND_EVENT_WAIT_TIME        10
#define VMIO_TYPE_VIRTIO_SCSI_CTRL  4
#define DEVICE_READY_TIMEOUT        15
#define DEVICE_READY_WAIT_TIME      100000

bool g_ssam_subsystem_exit = false;

struct ssam_event_user_ctx {
	bool session_freed;  /* true if session has been freed */
	bool async_done;    /* true if session event done */
	void *ctx;          /* store user context pointer */
};

struct ssam_session_fn_ctx {
	/* Device session pointer obtained before enqueuing the event */
	struct spdk_ssam_session *smsession;

	spdk_ssam_session_rsp_fn *rsp_fn;

	void *rsp_ctx;

	/* User provided function to be executed on session's thread. */
	spdk_ssam_session_fn cb_fn;
	/**
	 * User provided function to be called on the init thread
	 * after iterating through all sessions.
	 */
	spdk_ssam_session_cpl_fn cpl_fn;

	/* Custom user context */
	struct ssam_event_user_ctx user_ctx;

	/* Session start event time */
	uint64_t start_tsc;

	bool need_async;

	int rsp;
};

/* ssam total infomation */
struct spdk_ssam_info {
	ssam_mempool_t *mp[SSAM_MAX_CORE_NUM];
};

static struct spdk_ssam_info g_ssam_info;

/* Thread performing all ssam management operations */
static struct spdk_thread *g_ssam_init_thread;

static TAILQ_HEAD(, spdk_ssam_dev) g_ssam_devices =
	TAILQ_HEAD_INITIALIZER(g_ssam_devices);

static pthread_mutex_t g_ssam_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Save cpu mask when ssam management thread started */
static struct spdk_cpuset g_ssam_core_mask;

/* Call back when ssam_fini complete */
static spdk_ssam_fini_cb g_ssam_fini_cpl_cb;

static int ssam_init(void);

static int
ssam_sessions_init(struct spdk_ssam_session ***smsession)
{
	*smsession = (struct spdk_ssam_session **)calloc(
			     SSAM_MAX_SESSION_PER_DEV, sizeof(struct spdk_ssam_session *));
	if (*smsession == NULL) {
		SPDK_ERRLOG("calloc sessions failed\n");
		return -ENOMEM;
	}
	return 0;
}

static int
ssam_sessions_insert(struct spdk_ssam_session **smsessions, struct spdk_ssam_session *smsession)
{
	uint16_t i = smsession->gfunc_id;

	if (smsessions[i] != NULL) {
		SPDK_ERRLOG("smsessions already have such sesseion\n");
		return -ENOSPC;
	}

	smsessions[i] = smsession;

	return 0;
}

void
ssam_sessions_remove(struct spdk_ssam_session **smsessions, struct spdk_ssam_session *smsession)
{
	uint16_t i = smsession->gfunc_id;

	if (smsessions[i] == NULL) {
		SPDK_WARNLOG("smsessions no such sesseion\n");
		return;
	}

	smsessions[i] = NULL;
	return;
}

static struct spdk_ssam_session *
ssam_sessions_first(int begin, struct spdk_ssam_session **smsessions)
{
	int i;

	for (i = begin; i < SSAM_MAX_SESSION_PER_DEV; i++) {
		if (smsessions[i] != NULL) {
			return smsessions[i];
		}
	}
	return NULL;
}

bool
ssam_sessions_empty(struct spdk_ssam_session **smsessions)
{
	struct spdk_ssam_session *session;

	session = ssam_sessions_first(0, smsessions);
	if (session == NULL) {
		return true;
	}

	return false;
}

struct spdk_ssam_session *
ssam_sessions_next(struct spdk_ssam_session **smsessions, struct spdk_ssam_session *smsession)
{
	if (smsession == NULL) {
		return ssam_sessions_first(0, smsessions);
	}
	if (smsession->gfunc_id == SSAM_MAX_SESSION_PER_DEV) {
		return NULL;
	}
	return ssam_sessions_first(smsession->gfunc_id + 1, smsessions);
}

void
ssam_session_insert_io_wait(struct spdk_ssam_session *smsession,
			    struct spdk_ssam_session_io_wait *io_wait)
{
	TAILQ_INSERT_TAIL(&smsession->smdev->io_wait_queue, io_wait, link);
	smsession->smdev->io_wait_cnt++;
}

static void
ssam_session_remove_io_wait(struct spdk_ssam_dev *smdev,
			    struct spdk_ssam_session_io_wait *session_io_wait)
{
	TAILQ_REMOVE(&smdev->io_wait_queue, session_io_wait, link);
	smdev->io_wait_cnt--;
}

void
ssam_session_insert_io_wait_r(struct spdk_ssam_dev *smdev,
			      struct spdk_ssam_session_io_wait_r *io_wait_r)
{
	TAILQ_INSERT_TAIL(&smdev->io_wait_queue_r, io_wait_r, link);
	smdev->io_wait_r_cnt++;
}

static void
ssam_session_remove_io_wait_r(struct spdk_ssam_dev *smdev,
			      struct spdk_ssam_session_io_wait_r *session_io_wait_r)
{
	TAILQ_REMOVE(&smdev->io_wait_queue_r, session_io_wait_r, link);
	smdev->io_wait_r_cnt--;
}

void
ssam_session_destroy(struct spdk_ssam_session *smsession)
{
	if (smsession == NULL || smsession->smdev == NULL) {
		return;
	}
	/* Remove smsession from the queue in advance to prevent access by the poller thread. */
	if (!ssam_sessions_empty(smsession->smdev->smsessions)) {
		ssam_sessions_remove(smsession->smdev->smsessions, smsession);
	}
	/* The smdev poller is not deleted here, but at the end of the app. */
}

uint64_t
ssam_get_diff_tsc(uint64_t tsc)
{
	return spdk_get_ticks() - tsc;
}

int
ssam_check_gfunc_id(uint16_t gfunc_id)
{
	enum ssam_device_type type;

	if (gfunc_id == SPDK_INVALID_GFUNC_ID) {
		SPDK_ERRLOG("Check gfunc_id(%u) error\n", gfunc_id);
		return -EINVAL;
	}

	type = ssam_get_virtio_type(gfunc_id);
	if (type >= SSAM_DEVICE_VIRTIO_MAX) {
		SPDK_ERRLOG("Check gfunc_id(%u) virtio type(%d) error\n", gfunc_id, type);
		return -ENODEV;
	}

	return 0;
}

/* Find a tid which has minimum device */
static uint16_t
ssam_get_min_payload_tid(uint16_t cpu_num)
{
	if (cpu_num == 0) {
		return SPDK_INVALID_TID;
	}

	struct spdk_ssam_dev *smdev = NULL;
	struct spdk_ssam_dev *tmp = NULL;
	/* All tid have been used, find a tid which has minimum device */
	uint32_t min = UINT32_MAX;
	uint16_t tid = 0;

	TAILQ_FOREACH_SAFE(smdev, &g_ssam_devices, tailq, tmp) {
		if (smdev->active_session_num < min) {
			min = smdev->active_session_num;
			tid = smdev->tid;
		}
	}

	return tid;
}

/* Get a tid number */
uint16_t
ssam_get_tid(void)
{
	uint32_t cpu_num;

	cpu_num = spdk_cpuset_count(&g_ssam_core_mask);
	if ((cpu_num == 0) || (cpu_num > UINT16_MAX)) {
		/* If cpu_num > UINT16_MAX, the result of tid will overflow */
		SPDK_ERRLOG("CPU num %u not valid.\n", cpu_num);
		return SPDK_INVALID_TID;
	}

	return ssam_get_min_payload_tid((uint16_t)cpu_num);
}

void
ssam_lock(void)
{
	pthread_mutex_lock(&g_ssam_mutex);
}

int
ssam_trylock(void)
{
	return pthread_mutex_trylock(&g_ssam_mutex);
}

void
ssam_unlock(void)
{
	pthread_mutex_unlock(&g_ssam_mutex);
}

static struct spdk_ssam_session *
ssam_session_find_in_dev(const struct spdk_ssam_dev *smdev,
			 uint16_t gfunc_id)
{
	return smdev->smsessions[gfunc_id];
}

void
ssam_dump_info_json(struct spdk_ssam_dev *smdev, uint16_t gfunc_id,
		    struct spdk_json_write_ctx *w)
{
	struct spdk_ssam_session *smsession = NULL;

	spdk_json_write_named_array_begin(w, "session");
	if (gfunc_id == UINT16_MAX) {
		smsession = ssam_sessions_next(smdev->smsessions, NULL);
		while (smsession != NULL) {
			smsession->backend->dump_info_json(smsession, w);
			smsession = ssam_sessions_next(smdev->smsessions, smsession);
		}
	} else {
		smsession = ssam_session_find_in_dev(smdev, gfunc_id);
		smsession->backend->dump_info_json(smsession, w);
	}

	spdk_json_write_array_end(w);
}

const char *
ssam_dev_get_name(const struct spdk_ssam_dev *smdev)
{
	if (!smdev) {
		return "";
	}
	return smdev->name;
}

const char *
ssam_session_get_name(const struct spdk_ssam_session *smsession)
{
	if (!smsession) {
		return "";
	}
	return smsession->name;
}

struct spdk_ssam_dev *
ssam_dev_next(const struct spdk_ssam_dev *smdev)
{
	if (smdev == NULL) {
		return TAILQ_FIRST(&g_ssam_devices);
	}

	return TAILQ_NEXT(smdev, tailq);
}

struct spdk_ssam_session *
ssam_session_find(uint16_t gfunc_id)
{
	struct spdk_ssam_dev *smdev = NULL;
	struct spdk_ssam_dev *tmp = NULL;
	struct spdk_ssam_session *smsession = NULL;

	TAILQ_FOREACH_SAFE(smdev, &g_ssam_devices, tailq, tmp) {
		smsession = ssam_session_find_in_dev(smdev, gfunc_id);
		if (smsession != NULL) {
			return smsession;
		}
	}

	return NULL;
}

uint16_t
ssam_get_gfunc_id_by_name(char *name)
{
	struct spdk_ssam_dev *smdev = NULL;
	struct spdk_ssam_dev *tmp = NULL;
	struct spdk_ssam_session *smsession = NULL;
	uint16_t gfunc_id;
	TAILQ_FOREACH_SAFE(smdev, &g_ssam_devices, tailq, tmp) {
		if (smdev != NULL && smdev->active_session_num > 0) {
			for (gfunc_id = 0; gfunc_id <= SSAM_PF_NUM_MAX_VAL; gfunc_id++) {
				smsession = ssam_session_find_in_dev(smdev, gfunc_id);
				if (smsession != NULL && strcmp(name, smsession->name) == 0) {
					return gfunc_id;
				}
			}
		}
	}

	SPDK_WARNLOG("controller(%s) is not existed\n", name);
	return SPDK_INVALID_GFUNC_ID;
}

static struct spdk_ssam_dev *
ssam_dev_find(uint16_t tid)
{
	struct spdk_ssam_dev *smdev = NULL;
	struct spdk_ssam_dev *tmp = NULL;

	TAILQ_FOREACH_SAFE(smdev, &g_ssam_devices, tailq, tmp) {
		if (smdev->tid == tid) {
			return smdev;
		}
	}

	return NULL;
}

int
ssam_mount_normal(struct spdk_ssam_session *smsession, uint32_t lun_id)
{
	uint16_t gfunc_id = smsession->gfunc_id;
	uint16_t tid = gfunc_id % ssam_get_core_num();

	return ssam_function_mount(gfunc_id, lun_id, SSAM_MOUNT_NORMAL, tid);
}

int
ssam_umount_normal(struct spdk_ssam_session *smsession, uint32_t lun_id)
{
	int rc;

	rc = ssam_function_umount(smsession->gfunc_id, lun_id);
	if (rc != 0) {
		SPDK_WARNLOG("%s: function umount failed when add scsi tgt, %d.\n", smsession->name, rc);
		return rc;
	}

	return 0;
}

int
ssam_remount_normal(struct spdk_ssam_session *smsession, uint32_t lun_id)
{
	return ssam_function_mount(smsession->gfunc_id, lun_id, SSAM_MOUNT_NORMAL, smsession->smdev->tid);
}

static int
ssam_remove_session(struct spdk_ssam_session *smsession)
{
	int rc;

	if (smsession->backend->remove_session != NULL) {
		rc = smsession->backend->remove_session(smsession);
		if (rc != 0) {
			SPDK_ERRLOG("session: %s can not be removed, task cnt %d.\n",
				    smsession->name, smsession->task_cnt);
			return rc;
		}
	}

	return 0;
}

static void
ssam_dev_thread_exit(void *unused)
{
	(void)unused;
	spdk_thread_exit(spdk_get_thread());
}

static int
ssam_tid_to_cpumask(uint16_t tid, struct spdk_cpuset *cpumask)
{
	uint32_t core;
	uint32_t lcore;
	uint32_t cnt;

	for (lcore = 0, cnt = 0; lcore < SPDK_CPUSET_SIZE - 1; lcore++) {
		if (spdk_cpuset_get_cpu(&g_ssam_core_mask, lcore)) {
			if (cnt == tid) {
				core = lcore;
				spdk_cpuset_set_cpu(cpumask, core, true);
				return 0;
			}
			cnt++;
		}
	}

	return -1;
}

void
ssam_session_start_done(struct spdk_ssam_session *smsession, int response)
{
	if (response == 0) {
		if (smsession->smdev->active_session_num == UINT32_MAX) {
			SPDK_ERRLOG("smsession %s: active session num reached upper limit %u\n",
				    smsession->name, smsession->smdev->active_session_num);
			return;
		}
		smsession->smdev->active_session_num++;
	}
}

void
ssam_set_session_be_freed(void **ctx)
{
	struct ssam_event_user_ctx *_ctx;

	if (ctx == NULL) {
		return;
	}

	_ctx = SPDK_CONTAINEROF(ctx, struct ssam_event_user_ctx, ctx);
	_ctx->session_freed = true;
}

void
ssam_send_event_async_done(void **ctx)
{
	struct ssam_event_user_ctx *_ctx;

	if (ctx == NULL) {
		return;
	}

	_ctx = SPDK_CONTAINEROF(ctx, struct ssam_event_user_ctx, ctx);
	_ctx->async_done = true;
}

void
ssam_session_stop_done(struct spdk_ssam_session *smsession, int rsp, void **ctx)
{
	if (rsp == 0) {
		if (smsession->smdev->active_session_num > 0) {
			smsession->smdev->active_session_num--;
		} else {
			SPDK_ERRLOG("smsession %s: active session num reached lower limit %u\n",
				    smsession->name, smsession->smdev->active_session_num);
		}
	}
	/* Smdev cannot be free here */

	/* Stop process need async */
	ssam_send_event_async_done(ctx);
}

void
ssam_session_unreg_response_cb(struct spdk_ssam_session *smsession)
{
	smsession->rsp_fn = NULL;
	smsession->rsp_ctx = NULL;
}

static int
ssam_dev_create_register(struct spdk_ssam_dev *smdev, uint16_t tid)
{
	char name[NAME_MAX];
	struct spdk_cpuset cpumask;
	int rc;

	smdev->tid = tid;

	rc = snprintf(name, NAME_MAX, "%s%u", "ssam.", smdev->tid);
	if (rc < 0 || rc >= NAME_MAX) {
		SPDK_ERRLOG("ssam dev name is too long, tid %u\n", tid);
		return -EINVAL;
	}

	spdk_cpuset_zero(&cpumask);
	if (ssam_tid_to_cpumask(tid, &cpumask)) {
		SPDK_ERRLOG("Can not find cpu for tid %u\n", tid);
		return -EINVAL;
	}

	smdev->name = strdup(name);
	if (smdev->name == NULL) {
		SPDK_ERRLOG("Failed to create name for ssam controller %s.\n", name);
		return -EIO;
	}

	smdev->thread = spdk_thread_create(smdev->name, &cpumask);
	if (smdev->thread == NULL) {
		SPDK_ERRLOG("Failed to create thread for ssam controller %s.\n", name);
		free(smdev->name);
		smdev->name = NULL;
		return -EIO;
	}

	rc = ssam_sessions_init(&smdev->smsessions);
	if (rc != 0) {
		return rc;
	}
	TAILQ_INSERT_TAIL(&g_ssam_devices, smdev, tailq);
	TAILQ_INIT(&smdev->io_wait_queue);
	TAILQ_INIT(&smdev->io_wait_queue_r);

	SPDK_NOTICELOG("Controller %s: new controller added, tid %u\n", smdev->name, tid);

	return 0;
}

void
ssam_dev_unregister(struct spdk_ssam_dev **dev)
{
	struct spdk_ssam_dev *smdev = *dev;
	struct spdk_thread *thread = smdev->thread;

	if (!ssam_sessions_empty(smdev->smsessions)) {
		SPDK_NOTICELOG("Controller %s still has valid session.\n",
			       smdev->name);
		return;
	}
	memset(smdev->smsessions, 0, SSAM_MAX_SESSION_PER_DEV * sizeof(struct spdk_ssam_session *));
	free(smdev->smsessions);
	smdev->smsessions = NULL;

	/* Used for hot restart. */
	if (smdev->stop_poller != NULL) {
		spdk_poller_unregister(&smdev->stop_poller);
		smdev->stop_poller = NULL;
	}

	SPDK_NOTICELOG("Controller %s: removed\n", smdev->name);

	free(smdev->name);
	smdev->name = NULL;
	ssam_lock();
	TAILQ_REMOVE(&g_ssam_devices, smdev, tailq);
	ssam_unlock();

	free(smdev);
	smdev = NULL;
	*dev = NULL;

	spdk_thread_send_msg(thread, ssam_dev_thread_exit, NULL);

	return;
}

static int
ssam_init_session_fields(struct spdk_ssam_session_reg_info *info,
			 struct spdk_ssam_dev *smdev, struct spdk_ssam_session *smsession)
{
	smsession->mp = g_ssam_info.mp[smdev->tid % ssam_get_core_num()];
	smsession->initialized = true;
	smsession->registered = true;
	smsession->thread = smdev->thread;
	smsession->backend = info->backend;
	smsession->smdev = smdev;
	smsession->gfunc_id = info->gfunc_id;
	smsession->started = false;
	smsession->rsp_fn = info->rsp_fn;
	smsession->rsp_ctx = info->rsp_ctx;
	smsession->max_queues = info->queues;
	smsession->queue_size = SPDK_SSAM_DEFAULT_VQ_SIZE;
	if (info->name == NULL) {
		smsession->name = spdk_sprintf_alloc("%s_%s_%d", smdev->name, info->type_name, info->gfunc_id);
	} else {
		smsession->name = strdup(info->name);
	}
	if (smsession->name == NULL) {
		SPDK_ERRLOG("smsession name alloc failed\n");
		return -ENOMEM;
	}

	return 0;
}

static int
ssam_add_session(struct spdk_ssam_session_reg_info *info,
		 struct spdk_ssam_dev *smdev, struct spdk_ssam_session **smsession)
{
	struct spdk_ssam_session *l_stsession = NULL;
	size_t with_ctx_len = sizeof(*l_stsession) + info->session_ctx_size;
	int rc;

	if (smdev->active_session_num == SSAM_MAX_SESSION_PER_DEV) {
		SPDK_ERRLOG("%s reached upper limit %u\n", smdev->name, SSAM_MAX_SESSION_PER_DEV);
		return -EAGAIN;
	}

	if (g_ssam_info.mp == NULL) {
		SPDK_ERRLOG("No memory pool\n");
		return -ENOMEM;
	}

	rc = posix_memalign((void **)&l_stsession, SPDK_CACHE_LINE_SIZE, with_ctx_len);
	if (rc != 0) {
		SPDK_ERRLOG("smsession alloc failed\n");
		return -ENOMEM;
	}
	memset(l_stsession, 0, with_ctx_len);

	rc = ssam_init_session_fields(info, smdev, l_stsession);
	if (rc != 0) {
		free(l_stsession);
		l_stsession = NULL;
		return rc;
	}

	rc = ssam_sessions_insert(smdev->smsessions, l_stsession);
	if (rc != 0) {
		return rc;
	}
	*smsession = l_stsession;
	if (smdev->type == VIRTIO_TYPE_UNKNOWN) {
		smdev->type = info->backend->type;
	}

	return 0;
}

static int
ssam_dev_register(struct spdk_ssam_dev **dev, uint16_t tid)
{
	struct spdk_ssam_dev *smdev = NULL;
	int rc;

	smdev = calloc(1, sizeof(*smdev));
	if (smdev == NULL) {
		SPDK_ERRLOG("Couldn't alloc device for tid %u.\n", tid);
		return -1;
	}

	rc = ssam_dev_create_register(smdev, tid);
	if (rc != 0) {
		free(smdev);
		smdev = NULL;
		return -1;
	}

	*dev = smdev;

	return 0;
}

int
ssam_session_register(struct spdk_ssam_session_reg_info *info,
		      struct spdk_ssam_session **smsession)
{
	struct spdk_ssam_dev *smdev = NULL;
	int rc;

	if (ssam_session_find(info->gfunc_id) && (strcmp(info->type_name, SPDK_SESSION_TYPE_BLK) != 0)) {
		SPDK_ERRLOG("Session with function id %d already exists.\n", info->gfunc_id);
		return -EEXIST;
	}

	smdev = ssam_dev_find(info->tid);
	if (smdev == NULL) {
		/* The smdev has been started during process initialization. Do not need to start the poller here. */
		SPDK_ERRLOG("No device with function id %d tid %u.\n", info->gfunc_id, info->tid);
		return -ENODEV;
	}

	rc = ssam_add_session(info, smdev, smsession);
	if (rc != 0) {
		return rc;
	}

	return 0;
}

int
ssam_session_unregister(struct spdk_ssam_session *smsession)
{
	int rc;

	if (smsession == NULL) {
		SPDK_ERRLOG("smsession null.\n");
		return -EINVAL;
	}

	if (smsession->pending_async_op_num != 0) {
		SPDK_ERRLOG("[OFFLOAD_SNIC] %s has internal events(%d) and cannot be deleted.\n",
			    smsession->name, smsession->pending_async_op_num);
		return -EBUSY;
	}

	rc = ssam_remove_session(smsession);
	if (rc != 0) {
		return rc;
	}

	return 0;
}

static void
ssam_io_queue_handle(struct spdk_ssam_dev *smdev)
{
	uint64_t count = 0;
	uint64_t io_wait_cnt = smdev->io_wait_cnt;
	while (count < io_wait_cnt) {
		struct spdk_ssam_session_io_wait *io_wait = TAILQ_FIRST(&smdev->io_wait_queue);
		ssam_session_remove_io_wait(smdev, io_wait);
		if (io_wait->cb_fn != NULL) {
			io_wait->cb_fn(io_wait->cb_arg);
		}
		count++;
	}
}

struct forward_ctx {
	struct spdk_ssam_session *smsession;
	struct ssam_request *io_req;
};

static void
ssam_handle_forward_req(void *_ctx)
{
	struct forward_ctx *ctx = (struct forward_ctx *)_ctx;
	ctx->smsession->backend->request_worker(ctx->smsession, ctx->io_req);
	free(ctx);
}
/* The resent request that is polled at the beginning of the hot restart is not the smsession of this smdev
 * and needs to be forwarded to the corresponding smdev.
 * If the forwarding is successful, true is returned. Otherwise, false is returned.
 */
static bool
ssam_dev_forward_req(struct ssam_request *io_req)
{
	struct spdk_ssam_dev *smdev = NULL;
	struct forward_ctx *ctx = NULL;
	int rc;
	ssam_lock();
	smdev = ssam_dev_next(NULL);
	while (smdev != NULL) {
		if (smdev->smsessions[io_req->gfunc_id] != NULL &&
		    smdev->smsessions[io_req->gfunc_id]->started == true) {
			ctx = calloc(1, sizeof(struct forward_ctx));
			if (!ctx) {
				SPDK_ERRLOG("%s: calloc failed.\n", smdev->name);
				goto out;
			}
			ctx->smsession = smdev->smsessions[io_req->gfunc_id];
			ctx->io_req = io_req;
			rc = spdk_thread_send_msg(smdev->smsessions[io_req->gfunc_id]->thread, ssam_handle_forward_req,
						  ctx);
			if (rc) {
				SPDK_ERRLOG("%s: send msg error %d.\n", smdev->name, rc);
				free(ctx);
				goto out;
			}
			ssam_unlock();
			return true;
		}
		smdev = ssam_dev_next(smdev);
	}
out:
	ssam_unlock();
	return false;
}

struct ssam_dev_io_complete_arg {
	struct spdk_ssam_dev *smdev;
	struct ssam_request *io_req;
	bool success;
};

static int
ssam_dev_io_complete(struct spdk_ssam_dev *smdev, struct ssam_request *io_req, bool success)
{
	struct ssam_io_response io_resp;
	struct ssam_virtio_res *virtio_res = (struct ssam_virtio_res *)&io_resp.data;
	struct ssam_io_message *io_cmd = &io_req->req.cmd;
	struct iovec io_vec;
	struct virtio_scsi_cmd_resp resp = {0};
	enum ssam_device_type type;
	uint8_t res_status;
	type = ssam_get_virtio_type(io_req->gfunc_id);

	if (success) {
		switch (type) {
		case SSAM_DEVICE_VIRTIO_BLK:
			res_status = VIRTIO_BLK_S_OK;
			break;
		case SSAM_DEVICE_VIRTIO_SCSI:
			res_status = VIRTIO_SCSI_S_OK;
			break;
		default:
			res_status = 0; /* unknown type, maybe 0 means ok */
		}
	} else {
		SPDK_INFOLOG(ssam, "%s: io complete return error gfunc_id %u type %d.\n",
			     smdev->name, io_req->gfunc_id, type);
		switch (type) {
		case SSAM_DEVICE_VIRTIO_BLK:
			res_status = VIRTIO_BLK_S_IOERR;
			break;
		case SSAM_DEVICE_VIRTIO_SCSI:
			res_status = VIRTIO_SCSI_S_FAILURE;
			break;
		default:
			res_status = 1; /* unknown type, maybe 1 means error */
		}
	}

	memset(&io_resp, 0, sizeof(io_resp));
	io_resp.gfunc_id = io_req->gfunc_id;
	io_resp.iocb_id = io_req->iocb_id;
	io_resp.status = io_req->status;
	io_resp.flr_seq = io_req->flr_seq;
	io_resp.req = io_req;

	virtio_res->iovs = &io_vec;
	if (type == SSAM_DEVICE_VIRTIO_SCSI && io_cmd->writable) {
		virtio_res->iovs->iov_base = io_cmd->iovs[1].iov_base;
		virtio_res->iovs->iov_len = io_cmd->iovs[1].iov_len;
	} else {
		virtio_res->iovs->iov_base = io_cmd->iovs[io_cmd->iovcnt - 1].iov_base;
		virtio_res->iovs->iov_len = io_cmd->iovs[io_cmd->iovcnt - 1].iov_len;
	}
	virtio_res->iovcnt = 1;
	if (type == SSAM_DEVICE_VIRTIO_SCSI && io_req->type != VMIO_TYPE_VIRTIO_SCSI_CTRL) {
		resp.response = res_status;
		virtio_res->rsp = &resp;
		virtio_res->rsp_len = sizeof(struct virtio_scsi_cmd_resp);
	} else {
		virtio_res->rsp = &res_status;
		virtio_res->rsp_len = sizeof(res_status);
	}

	return  ssam_io_complete(smdev->tid, &io_resp);
}

static void
ssam_dev_io_complete_cb(void *arg)
{
	struct ssam_dev_io_complete_arg *cb_arg = (struct ssam_dev_io_complete_arg *)arg;
	int rc = ssam_dev_io_complete(cb_arg->smdev, cb_arg->io_req, cb_arg->success);
	if (rc != 0) {
		struct spdk_ssam_session_io_wait_r *io_wait_r =
			calloc(1, sizeof(struct spdk_ssam_session_io_wait_r));
		if (io_wait_r == NULL) {
			SPDK_ERRLOG("calloc for io_wait_r failed\n");
			sleep(1);
			raise(SIGTERM);
		}
		io_wait_r->cb_fn = ssam_dev_io_complete_cb;
		io_wait_r->cb_arg = cb_arg;
		ssam_session_insert_io_wait_r(cb_arg->smdev, io_wait_r);
		return;
	}
	free(cb_arg);
	cb_arg = NULL;
}

static void
ssam_dev_io_finish(struct spdk_ssam_dev *smdev, struct ssam_request *io_req, bool success)
{
	int rc;
	rc = ssam_dev_io_complete(smdev, io_req, success);
	if (rc != 0) {
		struct spdk_ssam_session_io_wait_r *io_wait_r =
			calloc(1, sizeof(struct spdk_ssam_session_io_wait_r));
		struct ssam_dev_io_complete_arg *cb_arg =
			calloc(1, sizeof(struct ssam_dev_io_complete_arg));
		if (io_wait_r == NULL || cb_arg == NULL) {
			SPDK_ERRLOG("calloc for io_wait_r failed\n");
			sleep(1);
			raise(SIGTERM);
		}
		cb_arg->smdev = smdev;
		cb_arg->io_req = io_req;
		cb_arg->success = success;
		io_wait_r->cb_fn = ssam_dev_io_complete_cb;
		io_wait_r->cb_arg = cb_arg;
		ssam_session_insert_io_wait_r(smdev, io_wait_r);
	}
}

static void
ssam_dev_io_request(struct spdk_ssam_dev *smdev, struct ssam_request *io_req)
{
	struct spdk_ssam_session *smsession = NULL;

	SPDK_INFOLOG(ssam_blk_data, "handling io tid=%u gfunc_id=%u type=%d rw=%u vqid=%u reqid=%u.\n",
		     smdev->tid, io_req->gfunc_id, io_req->type, io_req->req.cmd.writable,
		     io_req->req.cmd.virtio.vq_idx, io_req->req.cmd.virtio.req_idx);

	smsession = smdev->smsessions[io_req->gfunc_id];
	if (smsession == NULL || smsession->started == false) {
		if (!ssam_dev_forward_req(io_req)) {
			SPDK_INFOLOG(ssam, "%s: not have gfunc_id %u yet in io request.\n",
				     smdev->name, io_req->gfunc_id);
			ssam_dev_io_finish(smdev, io_req, false);
		}
		return;
	}

	smsession->backend->request_worker(smsession, io_req);
	return;
}

static void
ssam_io_wait_r_queue_handle(struct spdk_ssam_dev *smdev)
{
	uint64_t count = 0;
	uint64_t io_wait_r_cnt = smdev->io_wait_r_cnt > SSAM_MAX_REQ_POLL_SIZE ? SSAM_MAX_REQ_POLL_SIZE :
				 smdev->io_wait_r_cnt;
	while (count < io_wait_r_cnt) {
		struct spdk_ssam_session_io_wait_r *io_wait_r = TAILQ_FIRST(&smdev->io_wait_queue_r);
		ssam_session_remove_io_wait_r(smdev, io_wait_r);
		if (io_wait_r->cb_fn != NULL) {
			io_wait_r->cb_fn(io_wait_r->cb_arg);
		}
		count++;
		free(io_wait_r);
		io_wait_r = NULL;
	}
}

static int
ssam_dev_request_worker(void *arg)
{
	int io_num;
	struct ssam_request *io_req[SSAM_MAX_REQ_POLL_SIZE] = {0};
	struct spdk_ssam_dev *smdev = arg;
	bool poll_busy_flag = false;

	if (spdk_unlikely(smdev->io_wait_r_cnt > 0)) {
		ssam_io_wait_r_queue_handle(smdev);
		poll_busy_flag = true;
	}

	/* The I/O waiting due to insufficient memory needs to be processed first. */
	if (spdk_unlikely(smdev->io_wait_cnt > 0)) {
		ssam_io_queue_handle(smdev);
		return SPDK_POLLER_BUSY;
	}

	io_num = ssam_request_poll(smdev->tid, SSAM_MAX_REQ_POLL_SIZE, io_req);
	if ((io_num <= 0) || (io_num > SSAM_MAX_REQ_POLL_SIZE)) {
		/*
		 * The rpc delete callback is registered when the bdev deleting. spdk_put_io_channel
		 * executed the RPC delete callback.The stdev_io_no_data_request function continuously
		 * determines whether to perform the spdk_put_io_channel operation to ensure that the
		 * deletion of the bdev does not time out.
		 */
		if (spdk_unlikely(smdev->io_wait_r_cnt > 0)) {
			ssam_io_wait_r_queue_handle(smdev);
			poll_busy_flag = true;
		}
		return poll_busy_flag == true ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
	}

	if (spdk_unlikely(smdev->io_wait_r_cnt > 0)) {
		ssam_io_wait_r_queue_handle(smdev);
	}

	for (int i = 0; i < io_num; i++) {
		ssam_dev_io_request(smdev, io_req[i]);
	}

	return SPDK_POLLER_BUSY;
}

static void
ssam_dev_io_response(struct spdk_ssam_dev *smdev, const struct ssam_dma_rsp *dma_rsp)
{
	struct spdk_ssam_session *smsession = NULL;
	const struct spdk_ssam_dma_cb *dma_cb = (const struct spdk_ssam_dma_cb *)&dma_rsp->cb;

	SPDK_INFOLOG(ssam_blk_data,
		     "handle dma resp tid=%u gfunc_id=%u rw=%u vqid=%u task_idx=%u statuc=%u.\n",
		     smdev->tid, dma_cb->gfunc_id, dma_cb->req_dir,
		     dma_cb->vq_idx, dma_cb->task_idx, dma_cb->status);

	smsession = smdev->smsessions[dma_cb->gfunc_id];
	if (smsession == NULL) {
		smdev->discard_io_num++;
		SPDK_ERRLOG("smsessions not have gfunc_id %u yet in io response.\n", dma_cb->gfunc_id);
		return;
	}

	smsession->backend->response_worker(smsession, (void *)dma_rsp);

	return;
}

static void
ssam_dev_print_stuck_io(struct spdk_ssam_dev *smdev)
{
	struct spdk_ssam_session *smsession = NULL;
	int i;

	for (i = 0; i < SSAM_MAX_SESSION_PER_DEV; i++) {
		smsession = smdev->smsessions[i];
		if (smsession == NULL) {
			continue;
		}
		if (smsession->task_cnt > 0) {
			SPDK_ERRLOG("%s: %d IO stuck for %ds\n", smsession->name,
				    smsession->task_cnt, IO_STUCK_TIMEOUT);
			if (smsession->backend->print_stuck_io_info != NULL) {
				smsession->backend->print_stuck_io_info(smsession);
			}
		}
	}
}

static void
ssam_dev_io_stuck_check(struct spdk_ssam_dev *smdev)
{
	uint64_t diff_tsc = spdk_get_ticks() - smdev->io_stuck_tsc;

	if (smdev->io_num == 0) {
		smdev->io_stuck_tsc = spdk_get_ticks();
		return;
	}

	if ((diff_tsc / IO_STUCK_TIMEOUT) > spdk_get_ticks_hz()) {
		ssam_dev_print_stuck_io(smdev);
		smdev->io_stuck_tsc = spdk_get_ticks();
	}
}

void
ssam_dev_io_dec(struct spdk_ssam_dev *smdev)
{
	smdev->io_num--;
}

static int
ssam_dev_response_worker(void *arg)
{
	int io_num;
	struct spdk_ssam_dev *smdev = arg;
	struct ssam_dma_rsp dma_rsp[SSAM_MAX_RESP_POLL_SIZE] = {0};
	bool poll_busy_flag = false;

	uint64_t ticks = spdk_get_ticks();
	if (smdev->stat.poll_cur_tsc == 0) {
		smdev->stat.poll_cur_tsc = ticks;
	} else {
		smdev->stat.poll_tsc += ticks - smdev->stat.poll_cur_tsc;
		smdev->stat.poll_count++;
		smdev->stat.poll_cur_tsc = ticks;
	}

	do {
		io_num = ssam_dma_rsp_poll(smdev->tid, SSAM_MAX_RESP_POLL_SIZE, dma_rsp);
		if (io_num <= 0 || io_num > SSAM_MAX_RESP_POLL_SIZE) {
			ssam_dev_io_stuck_check(smdev);
			return poll_busy_flag == true ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
		}

		if (smdev->io_num < ((uint64_t)(uint32_t)io_num)) {
			SPDK_ERRLOG("%s: DMA response IO num too much, should be %lu but %d\n",
				    smdev->name, smdev->io_num, io_num);
			smdev->discard_io_num += io_num;
			return poll_busy_flag == true ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
		}
		smdev->io_stuck_tsc = spdk_get_ticks();

		for (int i = 0; i < io_num; i++) {
			ssam_dev_io_response(smdev, dma_rsp + i);
		}
		poll_busy_flag = true;
	} while (io_num == SSAM_MAX_RESP_POLL_SIZE);

	return SPDK_POLLER_BUSY;
}

int
ssam_dev_register_worker_poller(struct spdk_ssam_dev *smdev)
{
	SPDK_NOTICELOG("%s: worker starting.\n", smdev->name);
	if (smdev->requestq_poller == NULL) {
		smdev->requestq_poller = SPDK_POLLER_REGISTER(ssam_dev_request_worker, smdev, 0);
		if (smdev->requestq_poller == NULL) {
			SPDK_WARNLOG("%s: stdev_request_worker start failed.\n", smdev->name);
			return -1;
		}

		SPDK_INFOLOG(ssam, "%s: started stdev_request_worker poller on lcore %d\n",
			     smdev->name, spdk_env_get_current_core());
	}

	if (smdev->responseq_poller == NULL) {
		smdev->responseq_poller = SPDK_POLLER_REGISTER(ssam_dev_response_worker, smdev, 0);
		if (smdev->responseq_poller == NULL) {
			SPDK_WARNLOG("%s: stdev_response_worker start failed.\n", smdev->name);
			return -1;
		}

		SPDK_INFOLOG(ssam, "%s: started stdev_response_worker poller on lcore %d\n",
			     smdev->name, spdk_env_get_current_core());
	}
	return 0;
}

void
ssam_dev_unregister_worker_poller(struct spdk_ssam_dev *smdev)
{
	if (!ssam_sessions_empty(smdev->smsessions)) {
		return;
	}

	if (smdev->requestq_poller != NULL) {
		spdk_poller_unregister(&smdev->requestq_poller);
		smdev->requestq_poller = NULL;
	}

	if (smdev->responseq_poller != NULL) {
		spdk_poller_unregister(&smdev->responseq_poller);
		smdev->responseq_poller = NULL;
	}
}
/* When stopping the worker, need to stop the two pollers first
 * and wait until all sessions are deleted, and then free smdev.
 */
static int
ssam_dev_stop_poller(void *arg)
{
	struct spdk_ssam_dev *smdev = arg;
	struct spdk_ssam_session *smsession = NULL;

	/* special processing is required for virtio-scsi,
	 * because In scsi scenarios, smsessions are not actively or passively removed.
	 */
	if (smdev->type == VIRTIO_TYPE_SCSI && smdev->active_session_num > 0) {
		for (int i = 0; i < SSAM_MAX_SESSION_PER_DEV; i++) {
			if (smdev->smsessions[i] != NULL) {
				smsession = smdev->smsessions[i];
				smsession->backend->remove_self(smsession);     /* remove session */
			}
		}
	}

	/* 等待session全部被移除 */
	if (smdev->active_session_num != 0) {
		return SPDK_POLLER_BUSY;
	}

	/* 删除smdev的资源 */
	ssam_dev_unregister(&smdev);

	return SPDK_POLLER_BUSY;
}

static void
ssam_dev_stop_worker_poller(void *args)
{
	struct spdk_ssam_dev *smdev = (struct spdk_ssam_dev *)args;

	if (smdev->requestq_poller != NULL) {
		spdk_poller_unregister(&smdev->requestq_poller);
		smdev->requestq_poller = NULL;
	}

	if (smdev->responseq_poller != NULL) {
		spdk_poller_unregister(&smdev->responseq_poller);
		smdev->responseq_poller = NULL;
	}

	SPDK_NOTICELOG("%s: poller stopped.\n", smdev->name);
	smdev->stop_poller = SPDK_POLLER_REGISTER(ssam_dev_stop_poller, smdev, 0);
	if (smdev->stop_poller == NULL) {
		SPDK_WARNLOG("%s: ssam_dev stop failed.\n", smdev->name);
	}
}
/* When starting the worker, need to start the two pollers first */
static void
ssam_dev_start_worker_poller(void *args)
{
	struct spdk_ssam_dev *smdev = (struct spdk_ssam_dev *)args;
	ssam_dev_register_worker_poller(smdev);
}

static void
ssam_send_event_response(struct ssam_session_fn_ctx *ev_ctx)
{
	if (ev_ctx->user_ctx.session_freed == true) {
		goto out;
	}

	if (*ev_ctx->rsp_fn != NULL) {
		(*ev_ctx->rsp_fn)(ev_ctx->rsp_ctx, ev_ctx->rsp);
		*ev_ctx->rsp_fn = NULL;
	}

out:
	/* ev_ctx be allocated by another thread */
	free(ev_ctx);
	ev_ctx = NULL;
}

static void
ssam_check_send_event_timeout(struct ssam_session_fn_ctx *ev_ctx, spdk_msg_fn fn)
{
	uint64_t diff_tsc = spdk_get_ticks() - ev_ctx->start_tsc;
	struct spdk_ssam_session *smsession = ev_ctx->smsession;

	if ((diff_tsc / SEND_EVENT_WAIT_TIME) > spdk_get_ticks_hz()) {
		/* If timeout, finish send msg, end the process */
		SPDK_ERRLOG("Send event to session %s time out.\n", smsession->name);
		ev_ctx->rsp = -ETIMEDOUT;
		ssam_send_event_response(ev_ctx);
		return;
	}

	spdk_thread_send_msg(spdk_get_thread(), fn, (void *)ev_ctx);

	return;
}

static void
ssam_send_event_finish(void *ctx)
{
	struct ssam_session_fn_ctx *ev_ctx = ctx;
	struct spdk_ssam_session *smsession = ev_ctx->smsession;

	if ((ev_ctx->rsp == 0) && (ev_ctx->need_async) && (ev_ctx->user_ctx.async_done == false)) {
		ssam_check_send_event_timeout(ev_ctx, ssam_send_event_finish);
		return;
	}

	if (ssam_trylock() != 0) {
		ssam_check_send_event_timeout(ev_ctx, ssam_send_event_finish);
		return;
	}

	if (smsession->pending_async_op_num > 0) {
		smsession->pending_async_op_num--;
	} else {
		SPDK_ERRLOG("[OFFLOAD_SNIC] smsession %s: internal error.\n", smsession->name);
	}

	/* If ev_ctx->cb_fn proccess failed, ev_ctx->cpl_fn will not excute */
	if ((ev_ctx->rsp == 0) && (ev_ctx->cpl_fn != NULL)) {
		ev_ctx->cpl_fn(smsession, &ev_ctx->user_ctx.ctx);
	}

	ssam_unlock();

	ssam_send_event_response(ev_ctx);
}

static void
ssam_send_event(void *ctx)
{
	struct ssam_session_fn_ctx *ev_ctx = ctx;
	struct spdk_ssam_session *smsession = ev_ctx->smsession;

	if (ssam_trylock() != 0) {
		ssam_check_send_event_timeout(ev_ctx, ssam_send_event);
		return;
	}

	if (smsession->initialized && (ev_ctx->cb_fn != NULL)) {
		ev_ctx->user_ctx.async_done = false;
		ev_ctx->rsp = ev_ctx->cb_fn(smsession, &ev_ctx->user_ctx.ctx);
	} else {
		ev_ctx->rsp = 0;
		ev_ctx->user_ctx.async_done = true;
	}

	ssam_unlock();
	/* The judgment logic is used to adapt to the hot-restart.
	 * Because the session has been released during the hot restart,
	 * the following ssam_send_event_finish is not required.
	 */
	if (ev_ctx->user_ctx.session_freed) {
		free(ev_ctx);
		return;
	} else {
		ev_ctx->start_tsc = spdk_get_ticks();
		spdk_thread_send_msg(g_ssam_init_thread, ssam_send_event_finish, ctx);
	}
}

static spdk_ssam_session_rsp_fn g_rsp_fn = NULL;

int
ssam_send_event_to_session(struct spdk_ssam_session *smsession, spdk_ssam_session_fn fn,
			   spdk_ssam_session_cpl_fn cpl_fn, struct spdk_ssam_send_event_flag send_event_flag, void *ctx)
{
	struct ssam_session_fn_ctx *ev_ctx;
	int rc;

	ev_ctx = calloc(1, sizeof(*ev_ctx));
	if (ev_ctx == NULL) {
		SPDK_ERRLOG("Failed to alloc ssam event.\n");
		return -ENOMEM;
	}

	ev_ctx->smsession = smsession;
	ev_ctx->cb_fn = fn;
	ev_ctx->cpl_fn = cpl_fn;
	ev_ctx->need_async = send_event_flag.need_async;
	if (send_event_flag.need_rsp == true) {
		ev_ctx->rsp_fn = &smsession->rsp_fn;
		ev_ctx->rsp_ctx = smsession->rsp_ctx;
	} else {
		ev_ctx->rsp_fn = &g_rsp_fn;
		ev_ctx->rsp_ctx = NULL;
	}

	ev_ctx->user_ctx.ctx = ctx;
	ev_ctx->user_ctx.session_freed = false;

	if (smsession->pending_async_op_num < UINT32_MAX) {
		smsession->pending_async_op_num++;
	} else {
		SPDK_ERRLOG("[OFFLOAD_SNIC] smsession %s: internel error, events stuck too much\n",
			    smsession->name);
	}

	ev_ctx->start_tsc = spdk_get_ticks();
	rc = spdk_thread_send_msg(smsession->thread, ssam_send_event, ev_ctx);
	if (rc != 0) {
		SPDK_ERRLOG("send thread msg failed\n");
		free(ev_ctx);
		return rc;
	}
	return 0;
}

void
spdk_ssam_config_json(struct spdk_json_write_ctx *w)
{
	struct spdk_ssam_dev *smdev = NULL;
	struct spdk_ssam_session *smsession = NULL;

	spdk_json_write_array_begin(w);

	ssam_lock();
	smdev = ssam_dev_next(NULL);
	while (smdev != NULL) {
		smsession = ssam_sessions_next(smdev->smsessions, NULL);
		while (smsession != NULL) {
			smsession->backend->write_config_json(smsession, w);
			smsession = ssam_sessions_next(smdev->smsessions, smsession);
		}

		smdev = ssam_dev_next(smdev);
	}

	ssam_unlock();

	spdk_json_write_array_end(w);
}

int
ssam_get_config(struct spdk_ssam_session *smsession, uint8_t *config,
		uint32_t len, uint16_t queues)
{
	const struct spdk_ssam_session_backend *backend = smsession->backend;

	if (backend->ssam_get_config == NULL) {
		return -1;
	}

	return backend->ssam_get_config(smsession, config, len, queues);
}

struct dev_destroy_ctx {
	struct spdk_ssam_session *smsession;
	void *args;
};

static void
ssam_dev_destroy(void *arg)
{
	struct dev_destroy_ctx *ctx = (struct dev_destroy_ctx *)arg;
	ctx->smsession->backend->destroy_bdev_device(ctx->smsession, ctx->args);
	free(ctx);
}

void
ssam_send_dev_destroy_msg(struct spdk_ssam_session *smsession, void *args)
{
	struct dev_destroy_ctx *ctx = calloc(1, sizeof(struct dev_destroy_ctx));
	if (ctx == NULL) {
		SPDK_ERRLOG("%s: out of memory, destroy dev failed\n", smsession->name);
		return;
	}
	ctx->smsession = smsession;
	ctx->args = args;
	spdk_thread_send_msg(g_ssam_init_thread, ssam_dev_destroy, ctx);
}

void
spdk_ssam_poller_start(void)
{
	struct spdk_ssam_dev *smdev = NULL;
	struct spdk_ssam_dev *tmp = NULL;
	ssam_lock();
	smdev = ssam_dev_next(NULL);
	while (smdev != NULL) {
		tmp = ssam_dev_next(smdev);
		/* Send the message to each smdev to start the worker on the smdev. */
		spdk_thread_send_msg(smdev->thread, ssam_dev_start_worker_poller, smdev);
		smdev = tmp;
	}
	ssam_unlock();
}

static void
ssam_fini(void *arg)
{
	struct spdk_ssam_dev *smdev = NULL;
	struct spdk_ssam_dev *tmp = NULL;
	SPDK_WARNLOG("ssam is finishing\n");
	ssam_lock();
	smdev = ssam_dev_next(NULL);
	while (smdev != NULL) {
		tmp = ssam_dev_next(smdev);
		/* Send the message to each smdev to stop the worker on the smdev. */
		spdk_thread_send_msg(smdev->thread, ssam_dev_stop_worker_poller, smdev);
		smdev = tmp;
	}
	ssam_unlock();

	spdk_cpuset_zero(&g_ssam_core_mask);

	g_ssam_fini_cpl_cb();
}

static void *
ssam_session_shutdown(void *arg)
{
	SPDK_INFOLOG(ssam, "ssam sesssion Exiting\n");
	spdk_thread_send_msg(g_ssam_init_thread, ssam_fini, NULL);

	return NULL;
}

void
spdk_ssam_subsystem_fini(spdk_ssam_fini_cb fini_cb)
{
	if (spdk_get_thread() != g_ssam_init_thread) {
		SPDK_ERRLOG("ssam finish thread not equal init thread, internel error\n");
	}

	g_ssam_fini_cpl_cb = fini_cb;

	ssam_session_shutdown(NULL);
}

void
spdk_ssam_subsystem_init(spdk_ssam_init_cb init_cb)
{
	uint32_t i;
	int ret;
	int shm_id;

	g_ssam_init_thread = spdk_get_thread();
	if (g_ssam_init_thread == NULL) {
		ret = -EBUSY;
		SPDK_ERRLOG("get thread error\n");
		goto exit;
	}

	/* init ssam core mask */
	spdk_cpuset_zero(&g_ssam_core_mask);
	SPDK_ENV_FOREACH_CORE(i) {
		spdk_cpuset_set_cpu(&g_ssam_core_mask, i, true);
	}

	ret = ssam_set_core_num(spdk_cpuset_count(&g_ssam_core_mask));
	if (ret != 0) {
		goto exit;
	}

	ret = ssam_init();
	if (ret != 0) {
		goto exit;
	}

	if (!spdk_ssam_get_shm_created()) {
		shm_id = shm_open(SSAM_SHM, O_CREAT | O_EXCL | O_RDWR, SSAM_SHM_PERMIT);
		if (shm_id < 0) {
			SPDK_ERRLOG("failed to create shared memory %s\n", SSAM_SHM);
			ret = -1;
			goto exit;
		}
		spdk_ssam_set_shm_created(true);
	}

exit:
	init_cb(ret);
	return;
}

/* Initialize all smdev modules during submodule initialization. */
static int
ssam_smdev_init(void)
{
	int rc = 0;
	struct spdk_ssam_dev *smdev;
	struct spdk_ssam_dev *tmp = NULL;
	uint16_t core_num = ssam_get_core_num();
	for (uint16_t i = 0; i < core_num; ++i) {
		rc = ssam_dev_register(&smdev, i);
		if (rc != 0) {
			goto out;
		}
	}

	rc = ssam_get_hot_upgrade_state();
	if (rc != 0) {
		SPDK_ERRLOG(": virtio upgrade state failed.\n");
		return rc;
	}

	return 0;
out:
	smdev = ssam_dev_next(NULL);
	while (smdev != NULL) {
		tmp = ssam_dev_next(smdev);
		ssam_dev_unregister(&smdev);
		smdev = tmp;
	}
	return rc;
}

static int
ssam_server_init(void)
{
	uint32_t core_num = ssam_get_core_num();
	uint32_t mempool_size = (ssam_get_mempool_size() / core_num) & (~0U - 1);
	uint32_t i;

	/* Disable dummy I/O for hot restart */

	for (i = 0; i < core_num; i++) {
		g_ssam_info.mp[i] = ssam_mempool_create(mempool_size * SSAM_MB, SSAM_DEFAULT_MEMPOOL_EXTRA_SIZE);
		if (g_ssam_info.mp[i] == NULL) {
			SPDK_ERRLOG("ssam create mempool[%d] failed, mempool_size = %uMB.\n", i, mempool_size);
			return -ENOMEM;
		}
	}

	return 0;
}

static void
ssam_server_exit(void)
{
	uint32_t core_num = ssam_get_core_num();
	uint32_t i;

	for (i = 0; i < core_num; i++) {
		if (g_ssam_info.mp[i] != NULL) {
			ssam_mempool_destroy(g_ssam_info.mp[i]);
			g_ssam_info.mp[i] = NULL;
		}
	}

	memset(&g_ssam_info, 0x0, sizeof(struct spdk_ssam_info));
}


static int
ssam_check_device_status(void)
{
	uint8_t ready = 0;
	int times = 0;
	int rc;

	do {
		rc = ssam_check_device_ready(0, 0, &ready);
		if (rc != 0) {
			SPDK_ERRLOG("device check failed.\n");
			return rc;
		}

		if (ready != 0) {
			break;
		}

		usleep(DEVICE_READY_WAIT_TIME);
		times++;
	} while (times < DEVICE_READY_TIMEOUT);

	if (ready == 0) {
		SPDK_ERRLOG("device has not been ready after 1.5s.\n");
		return -1;
	}

	return 0;
}


static int
ssam_init(void)
{
	int rc;

	rc = ssam_check_device_status();
	if (rc != 0) {
		return rc;
	}

	rc = ssam_config_init();
	if (rc != 0) {
		return rc;
	}

	rc = ssam_server_init();
	if (rc != 0) {
		ssam_config_exit();
		return rc;
	}

	rc = ssam_smdev_init();
	if (rc != 0) {
		ssam_server_exit();
		ssam_config_exit();
	}

	return rc;
}

void
spdk_ssam_exit(void)
{
	ssam_deinit_device_pcie_list();
	ssam_config_exit();
	ssam_server_exit();
}

SPDK_LOG_REGISTER_COMPONENT(ssam)
