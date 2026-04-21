/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021-2025 Huawei Technologies Co.
 *   All rights reserved.
 */

#ifndef SSAM_QOS_H
#define SSAM_QOS_H

#include "ssam_internal.h"

#define SINGLE_BUCKET_QOS_NUM  2
#define DOUBLE_BUCKET_QOS_NUM  4

#define SSAM_QOS_LEVEL_HOST 0
#define SSAM_QOS_LEVEL_GROUP 1
#define SSAM_QOS_LEVEL_FUNCTION 2

#define DECIMAL_BASE 10
#define MAX_PPS_CIR 128000000UL   //pps
#define MIN_PPS_CIR 1000UL        //pps
#define STEP_PPS_CIR 1000UL 	  //pps
#define MAX_PPS_CBS 1000000UL     //packet
#define MIN_PPS_CBS 1000UL        //packet
#define MAX_BPS_CIR 400000UL      //Mbps
#define MIN_BPS_CIR 1UL           //Mbps
#define STEP_BPS_CIR 100UL		  //Mbps
#define MAX_BPS_CBS 2560UL        //Mbit
#define MIN_BPS_CBS 1UL           //Mbit
#define BPS_STEP_SPLIT 1000UL
#define PPS_STEP_SPLIT_1 1000UL
#define PPS_STEP_SPLIT_2 128000000UL

#define MIBITS_TO_KIBITS 1000UL

#define MAX_GROUP_QOS_NUM 256
#define MIN_GROUP_QOS_ID 513
#define MAX_GROUP_QOS_ID 768
#define MAX_GROUP_NAME_LEN 512

#define QOS_MIN_FUNCTION_ID 0
#define QOS_MAX_FUNCTION_ID 4096

enum ssam_qos_cfg_type {
	SSAM_QOS_CIR,
	SSAM_QOS_CBS,
	SSAM_QOS_PIR,
	SSAM_QOS_PBS,
	SSAM_QOS_MAX,
};


enum ssam_qos_type {
	SSAM_QOS_PPS ,
	SSAM_QOS_BPS,
	SSAM_QOS_ALL,
};

struct rpc_ssam_qos_type {
	struct ssam_qos_cfg cfg;
	size_t qos_cfg_num;
};

struct ssam_qos_cfg_element {
	size_t qos_cfg_num;
	char *qos_str[SSAM_QOS_MAX];
};

struct rpc_ssam_function_qos {
	uint32_t func_id;
	struct ssam_qos_cfg_element rw_iops_cfg;
	struct ssam_qos_cfg_element rw_bw_cfg;
};

struct rpc_ssam_group_qos {
	char* group_name;
	struct ssam_qos_cfg_element rw_iops_cfg;
	struct ssam_qos_cfg_element rw_bw_cfg;
};

struct rpc_ssam_qos_request {
	struct ssam_qos_cfg_element rw_iops_cfg;
	struct ssam_qos_cfg_element rw_bw_cfg;
};

struct rpc_ssam_controller_group_qos_map {
	char * group_name;
	uint32_t func_id;
};


int ssam_create_function_qos(struct rpc_ssam_function_qos *req,
	struct rpc_ssam_qos_type *function_qos_cfg);

int ssam_get_function_qos(uint32_t func_id,
	struct ssam_qos_cfg *qos_cfg);

int ssam_create_group_qos(struct rpc_ssam_group_qos *req,
	struct rpc_ssam_qos_type *group_qos_cfg);

int ssam_create_host_qos(struct rpc_ssam_qos_request *req,
	struct rpc_ssam_qos_type *host_qos_cfg);

int ssam_qos_add_function_to_group(struct rpc_ssam_controller_group_qos_map *req);

int ssam_qos_delete_function_from_group(struct rpc_ssam_controller_group_qos_map *req);

int ssam_qos_get_function_of_group(char *group_name,
	struct ssam_qos_group_func *result);

int ssam_set_group_qos(struct rpc_ssam_group_qos *req,
	struct rpc_ssam_qos_type *group_qos_cfg);

int ssam_get_group_qos(char *group_name, struct ssam_qos_cfg *qos_cfg);

int ssam_delete_group_qos(char *name);

int ssam_get_host_qos(struct ssam_qos_cfg *qos_cfg);

int ssam_clear_qos_cfg(void);

void ssam_init_qos(void);

#endif
