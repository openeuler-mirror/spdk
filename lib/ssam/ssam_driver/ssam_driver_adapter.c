/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021-2025 Huawei Technologies Co.
 *   All rights reserved.
 */

#include <dlfcn.h>
#include "spdk/stdinc.h"
#include "spdk/log.h"
#include "ssam_driver_adapter.h"

#define SSAM_DRV_SHARD_LIBRARY "/usr/lib64/libhivio.so"
#define SSAM_DRV_FUNC_NO_PTR (-1)
#define SSAM_DRV_ADD_FUNC(class, name) {#name, (void**)&(class).name}
#define SSAM_FUNC_PTR_OR_ERR_RET(func, retval) do { \
    if ((func) == NULL) \
        return retval; \
} while (0)

struct ssam_drv_ops_map {
	char *name;
	void **func;
};

static void *g_ssam_drv_handler = NULL;
static struct ssam_drv_ops g_ssam_drv_ops = { 0 };
typedef void (*lib_dlsym_uninit_cb_t)(void);
static lib_dlsym_uninit_cb_t g_lib_dlsym_uninit_cb = NULL;

static struct ssam_drv_ops_map g_ssam_drv_ops_map[] = {
	SSAM_DRV_ADD_FUNC(g_ssam_drv_ops, hvio_host_dma_request),
	SSAM_DRV_ADD_FUNC(g_ssam_drv_ops, hvio_vmio_req_poll_batch),
	SSAM_DRV_ADD_FUNC(g_ssam_drv_ops, hvio_vmio_req_poll_batch_ext),
	SSAM_DRV_ADD_FUNC(g_ssam_drv_ops, hvio_lib_deinit),
	SSAM_DRV_ADD_FUNC(g_ssam_drv_ops, hvio_volume_umount),
	SSAM_DRV_ADD_FUNC(g_ssam_drv_ops, hvio_lib_init),
	SSAM_DRV_ADD_FUNC(g_ssam_drv_ops, hvio_volume_mount),
	SSAM_DRV_ADD_FUNC(g_ssam_drv_ops, hvio_host_dma_rsp_poll),
	SSAM_DRV_ADD_FUNC(g_ssam_drv_ops, hvio_get_glb_function_id_by_dbdf),
	SSAM_DRV_ADD_FUNC(g_ssam_drv_ops, hvio_send_action),
	SSAM_DRV_ADD_FUNC(g_ssam_drv_ops, hvio_update_virtio_blk_capacity),
	SSAM_DRV_ADD_FUNC(g_ssam_drv_ops, hvio_setup_function),
	SSAM_DRV_ADD_FUNC(g_ssam_drv_ops, hvio_check_device_ready),
	SSAM_DRV_ADD_FUNC(g_ssam_drv_ops, hvio_write_function_config),
	SSAM_DRV_ADD_FUNC(g_ssam_drv_ops, hvio_get_hot_upgrade_state),
	SSAM_DRV_ADD_FUNC(g_ssam_drv_ops, hvio_vmio_complete),
	SSAM_DRV_ADD_FUNC(g_ssam_drv_ops, hvio_vmio_rxq_create),
	SSAM_DRV_ADD_FUNC(g_ssam_drv_ops, hvio_update_virtio_device_used),
	SSAM_DRV_ADD_FUNC(g_ssam_drv_ops, hvio_virtio_blk_alloc_resource),
	SSAM_DRV_ADD_FUNC(g_ssam_drv_ops, hvio_virtio_blk_release_resource),
	SSAM_DRV_ADD_FUNC(g_ssam_drv_ops, hvio_virtio_vq_bind_core),
	SSAM_DRV_ADD_FUNC(g_ssam_drv_ops, hvio_virtio_vq_unbind_core),
};

void ssam_lib_dlsym_uninit_cb_register(lib_dlsym_uninit_cb_t cb);


struct ssam_drv_ops *
ssam_get_drv_ops(void)
{
	return &g_ssam_drv_ops;
}

