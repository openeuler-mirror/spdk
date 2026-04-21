/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021-2025 Huawei Technologies Co.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/log.h"
#include "ssam_driver_adapter.h"
#include "dpak_ssam.h"

#define SSAM_DRV_PRIORITY_LAST 65535
#define VIRTIO_F_NOTIFICATION_DATA      (1UL << 38)
#define SSAM_DPAK_DIR           "/etc/dpak/"
#define SSAM_CFG_DIR            SSAM_DPAK_DIR SSAM_SERVER_NAME "/"
#define SSAM_RECOVER_CFG_JSON   SSAM_CFG_DIR "recover.json"
#define SSAM_PARAM_CFG_JSON     SSAM_CFG_DIR "parameter.json"
#define SSAM_CONFIG_DIR_PERMIT  0750

__attribute__((constructor(SSAM_DRV_PRIORITY_LAST))) int ssam_construct(void);

__attribute__((destructor(SSAM_DRV_PRIORITY_LAST))) void ssam_destruct(void);

int
ssam_lib_init(struct ssam_lib_args *args_in, struct ssam_hostep_info *eps_out)
{
	hvio_lib_args_s hvio_args_in;
	hvio_hostep_info_s *hostep_info = NULL;

	if (args_in == NULL || eps_out == NULL) {
		SPDK_ERRLOG("input paramter error, null pointer.\n");
		return -EINVAL;
	}

	memset(&hvio_args_in, 0, sizeof(hvio_lib_args_s));
	hvio_args_in.role = args_in->role;
	hvio_args_in.core_num = args_in->core_num;
	hvio_args_in.cb_ops.hvio_heap_malloc = (__typeof__(hvio_args_in.cb_ops.hvio_heap_malloc))
					       args_in->ssam_heap_malloc;
	hvio_args_in.cb_ops.hvio_heap_free = args_in->ssam_heap_free;
	hvio_args_in.host_dma_queue_per_chnl = args_in->dma_queue_num;
	hvio_args_in.hash_mode = args_in->hash_mode;

	hostep_info = (hvio_hostep_info_s *)(void *)eps_out;

	return ssam_drv_lib_init(&hvio_args_in, hostep_info);
}

int
ssam_lib_exit(void)
{
	return ssam_drv_lib_deinit();
}

int
ssam_setup_function(uint16_t pf_id, uint16_t num_vf, enum ssam_device_type dev_type)
{
	enum device_type type;
	switch (dev_type) {
	case SSAM_DEVICE_VIRTIO_BLK:
		type = DEVICE_VIRTIO_BLK;
		break;
	case SSAM_DEVICE_VIRTIO_SCSI:
		type = DEVICE_VIRTIO_SCSI;
		break;
	case SSAM_DEVICE_VIRTIO_FS:
		type = DEVICE_VIRTIO_FS;
		break;
	default:
		type = DEVICE_VIRTIO_MAX;
		break;
	}

	return ssam_drv_setup_function(pf_id, num_vf, type, type);
}

int
ssam_write_function_config(struct ssam_function_config *cfg)
{
	struct function_config hvio_function_cfg;

	if (cfg == NULL) {
		SPDK_ERRLOG("libssam input paramter error, null pointer.\n");
		return -EINVAL;
	}

	if ((cfg->virtio_config.device_feature & VIRTIO_F_NOTIFICATION_DATA) != 0) {
		SPDK_ERRLOG("Virtio feature is error.\n");
		return -EINVAL;
	}

	memset(&hvio_function_cfg, 0x0, sizeof(struct function_config));

	hvio_function_cfg.function_id = (uint32_t)cfg->gfunc_id;
	switch (cfg->type) {
	case SSAM_DEVICE_VIRTIO_BLK:
		hvio_function_cfg.type = DEVICE_VIRTIO_BLK;
		break;
	case SSAM_DEVICE_VIRTIO_SCSI:
		hvio_function_cfg.type = DEVICE_VIRTIO_SCSI;
		break;
	case SSAM_DEVICE_VIRTIO_FS:
		hvio_function_cfg.type = DEVICE_VIRTIO_FS;
		break;
	default:
		hvio_function_cfg.type = DEVICE_VIRTIO_MAX;
		break;
	}

	memcpy(&hvio_function_cfg.config.virtio, &cfg->virtio_config, sizeof(struct ssam_virtio_config));
	return ssam_drv_write_function_config(&hvio_function_cfg);
}

