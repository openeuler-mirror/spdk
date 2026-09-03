/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Huawei Technologies Co., Ltd. 2021-2021. All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef SSAM_H
#define SSAM_H

#include <sys/capability.h>

#include "spdk/stdinc.h"
#include "spdk/cpuset.h"
#include "spdk/json.h"
#include "spdk/thread.h"
#include "spdk/event.h"

#include "../../lib/ssam/ssam_driver/dpak_ssam.h"

#ifdef DEBUG
#define ASSERT(f) assert(f)
#else
#define ASSERT(f) ((void)0)
#endif

#define SPDK_INVALID_TID            UINT16_MAX
#define SPDK_SESSION_TYPE_MAX_LEN   64

#define SPDK_SESSION_TYPE_BLK   "blk"
#define SPDK_SESSION_TYPE_SCSI  "scsi"
#define SPDK_SESSION_TYPE_FS    "fs"

#define SSAM_SHM "ssam_shm"
#define SSAM_SHM_PERMIT 0640
#define SSAM_STORAGE_READY_FILE "/proc/sdi_storage/storage_ready"

enum virtio_type {
	VIRTIO_TYPE_UNKNOWN = 0,
	VIRTIO_TYPE_BLK     = (1U << 0),
	VIRTIO_TYPE_SCSI    = (1U << 1),
	VIRTIO_TYPE_FS      = (1U << 2),
};

/**
 * ssam subsystem init callback
 *
 * \param rc The preceding processing result,
 *  0 on success, negative errno on error.
 */
typedef void (*ssam_init_cb)(int rc);

/**
 * ssam subsystem fini callback
 */
typedef void (*ssam_fini_cb)(void);

/**
 * ssam dump config json
 */
void spdk_ssam_config_json(struct spdk_json_write_ctx *w);

/**
 * Check if ssam support the global vf id.
 *
 * \param gfunc_id ssam global vf id.
 *
 * \return -EINVAL indicate gfunc_id invalid, -ENODEV indicate no such vf or
 * 0 indicate gfunc_id valid.
 */
int ssam_check_gfunc_id(uint16_t gfunc_id);

/**
 * Find a ssam session by global vf id.
 *
 * \param gfunc_id ssam global vf id.
 *
 * \return ssam session or NULL indicate not find.
 */
struct spdk_ssam_session *ssam_session_find(uint16_t gfunc_id);

/**
 * Get gfunc id by controller name.
 *
 * \param name controller name.
 *
 * \return gfunc id or SPDK_INVALID_GFUNC_ID gfunc id not find.
 */
uint16_t ssam_get_gfunc_id_by_name(char *name);

/**
 * Get the next ssam device. If there's no more devices to iterate
 * through, NULL will be returned.
 *
 * \param smdev ssam device. If NULL, this function will return the
 * very first device.
 *
 * \return smdev ssam device or NULL indicate no more devices
 */
struct spdk_ssam_dev *ssam_dev_next(const struct spdk_ssam_dev *smdev);

/**
 * Lock the global ssam mutex synchronizing all the ssam device accesses.
 */
void ssam_lock(void);

/**
 * Lock the global ssam mutex synchronizing all the ssam device accesses.
 *
 * \return 0 if the mutex could be locked immediately, negative errno otherwise.
 */
int ssam_trylock(void);

/**
 * Unlock the global ssam mutex.
 */
void ssam_unlock(void);

/**
 * \param smsession ssam session.
 * \param arg user-provided parameter.
 *
 * \return 0 on success, negative if failed
 */
typedef int (*spdk_ssam_session_fn)(struct spdk_ssam_session *smsession, void **arg);

/**
 * \param smsession ssam session.
 * \param arg user-provided parameter.
 */
typedef void (*spdk_ssam_session_cpl_fn)(struct spdk_ssam_session *smsession, void **arg);

/**
 * \param arg user-provided parameter.
 * \param rsp spdk_ssam_session_fn call back response value, 0 success, negative if failed.
 */
typedef void (*spdk_ssam_session_rsp_fn)(void *arg, int rsp);

typedef void (*ssam_fs_add_fsdev_cpl_cb)(void *cb_arg, int status);

struct spdk_ssam_session_reg_info {
	char type_name[SPDK_SESSION_TYPE_MAX_LEN];
	spdk_ssam_session_rsp_fn rsp_fn;
	void *rsp_ctx;
	uint16_t gfunc_id;
	uint16_t tid;
	uint16_t queues;
	const struct spdk_ssam_session_backend *backend;
	uint32_t session_ctx_size;
	char *name;
	char *dbdf;
};

struct ssam_fs_construct_info {
	uint16_t gfunc_id;
	uint16_t max_threads;
	char *dbdf;
	char *name;
	char *fsdev_name;
};

/**
 * Construct a ssam blk device. This will create a ssam
 * blk device and then create a session. Creating the smdev will
 * start an I/O poller and hog a CPU. If already exist a ssam
 * blk device, then it will only create a session to this device.
 * All sessions in the same device share one I/O poller and one CPU.
 * ssam blk device is tightly associated with given SPDK bdev.
 * Given bdev can not be changed, unless it has been hotremoved. This
 * would result in all I/O failing with virtio VIRTIO_BLK_S_IOERR
 * error code.
 *
 * This function is thread-safe.
 *
 * \param info session register information.
 * \param dev_name bdev name to associate with this vhost device
 * \param readonly if set, all writes to the device will fail with
 * VIRTIO_BLK_S_IOERR error code.
 * \param serial means volume id.
 *
 * \return 0 on success, negative errno on error.
 */
int ssam_blk_construct(struct spdk_ssam_session_reg_info *info,
		       const char *dev_name, bool readonly, char *serial);

/**
 * ssam user config init.
 */
void spdk_ssam_user_config_init(void);

/**
 * ssam get tid which has minimum device.
 */
uint16_t ssam_get_tid(void);

uint32_t ssam_get_tids(uint16_t max_threads);

void spdk_ssam_exit(void);

void spdk_ssam_subsystem_fini(ssam_fini_cb fini_cb);

void spdk_ssam_subsystem_init(ssam_init_cb init_cb);

int ssam_scsi_construct(struct spdk_ssam_session_reg_info *info);

int ssam_scsi_dev_add_tgt(struct spdk_ssam_session *smsession, int target_num,
			  const char *bdev_name);

int ssam_scsi_dev_remove_tgt(struct spdk_ssam_session *smsession,
			     unsigned scsi_tgt_num, spdk_ssam_session_rsp_fn cb_fn, void *cb_arg);

void spdk_ssam_set_shm_created(bool shm_created);

bool spdk_ssam_get_shm_created(void);

void spdk_ssam_poller_start(void);

void ssam_deinit_device_pcie_list(void);

int ssam_init_device_pcie_list(void);

void ssam_dump_device_pcie_list(struct spdk_json_write_ctx *w);

uint32_t ssam_get_device_pcie_list_size(void);

int ssam_fs_construct(struct ssam_fs_construct_info *info, void *request, 
	spdk_ssam_session_rsp_fn rpc_ssam_send_response_cb);

int ssam_fs_destory(char *name, bool force, void *request,
	spdk_ssam_session_rsp_fn rpc_ssam_send_response_cb);

#endif /* SSAM_H */