static void
ssam_drv_ops_cb_uninit(void)
{
	if (g_ssam_drv_handler != NULL) {
		memset(&g_ssam_drv_ops, 0, sizeof(struct ssam_drv_ops));
		dlclose(g_ssam_drv_handler);
		g_ssam_drv_handler = NULL;
	}
}

static int
ssam_drv_ops_init_sub(void *handler, struct ssam_drv_ops_map driver_map[], int size)
{
	for (int index = 0; index < size; index++) {
		if (*driver_map[index].func != NULL) {
			continue;
		}

		*driver_map[index].func = dlsym(handler, driver_map[index].name);
		if (*driver_map[index].func == NULL) {
			SPDK_ERRLOG("%s load func %s fail: %s", SSAM_DRV_SHARD_LIBRARY, driver_map[index].name, dlerror());
			return -1;
		}
	}
	return 0;
}

void
ssam_lib_dlsym_uninit_cb_register(lib_dlsym_uninit_cb_t cb)
{
	g_lib_dlsym_uninit_cb = cb;
}

int
ssam_drv_ops_init(void)
{
	int ret = 0;
	void *handler;

	if (g_ssam_drv_handler != NULL) {
		return 0;
	}

	handler = dlopen(SSAM_DRV_SHARD_LIBRARY, RTLD_NOW);
	if (handler == NULL) {
		SPDK_ERRLOG("%s load err %s\n", SSAM_DRV_SHARD_LIBRARY, dlerror());
		return -1;
	}

	ret = ssam_drv_ops_init_sub(handler, g_ssam_drv_ops_map,
				    sizeof(g_ssam_drv_ops_map) / sizeof(g_ssam_drv_ops_map[0]));
	if (ret != 0) {
		SPDK_ERRLOG("hwoff drv ops init: common api load failed");
		dlclose(handler);
		return -1;
	}

	g_ssam_drv_handler = handler;
	ssam_lib_dlsym_uninit_cb_register(ssam_drv_ops_cb_uninit);

	return 0;
}

void
ssam_drv_ops_uninit(void)
{
	if (g_lib_dlsym_uninit_cb != NULL) {
		g_lib_dlsym_uninit_cb();
		g_lib_dlsym_uninit_cb = NULL;
	}
}

int
ssam_drv_host_dma_request(uint16_t chnl_id, hvio_host_dma_req_s *req)
{
	int ret;
	struct ssam_drv_ops *ops = NULL;

	ops = ssam_get_drv_ops();
	SSAM_FUNC_PTR_OR_ERR_RET(ops->hvio_host_dma_request, SSAM_DRV_FUNC_NO_PTR);
	ret = ops->hvio_host_dma_request(chnl_id, req);
	if (ret != 0) {
		return ret;
	}

	return 0;
}

int
ssam_drv_vmio_req_poll_batch(uint16_t tid, uint16_t poll_num, struct vmio_request **req)
{
	int ret;
	struct ssam_drv_ops *ops = NULL;

	ops = ssam_get_drv_ops();
	SSAM_FUNC_PTR_OR_ERR_RET(ops->hvio_vmio_req_poll_batch, SSAM_DRV_FUNC_NO_PTR);

	ret = ops->hvio_vmio_req_poll_batch(tid, poll_num, req);
	if (ret < 0) {
		SPDK_ERRLOG("hvio_vmio_req_poll_batch exec fail, ret=%d\n", ret);
		return -1;
	}

	return ret;
}

int
ssam_drv_vmio_req_poll_batch_ext(uint16_t tid, uint16_t poll_num, struct vmio_request **req,
				 hvio_vmio_req_poll_opt_s *poll_opt)
{
	int ret;
	struct ssam_drv_ops *ops = NULL;

	ops = ssam_get_drv_ops();
	SSAM_FUNC_PTR_OR_ERR_RET(ops->hvio_vmio_req_poll_batch_ext, SSAM_DRV_FUNC_NO_PTR);

	ret = ops->hvio_vmio_req_poll_batch_ext(tid, poll_num, req, poll_opt);
	if (ret < 0) {
		SPDK_ERRLOG("hvio_vmio_req_poll_batch_ext exec fail, ret=%d\n", ret);
		return -1;
	}

	return ret;
}