int
ssam_send_action(uint16_t gfunc_id, enum ssam_function_action action, const void *data,
		 uint16_t data_len)
{
	enum function_action func_act;

	if (data == NULL || data_len == 0) {
		SPDK_ERRLOG("libssam input paramter error.\n");
		return -EINVAL;
	}

	switch (action) {
	case SSAM_FUNCTION_ACTION_START:
		func_act = FUNCTION_ACTION_START;
		break;

	case SSAM_FUNCTION_ACTION_STOP:
		func_act = FUNCTION_ACTION_STOP;
		break;

	case SSAM_FUNCTION_ACTION_RESET:
		func_act = FUNCTION_ACTION_RESET;
		break;

	case SSAM_FUNCTION_ACTION_CONFIG_CHANGE:
		func_act = FUNCTION_ACTION_CONFIG_CHANGE;
		break;

	case SSAM_FUNCTION_ACTION_SCSI_EVENT:
		func_act = FUNCTION_ACTION_SCSI_EVENT;
		break;

	default:
		func_act = FUNCTION_ACTION_MAX;
		break;
	}

	return ssam_drv_send_action(gfunc_id, func_act, data, data_len);
}

int
ssam_function_mount(uint16_t gfunc_id, uint32_t lun_id, enum ssam_mount_type type, uint16_t tid)
{
	struct hvio_mount_para hash_paras;

	memset(&hash_paras, 0x0, sizeof(struct hvio_mount_para));

	hash_paras.algo_type = type;
	hash_paras.key[0] = tid;

	return ssam_drv_volume_mount(gfunc_id, lun_id, &hash_paras);
}

int
ssam_function_umount(uint16_t gfunc_id, uint32_t lun_id)
{
	return ssam_drv_volume_umount(gfunc_id, lun_id);
}

int
ssam_request_poll(uint16_t tid, uint16_t poll_num, struct ssam_request **io_req)
{
	if (io_req == NULL || poll_num > SSAM_MAX_REQ_POLL_SIZE) {
		SPDK_ERRLOG("ssam request poll input paramter error.\n");
		return -EINVAL;
	}

	return ssam_drv_vmio_req_poll_batch(tid, poll_num, (struct vmio_request **)io_req);
}

int
ssam_request_poll_ext(uint16_t tid, uint16_t poll_num, struct ssam_request **io_req,
		      struct ssam_request_poll_opt *poll_opt)
{
	if (io_req == NULL || poll_num > SSAM_MAX_REQ_POLL_SIZE || poll_opt == NULL) {
		SPDK_ERRLOG("ssam request poll ext input paramter error.\n");
		return -EINVAL;
	}

	hvio_vmio_req_poll_opt_s hvio_poll_opt = {
		.sge1_iov = poll_opt->sge1_iov,
		.queue_id = poll_opt->queue_id,
	};

	return ssam_drv_vmio_req_poll_batch_ext(tid, poll_num, (struct vmio_request **)io_req,
						&hvio_poll_opt);
}

int
ssam_dma_data_request(uint16_t tid, struct ssam_dma_request *dma_req)
{
	if (dma_req == NULL || dma_req->direction >= SSAM_REQUEST_DATA_MAX) {
		SPDK_ERRLOG("ssam dma request input paramter error.\n");
		return -EINVAL;
	}

	hvio_host_dma_req_s *mode_para = (hvio_host_dma_req_s *)dma_req;

	return ssam_drv_host_dma_request(tid, mode_para);
}

int
ssam_dma_rsp_poll(uint16_t tid, uint16_t poll_num, struct ssam_dma_rsp *dma_rsp)
{
	if (dma_rsp == NULL || poll_num > SSAM_MAX_RESP_POLL_SIZE) {
		SPDK_ERRLOG("resp poll input paramter error.\n");
		return -EINVAL;
	}

	return ssam_drv_host_dma_rsp_poll(tid, poll_num, (hvio_host_dma_rsp_s *)dma_rsp);
}

