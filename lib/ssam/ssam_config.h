/*
 *   BSD LICENSE
 *   Copyright (c) Huawei Technologies Co., Ltd. 2021-2021. All rights reserved.
 *
 */

#ifndef SSAM_CONFIG_H
#define SSAM_CONFIG_H

int ssam_set_core_num(uint32_t core_num);

uint16_t ssam_get_core_num(void);

uint32_t ssam_get_mempool_size(void);

uint16_t ssam_get_queues(void);

bool ssam_get_virtio_fs_enable(void);

enum ssam_device_type ssam_get_virtio_type(uint16_t gfunc_id);

int ssam_config_init(void);

void ssam_config_exit(void);

uint16_t ssam_get_queue_id(uint32_t func_id);

ssam_mempool_t *ssam_get_fs_mp(uint32_t func_id);

#endif /* SSAM_CONFIG_H */