int
ssam_drv_lib_deinit(void)
{
	int ret;
	struct ssam_drv_ops *ops = NULL;

	ops = ssam_get_drv_ops();
	SSAM_FUNC_PTR_OR_ERR_RET(ops->hvio_lib_deinit, SSAM_DRV_FUNC_NO_PTR);

	ret = ops->hvio_lib_deinit();
	if (ret != 0) {
		SPDK_ERRLOG("hvio_lib_deinit exec fail, ret=%d\n", ret);
		return -1;
	}

	return 0;
}

int
ssam_drv_volume_umount(uint16_t glb_function_id, uint32_t lun_id)
{
	int ret;
	struct ssam_drv_ops *ops = NULL;

	ops = ssam_get_drv_ops();
	SSAM_FUNC_PTR_OR_ERR_RET(ops->hvio_volume_umount, SSAM_DRV_FUNC_NO_PTR);

	ret = ops->hvio_volume_umount(glb_function_id, lun_id);
	if (ret != 0) {
		SPDK_ERRLOG("hvio_volume_umount exec fail, ret=%d\n", ret);
		return -1;
	}

	return 0;
}

int
ssam_drv_lib_init(hvio_lib_args_s *args_in, hvio_hostep_info_s *eps_out)
{
	int ret;
	struct ssam_drv_ops *ops = NULL;

	ops = ssam_get_drv_ops();
	SSAM_FUNC_PTR_OR_ERR_RET(ops->hvio_lib_init, SSAM_DRV_FUNC_NO_PTR);

	ret = ops->hvio_lib_init(args_in, eps_out);
	if (ret != 0) {
		SPDK_ERRLOG("hvio_lib_init exec fail, ret=%d\n", ret);
		return -1;
	}

	return 0;
}

int
ssam_drv_volume_mount(uint16_t glb_function_id, uint32_t lun_id, struct hvio_mount_para *hash_paras)
{
	int ret;
	struct ssam_drv_ops *ops = NULL;

	ops = ssam_get_drv_ops();
	SSAM_FUNC_PTR_OR_ERR_RET(ops->hvio_volume_mount, SSAM_DRV_FUNC_NO_PTR);

	ret = ops->hvio_volume_mount(glb_function_id, lun_id, hash_paras);
	if (ret != 0) {
		SPDK_ERRLOG("hvio_volume_mount exec fail, ret=%d\n", ret);
		return -1;
	}

	return 0;
}

int
ssam_drv_host_dma_rsp_poll(uint16_t chnl_id, uint16_t poll_num, hvio_host_dma_rsp_s *rsp)
{
	int ret;
	struct ssam_drv_ops *ops = NULL;

	ops = ssam_get_drv_ops();
	SSAM_FUNC_PTR_OR_ERR_RET(ops->hvio_host_dma_rsp_poll, SSAM_DRV_FUNC_NO_PTR);

	ret = ops->hvio_host_dma_rsp_poll(chnl_id, poll_num, rsp);
	if (ret < 0) {
		SPDK_ERRLOG("hvio_host_dma_rsp_poll exec fail, ret=%d\n", ret);
		return -1;
	}

	return ret;
}

int
ssam_drv_get_glb_function_id_by_dbdf(uint32_t dbdf, uint16_t *glb_function_id)
{
	int ret;
	struct ssam_drv_ops *ops = NULL;

	ops = ssam_get_drv_ops();
	SSAM_FUNC_PTR_OR_ERR_RET(ops->hvio_get_glb_function_id_by_dbdf, SSAM_DRV_FUNC_NO_PTR);

	ret = ops->hvio_get_glb_function_id_by_dbdf(dbdf, glb_function_id);
	if (ret != 0) {
		SPDK_ERRLOG("hvio_get_glb_function_id_by_dbdf exec fail, ret=%d", ret);
		return -1;
	}

	return 0;
}