static enum vmio_type
ssam_io_type_to_vmio(enum ssam_io_type io_type) {
	enum vmio_type vmio_type;

	switch (io_type)
	{
	case SSAM_VIRTIO_BLK_IO:
		vmio_type = VMIO_TYPE_VIRTIO_BLK_IO;
		break;

	case SSAM_VIRTIO_SCSI_IO:
		vmio_type = VMIO_TYPE_VIRTIO_SCSI_IO;
		break;

	case SSAM_VIRTIO_SCSI_CTRL:
		vmio_type = VMIO_TYPE_VIRTIO_SCSI_CTRL;
		break;

	case SSAM_VIRTIO_SCSI_EVT:
		vmio_type = VMIO_TYPE_VIRTIO_SCSI_EVT;
		break;

	case SSAM_VIRTIO_FUNC_STATUS:
		vmio_type = VMIO_TYPE_VIRTIO_FUNC_STATUS;
		break;

	case SSAM_VIRTIO_FS_IO:
		vmio_type = VMIO_TYPE_VIRTIO_FS_IO;
		break;

	case SSAM_VIRTIO_FS_HIPRI:
		vmio_type = VMIO_TYPE_VIRTIO_FS_HIPRI;
		break;

	default:
		vmio_type = VMIO_TYPE_RSVD;
	}

	return vmio_type;
}

int
ssam_io_complete(uint16_t tid, struct ssam_io_response *resp)
{
	struct vmio_response vmio_res;
	struct virtio_response *virtio_res = NULL;

	if (resp == NULL)  {
		SPDK_ERRLOG("ssam io complete input paramter error, null pointer.\n");
		return -EINVAL;
	}

	memset(&vmio_res, 0x0, sizeof(vmio_res));
	vmio_res.glb_function_id = resp->gfunc_id;
	vmio_res.iocb_id = resp->iocb_id;
	vmio_res.type = ssam_io_type_to_vmio(resp->req->type);

	switch (resp->status) {
	case SSAM_IO_STATUS_OK:
		vmio_res.status = VMIO_STATUS_OK;
		break;
	case SSAM_IO_STATUS_EMPTY:
		vmio_res.status = VMIO_STATUS_VQ_EMPTY;
		break;
	default:
		vmio_res.status = VMIO_STATUS_ERROR;
		break;
	}

	vmio_res.req = (struct vmio_request *)(void *)resp->req;
	vmio_res.flr_seq = resp->flr_seq;

	virtio_res = (struct virtio_response *)&vmio_res.virtio;
	virtio_res->used_len = 0; /* virtio-blk insensitive of this value, set 0 */
	virtio_res->rsp_len = resp->data.rsp_len;
	virtio_res->iovcnt = resp->data.iovcnt;
	virtio_res->iovs = resp->data.iovs;
	virtio_res->rsp = resp->data.rsp;

	return ssam_drv_vmio_complete(tid, &vmio_res);
}

int
ssam_vmio_rxq_create(uint16_t *queue_id_out)
{
	if (queue_id_out == NULL) {
		return -EINVAL;
	}
	return ssam_drv_vmio_rxq_create(queue_id_out);
}

int
ssam_update_virtio_device_used(uint16_t glb_function_id, uint64_t device_used)
{
	return ssam_drv_update_virtio_device_used(glb_function_id, device_used);
}

int
ssam_virtio_blk_resize(uint16_t gfunc_id, uint64_t capacity)
{
	return ssam_drv_update_virtio_blk_capacity(gfunc_id, capacity);
}

int
ssam_get_funcid_by_dbdf(uint32_t dbdf, uint16_t *gfunc_id)
{
	if (gfunc_id == NULL) {
		SPDK_ERRLOG("libssam input paramter error, null pointer.\n");
		return -EINVAL;
	}

	return ssam_drv_get_glb_function_id_by_dbdf(dbdf, gfunc_id);
}

int
ssam_check_device_ready(uint8_t role, uint32_t proc_type, uint8_t *ready)
{
	if (ready == NULL) {
		SPDK_ERRLOG("libssam input paramter error, null pointer.\n");
		return -EINVAL;
	}

	return ssam_drv_check_device_ready(role, proc_type, ready);
}

int
ssam_get_hot_upgrade_state(void)
{
	return ssam_drv_get_hot_upgrade_state();
}

void
ssam_hotplug_cfg(void)
{
	ssam_drv_hotplug_cfg();
}

int
ssam_hotplug_add(uint16_t port_id)
{
	return ssam_drv_hotplug_add(port_id);
}

int
ssam_hotplug_del(uint16_t port_id)
{
	return ssam_drv_hotplug_del(port_id);
}

int
ssam_hotplug_del_async(uint16_t port_id)
{
	return ssam_drv_hotplug_del_async(port_id);
}

