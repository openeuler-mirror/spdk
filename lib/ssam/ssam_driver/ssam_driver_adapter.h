/*
 *   BSD LICENSE
 *   Copyright (c) Huawei Technologies Co., Ltd. 2021-2021. All rights reserved.
 *
 */

#ifndef SSAM_DRIVER_ADAPTER_H
#define SSAM_DRIVER_ADAPTER_H

#include "hivio_api.h"

struct ssam_drv_ops {
	int (*hvio_host_dma_request)(uint16_t chnl_id, hvio_host_dma_req_s *req);
	int (*hvio_vmio_req_poll_batch)(uint16_t tid, uint16_t poll_num, struct vmio_request **req);
	int (*hvio_vmio_req_poll_batch_ext)(uint16_t tid, uint16_t poll_num, struct vmio_request **req,
					    hvio_vmio_req_poll_opt_s *poll_opt);
	int (*hvio_lib_deinit)(void);
	int (*hvio_volume_umount)(uint16_t glb_function_id, uint32_t lun_id);
	int (*hvio_lib_init)(hvio_lib_args_s *args_in, hvio_hostep_info_s *eps_out);
	int (*hvio_volume_mount)(uint16_t glb_function_id, uint32_t lun_id,
				 struct hvio_mount_para *hash_paras);
	int (*hvio_host_dma_rsp_poll)(uint16_t chnl_id, uint16_t poll_num, hvio_host_dma_rsp_s *rsp);
	int (*hvio_get_glb_function_id_by_dbdf)(uint32_t dbdf, uint16_t *glb_function_id);
	int (*hvio_send_action)(uint16_t glb_function_id, enum function_action action, const void *data,
				uint16_t data_len);
	int (*hvio_update_virtio_blk_capacity)(uint16_t glb_function_id, uint64_t capacity);
	int (*hvio_setup_function)(uint16_t pf_id, uint16_t num_vf, enum device_type pf_type,
				   enum device_type vf_type);
	int (*hvio_check_device_ready)(uint8_t role, uint32_t proc_type, uint8_t *ready);
	int (*hvio_write_function_config)(struct function_config *cfg);
	int (*hvio_get_hot_upgrade_state)(void);
	int (*hvio_vmio_complete)(uint16_t tid, struct vmio_response *resp);
	int (*hvio_vmio_rxq_create)(uint16_t *queue_id_out);
	int (*hvio_update_virtio_device_used)(uint16_t glb_function_id, uint64_t device_used);
	int (*hvio_virtio_blk_release_resource)(uint16_t glb_function_id);
	int (*hvio_virtio_blk_alloc_resource)(uint16_t glb_function_id, uint16_t queue_num);
	void (*hvio_hotplug_cfg)(void);
	int (*hvio_hotplug_add)(uint16_t port_id);
	int (*hvio_hotplug_del)(uint16_t port_id);
	bool (*hvio_hotplug_enable_check)(void);
};

int ssam_drv_ops_init(void);
void ssam_drv_ops_uninit(void);
struct ssam_drv_ops *ssam_get_drv_ops(void);
int ssam_drv_host_dma_request(uint16_t chnl_id, hvio_host_dma_req_s *req);
int ssam_drv_vmio_req_poll_batch(uint16_t tid, uint16_t poll_num, struct vmio_request **req);
int ssam_drv_vmio_req_poll_batch_ext(uint16_t tid, uint16_t poll_num, struct vmio_request **req,
				     hvio_vmio_req_poll_opt_s *poll_opt);
int ssam_drv_lib_deinit(void);
int ssam_drv_volume_umount(uint16_t glb_function_id, uint32_t lun_id);
int ssam_drv_lib_init(hvio_lib_args_s *args_in, hvio_hostep_info_s *eps_out);
int ssam_drv_volume_mount(uint16_t glb_function_id, uint32_t lun_id,
			  struct hvio_mount_para *hash_paras);
int ssam_drv_host_dma_rsp_poll(uint16_t chnl_id, uint16_t poll_num, hvio_host_dma_rsp_s *rsp);
int ssam_drv_get_glb_function_id_by_dbdf(uint32_t dbdf, uint16_t *glb_function_id);
int ssam_drv_send_action(uint16_t glb_function_id, enum function_action action, const void *data,
			 uint16_t data_len);
int ssam_drv_update_virtio_blk_capacity(uint16_t glb_function_id, uint64_t capacity);
int ssam_drv_setup_function(uint16_t pf_id, uint16_t num_vf, enum device_type pf_type,
			    enum device_type vf_type);
int ssam_drv_check_device_ready(uint8_t role, uint32_t proc_type, uint8_t *ready);
int ssam_drv_write_function_config(struct function_config *cfg);
int ssam_drv_get_hot_upgrade_state(void);
int ssam_drv_vmio_complete(uint16_t tid, struct vmio_response *resp);
int ssam_drv_vmio_rxq_create(uint16_t *queue_id_out);
int ssam_drv_update_virtio_device_used(uint16_t glb_function_id, uint64_t device_used);
int ssam_drv_virtio_blk_release_resource(uint16_t glb_function_id);
int ssam_drv_virtio_blk_alloc_resource(uint16_t glb_function_id, uint16_t queue_num);
int ssam_drv_hotplug_cfg(void);
int ssam_drv_hotplug_add(uint16_t port_id);
int ssam_drv_hotplug_del(uint16_t port_id);
bool ssam_drv_hotplug_enable_check(void);
#endif