int
ssam_drv_send_action(uint16_t glb_function_id, enum function_action action, const void *data,
		     uint16_t data_len)
{
	int ret;
	struct ssam_drv_ops *ops = NULL;

	ops = ssam_get_drv_ops();
	SSAM_FUNC_PTR_OR_ERR_RET(ops->hvio_send_action, SSAM_DRV_FUNC_NO_PTR);

	ret = ops->hvio_send_action(glb_function_id, action, data, data_len);
	if (ret != 0) {
		SPDK_ERRLOG("hvio_send_action exec fail, ret=%d", ret);
		return ret;
	}

	return 0;
}

int
ssam_drv_update_virtio_blk_capacity(uint16_t glb_function_id, uint64_t capacity)
{
	int ret;
	struct ssam_drv_ops *ops = NULL;

	ops = ssam_get_drv_ops();
	SSAM_FUNC_PTR_OR_ERR_RET(ops->hvio_update_virtio_blk_capacity, SSAM_DRV_FUNC_NO_PTR);

	ret = ops->hvio_update_virtio_blk_capacity(glb_function_id, capacity);
	if (ret != 0) {
		SPDK_ERRLOG("hvio_update_virtio_blk_capacity exec fail, ret=%d", ret);
		return -1;
	}

	return 0;
}

int
ssam_drv_setup_function(uint16_t pf_id, uint16_t num_vf, enum device_type pf_type,
			enum device_type vf_type)
{
	int ret;
	struct ssam_drv_ops *ops = NULL;

	ops = ssam_get_drv_ops();
	SSAM_FUNC_PTR_OR_ERR_RET(ops->hvio_setup_function, SSAM_DRV_FUNC_NO_PTR);

	ret = ops->hvio_setup_function(pf_id, num_vf, pf_type, vf_type);
	if (ret != 0) {
		SPDK_ERRLOG("hvio_setup_function exec fail, ret=%d", ret);
		return -1;
	}

	return 0;
}

int
ssam_drv_check_device_ready(uint8_t role, uint32_t proc_type, uint8_t *ready)
{
	int ret;
	struct ssam_drv_ops *ops = NULL;

	ops = ssam_get_drv_ops();
	SSAM_FUNC_PTR_OR_ERR_RET(ops->hvio_check_device_ready, SSAM_DRV_FUNC_NO_PTR);

	ret = ops->hvio_check_device_ready(role, proc_type, ready);
	if (ret != 0) {
		SPDK_ERRLOG("hvio_check_device_ready exec fail, ret=%d", ret);
		return -1;
	}

	return 0;
}

int
ssam_drv_write_function_config(struct function_config *cfg)
{
	int ret;
	struct ssam_drv_ops *ops = NULL;

	ops = ssam_get_drv_ops();
	SSAM_FUNC_PTR_OR_ERR_RET(ops->hvio_write_function_config, SSAM_DRV_FUNC_NO_PTR);

	ret = ops->hvio_write_function_config(cfg);
	if (ret != 0) {
		SPDK_ERRLOG("hvio_write_function_config exec fail, ret=%d", ret);
		return -1;
	}

	return 0;
}

int
ssam_drv_get_hot_upgrade_state(void)
{
	int ret;
	struct ssam_drv_ops *ops = NULL;

	ops = ssam_get_drv_ops();
	SSAM_FUNC_PTR_OR_ERR_RET(ops->hvio_get_hot_upgrade_state, SSAM_DRV_FUNC_NO_PTR);

	ret = ops->hvio_get_hot_upgrade_state();
	if (ret != 0) {
		SPDK_ERRLOG("hvio_get_hot_upgrade_state exec fail, ret=%d", ret);
		return -1;
	}

	return 0;
}