bool
ssam_hotplug_enable_check(void)
{
	return ssam_drv_hotplug_enable_check();
}

int
ssam_hotplug_del_async_check(uint16_t port_id)
{
	return ssam_drv_hotplug_del_async_check(port_id);
}

int
ssam_virtio_blk_release_resource(uint16_t glb_function_id)
{
	return ssam_drv_virtio_blk_release_resource(glb_function_id);
}

int
ssam_virtio_blk_alloc_resource(uint16_t glb_function_id, uint16_t queue_num)
{
	return ssam_drv_virtio_blk_alloc_resource(glb_function_id, queue_num);
}

int
ssam_virtio_vq_bind_core(uint16_t glb_function_id, uint16_t queue_num)
{
	return ssam_drv_virtio_vq_bind_core(glb_function_id, queue_num);
}

int
ssam_virtio_vq_unbind_core(uint16_t glb_function_id)
{
	return ssam_drv_virtio_vq_unbind_core(glb_function_id);
}

int
ssam_qos_set_limit(uint32_t level, uint32_t id, struct ssam_qos_cfg *qos_cfg)
{
	if (qos_cfg == NULL) {
		SPDK_ERRLOG("ssam set qos info, qos config is NULL.\n");
		return -EINVAL;
	}

	return ssam_drv_qos_set_limit(level, id, (struct hivo_qos_cfg *)qos_cfg);
}

int
ssam_qos_get_limit(uint32_t level, uint32_t id, struct ssam_qos_cfg *qos_cfg)
{
	if (qos_cfg == NULL) {
		SPDK_ERRLOG("ssam get qos info, qos config is NULL.\n");
		return -EINVAL;
	}

	return ssam_drv_qos_get_limit(level, id, (struct hivo_qos_cfg *)qos_cfg);
}

int
ssam_qos_group_func_mmap(uint32_t group_id, uint32_t func_id)
{
	return ssam_drv_qos_group_func_mmap(group_id, func_id);
}

int
ssam_qos_group_func_unmmap(uint32_t group_id, uint32_t func_id)
{
	return ssam_drv_qos_group_func_unmmap(group_id, func_id);
}

int
ssam_qos_get_func_of_group(uint32_t group_id, struct ssam_qos_func_list *result)
{
	if (result == NULL) {
		SPDK_ERRLOG("ssam get group qos mapped function info , result is NULL.\n");
		return -EINVAL;
	}

	return ssam_drv_qos_get_func_of_group(group_id, (struct storage_qos_func_list *)result);
}

int
ssam_qos_clear(void)
{
	return ssam_drv_qos_clear();
}

int
ssam_qos_get_group_id(uint32_t function_id, uint32_t *group_id)
{
	return ssam_drv_qos_get_group_id(function_id, group_id);
}

static int
ssam_try_mkdir(const char *dir, mode_t mode)
{
	int rc;

	rc = mkdir(dir, mode);
	if (rc < 0 && errno != EEXIST) {
		SPDK_ERRLOG("ssam try mkdir error, dir: '%s': %s\n", dir, strerror(errno));
		return -errno;
	}
	return 0;
}

int
spdk_ssam_rc_preinit(void)
{
	int rc;

	rc = ssam_try_mkdir(SSAM_DPAK_DIR, SSAM_CONFIG_DIR_PERMIT);
	if (rc != 0) {
		return rc;
	}

	rc = ssam_try_mkdir(SSAM_CFG_DIR, SSAM_CONFIG_DIR_PERMIT);
	if (rc != 0) {
		return rc;
	}

	if (access(SSAM_RECOVER_CFG_JSON, F_OK) != 0) {
		return 0;
	}

	return 1;
}

char *
ssam_rc_get_recover_json_file_path(void)
{
	return (char *)SSAM_RECOVER_CFG_JSON;
}

char *
ssam_rc_get_param_json_file_path(void)
{
	return (char *)SSAM_PARAM_CFG_JSON;
}

__attribute__((constructor(SSAM_DRV_PRIORITY_LAST))) int
ssam_construct(void)
{
	int ret = ssam_drv_ops_init();
	if (ret != 0) {
		SPDK_ERRLOG("ssam drv ops init failed");
		return -1;
	}

	SPDK_NOTICELOG("ssam construct finish");
	return 0;
}

__attribute__((destructor(SSAM_DRV_PRIORITY_LAST))) void
ssam_destruct(void)
{
	ssam_drv_ops_uninit();
}
