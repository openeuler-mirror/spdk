/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021-2025 Huawei Technologies Co.
 *   All rights reserved.
 */

#ifndef SSAM_CONFIG_H
#define SSAM_CONFIG_H

int ssam_set_core_num(uint32_t core_num);

uint16_t ssam_get_core_num(void);

uint32_t ssam_get_mempool_size(void);

uint16_t ssam_get_queues(void);

uint8_t ssam_get_hash_mode(void);

enum ssam_device_type ssam_get_virtio_type(uint16_t gfunc_id);

int ssam_config_init(void);

void ssam_config_exit(void);

#endif /* SSAM_CONFIG_H */