int
ssam_drv_vmio_complete(uint16_t tid, struct vmio_response *resp)
{
	int ret;
	struct ssam_drv_ops *ops = NULL;

	ops = ssam_get_drv_ops();
	SSAM_FUNC_PTR_OR_ERR_RET(ops->hvio_vmio_complete, SSAM_DRV_FUNC_NO_PTR);

	ret = ops->hvio_vmio_complete(tid, resp);
	if (ret != 0) {
		return ret;
	}

	return 0;
}

int
ssam_drv_vmio_rxq_create(uint16_t *queue_id_out)
{
	int ret;
	struct ssam_drv_ops *ops = NULL;

	ops = ssam_get_drv_ops();
	SSAM_FUNC_PTR_OR_ERR_RET(ops->hvio_vmio_rxq_create, SSAM_DRV_FUNC_NO_PTR);

	ret = ops->hvio_vmio_rxq_create(queue_id_out);
	if (ret != 0) {
		return ret;
	}

	return 0;
}

int
ssam_drv_update_virtio_device_used(uint16_t glb_function_id, uint64_t device_used)
{
	int ret;
	struct ssam_drv_ops *ops = NULL;

	ops = ssam_get_drv_ops();
	SSAM_FUNC_PTR_OR_ERR_RET(ops->hvio_update_virtio_device_used, SSAM_DRV_FUNC_NO_PTR);

	ret = ops->hvio_update_virtio_device_used(glb_function_id, device_used);
	if (ret != 0) {
		return ret;
	}

	return 0;
}

int
ssam_drv_virtio_blk_release_resource(uint16_t glb_function_id)
{
	int ret;
	struct ssam_drv_ops *ops = NULL;

	ops = ssam_get_drv_ops();
	SSAM_FUNC_PTR_OR_ERR_RET(ops->hvio_virtio_blk_release_resource, SSAM_DRV_FUNC_NO_PTR);

	ret = ops->hvio_virtio_blk_release_resource(glb_function_id);
	if (ret != 0) {
		return ret;
	}

	return 0;
}

int
ssam_drv_virtio_blk_alloc_resource(uint16_t glb_function_id, uint16_t queue_num)
{
	int ret;
	struct ssam_drv_ops *ops = NULL;

	ops = ssam_get_drv_ops();
	SSAM_FUNC_PTR_OR_ERR_RET(ops->hvio_virtio_blk_alloc_resource, SSAM_DRV_FUNC_NO_PTR);

	ret = ops->hvio_virtio_blk_alloc_resource(glb_function_id, queue_num);
	if (ret != 0) {
		return ret;
	}

	return 0;
}

int
ssam_drv_virtio_vq_bind_core(uint16_t glb_function_id, uint16_t queue_num)
{
	bool ret;
	struct ssam_drv_ops *ops = NULL;

	ops = ssam_get_drv_ops();
	SSAM_FUNC_PTR_OR_ERR_RET(ops->hvio_virtio_vq_bind_core, SSAM_DRV_FUNC_NO_PTR);

	ret = ops->hvio_virtio_vq_bind_core(glb_function_id, queue_num);
	if (ret != 0) {
		SPDK_ERRLOG("hvio_virtio_vq_bind_core exec fail, ret=%d", ret);
		return ret;
	}

	return 0;
}

int
ssam_drv_virtio_vq_unbind_core(uint16_t glb_function_id)
{
	bool ret;
	struct ssam_drv_ops *ops = NULL;

	ops = ssam_get_drv_ops();
	SSAM_FUNC_PTR_OR_ERR_RET(ops->hvio_virtio_vq_unbind_core, SSAM_DRV_FUNC_NO_PTR);

	ret = ops->hvio_virtio_vq_unbind_core(glb_function_id);
	if (ret != 0) {
		SPDK_ERRLOG("hvio_virtio_vq_unbind_core exec fail, ret=%d", ret);
		return ret;
	}

	return 0;
}
