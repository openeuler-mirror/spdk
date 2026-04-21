/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021-2025 Huawei Technologies Co.
 *   All rights reserved.
 */
 
#include "spdk/string.h"
#include "spdk/env.h"
#include "spdk/ssam.h"
#include "ssam_qos.h"

static int
decode_rpc_qos_info(const struct spdk_json_val *val, void *out)
{
	struct ssam_qos_cfg_element *cfg = out;

	return spdk_json_decode_array (val, spdk_json_decode_string, cfg->qos_str, DOUBLE_BUCKET_QOS_NUM,
					&cfg->qos_cfg_num, sizeof(char *));
}

static const struct spdk_json_object_decoder g_rpc_function_qos[] = {
	{"func_id",
		offsetof(struct rpc_ssam_function_qos, func_id),
		spdk_json_decode_uint32},
	{"rw_ios_per_sec",
		offsetof(struct rpc_ssam_function_qos, rw_iops_cfg),
		decode_rpc_qos_info, true},
	{"rw_mbits_per_sec",
		offsetof(struct rpc_ssam_function_qos, rw_bw_cfg),
		decode_rpc_qos_info, true},
};

static const struct spdk_json_object_decoder g_rpc_get_function_qos[] = {
	{"func_id",
		offsetof(struct rpc_ssam_function_qos, func_id),
		spdk_json_decode_uint32},
};

static const struct spdk_json_object_decoder g_rpc_controller_group_qos_decoders[] = {
	{"group_name",
		offsetof(struct rpc_ssam_group_qos, group_name),
		spdk_json_decode_string},
	{"rw_ios_per_sec",
		offsetof(struct rpc_ssam_group_qos, rw_iops_cfg),
		decode_rpc_qos_info, true},
	{"rw_mbits_per_sec",
		offsetof(struct rpc_ssam_group_qos, rw_bw_cfg),
		decode_rpc_qos_info, true},
};

static const struct spdk_json_object_decoder g_rpc_controller_group_create_decoders[] = {
	{"group_name",
		offsetof(struct rpc_ssam_group_qos, group_name),
		spdk_json_decode_string},
};

static const struct spdk_json_object_decoder g_rpc_controller_get_group_qos_decoders[] = {
	{"group_name",
		offsetof(struct rpc_ssam_group_qos, group_name),
		spdk_json_decode_string},
};

static const struct spdk_json_object_decoder g_rpc_group_qos_map_decoders[] = {
	{"group_name",
		offsetof(struct rpc_ssam_controller_group_qos_map, group_name),
		spdk_json_decode_string},
	{"func_id",
		offsetof(struct rpc_ssam_controller_group_qos_map, func_id),
		spdk_json_decode_uint32},
};


static const struct spdk_json_object_decoder g_rpc_qos_decoders[] = {
	{"rw_ios_per_sec",
		offsetof(struct rpc_ssam_qos_request, rw_iops_cfg),
		decode_rpc_qos_info},
	{"rw_mbits_per_sec",
		offsetof(struct rpc_ssam_qos_request, rw_bw_cfg),
		decode_rpc_qos_info},
};

static int
rpc_dump_host_qos_cfg(void *ctx, struct ssam_qos_cfg *qos_cfg)
{
	struct spdk_json_write_ctx *w = ctx;

	spdk_json_write_named_uint32(w, "pps_cir", qos_cfg->pps_cir);
	spdk_json_write_named_uint32(w, "pps_cbs", qos_cfg->pps_cbs);

	spdk_json_write_named_uint32(w, "mbps_cir", qos_cfg->bps_cir);
	spdk_json_write_named_uint32(w, "mbps_cbs", qos_cfg->bps_cbs);

	return 0;
}

static int
rpc_dump_qos_cfg(void *ctx, struct ssam_qos_cfg *qos_cfg)
{
	struct spdk_json_write_ctx *w = ctx;

	spdk_json_write_named_uint32(w, "pps_cir", qos_cfg->pps_cir);
	spdk_json_write_named_uint32(w, "pps_cbs", qos_cfg->pps_cbs);

	spdk_json_write_named_uint32(w, "pps_pir", qos_cfg->pps_pir);
	spdk_json_write_named_uint32(w, "pps_pbs", qos_cfg->pps_pbs);

	spdk_json_write_named_uint32(w, "mbps_cir", qos_cfg->bps_cir);
	spdk_json_write_named_uint32(w, "mbps_cbs", qos_cfg->bps_cbs);

	spdk_json_write_named_uint32(w, "mbps_pir", qos_cfg->bps_pir);
	spdk_json_write_named_uint32(w, "mbps_pbs", qos_cfg->bps_pbs);

	return 0;
}

/* For function level QoS, only DOUBLE_BUCKET_QOS_NUM is supported.
 * For group level QoS, only DOUBLE_BUCKET_QOS_NUM is supported.
 * For host level QoS, only SINGLE_BUCKET_QOS_NUM is supported.
*/

static void
rpc_ssam_controller_function_set_qos_limit(struct spdk_jsonrpc_request *request,
	const struct spdk_json_val *params)
{
	struct rpc_ssam_qos_type function_qos_cfg = {{0}, DOUBLE_BUCKET_QOS_NUM};
	struct rpc_ssam_function_qos req = {0};
	struct ssam_qos_cfg *cfg = NULL;
	int rc = 0;

	if (params == NULL) {
		SPDK_ERRLOG("invalid qos params.\n");
		spdk_jsonrpc_send_error_response(request,
					SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
					"paras is null");
		goto cleanup;
	}

	if (spdk_json_decode_object(params, g_rpc_function_qos,
				SPDK_COUNTOF(g_rpc_function_qos),
				&req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request,
					SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
					"spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = ssam_create_function_qos(&req, &function_qos_cfg);
	if (rc != 0) {
		SPDK_ERRLOG("ssam create function qos failed\n");
		spdk_jsonrpc_send_error_response(request,
					SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					spdk_strerror(-rc));
		goto cleanup;
	}

	spdk_jsonrpc_send_bool_response(request, true);
	cfg = &function_qos_cfg.cfg;
	SPDK_NOTICELOG("set func(%u) qos cfg(IOPS):pps_cir(%u), pps_cbs(%u), pps_pir(%u), pps_pbs(%u).\n",
	req.func_id, cfg->pps_cir, cfg->pps_cbs, cfg->pps_pir, cfg->pps_pbs);
	SPDK_NOTICELOG("set func(%u) qos cfg(BW):mbps_cir(%lu), mbps_cbs(%lu), mbps_pir(%lu), mbps_pbs(%lu).\n",
	req.func_id, cfg->bps_cir / MIBITS_TO_KIBITS, cfg->bps_cbs / MIBITS_TO_KIBITS, cfg->bps_pir / MIBITS_TO_KIBITS, cfg->bps_pbs / MIBITS_TO_KIBITS);

cleanup:
	for (int i = 0; i < SSAM_QOS_MAX; i++) {
		if (req.rw_iops_cfg.qos_str[i] != NULL)
			free(req.rw_iops_cfg.qos_str[i]);
		if (req.rw_bw_cfg.qos_str[i] != NULL)
			free(req.rw_bw_cfg.qos_str[i]);
	}
}

SPDK_RPC_REGISTER("controller_function_set_qos_limit",
		rpc_ssam_controller_function_set_qos_limit, SPDK_RPC_RUNTIME)

static void
rpc_ssam_controller_function_get_qos_limit(struct spdk_jsonrpc_request *request,
				const struct spdk_json_val *params)
{
	struct ssam_qos_cfg function_qos_cfg = {0};
	struct rpc_ssam_function_qos req = {0};
	struct spdk_json_write_ctx *w = NULL;
	int rc = 0;

	if (params == NULL) {
		SPDK_ERRLOG("function qos get params is null\n");
		spdk_jsonrpc_send_error_response(request,
					SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
					"paras is null");
		goto cleanup;
	}

	if (spdk_json_decode_object(params, g_rpc_get_function_qos,
				SPDK_COUNTOF(g_rpc_get_function_qos),
				&req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request,
					SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
					"spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = ssam_get_function_qos(req.func_id, &function_qos_cfg);
	if (rc != 0) {
		SPDK_ERRLOG("ssam get function qos failed\n");
		spdk_jsonrpc_send_error_response(request,
					SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					spdk_strerror(-rc));
		goto cleanup;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_object_begin(w);
	spdk_json_write_named_uint32(w, "function_id", req.func_id);
	rpc_dump_qos_cfg(w, &function_qos_cfg);
	spdk_json_write_object_end(w);
	spdk_jsonrpc_end_result(request, w);

cleanup:
	for (int i = 0; i < SSAM_QOS_MAX; i++) {
		if (req.rw_iops_cfg.qos_str[i] != NULL)
			free(req.rw_iops_cfg.qos_str[i]);
		if (req.rw_bw_cfg.qos_str[i] != NULL)
			free(req.rw_bw_cfg.qos_str[i]);
	}
}

SPDK_RPC_REGISTER("controller_function_get_qos_limit",
			rpc_ssam_controller_function_get_qos_limit, SPDK_RPC_RUNTIME)


static void
rpc_ssam_controller_create_group_qos_limit(struct spdk_jsonrpc_request *request,
	const struct spdk_json_val *params)
{
	struct rpc_ssam_qos_type group_qos_cfg = {{0}, DOUBLE_BUCKET_QOS_NUM};
	struct rpc_ssam_group_qos req = {0};
	int rc = 0;

	if (params == NULL) {
		SPDK_ERRLOG("create group qos limit params is null\n");
		spdk_jsonrpc_send_error_response(request,
					SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
					"paras is null");
		goto cleanup;
	}

	if (spdk_json_decode_object(params,
			g_rpc_controller_group_create_decoders,
			SPDK_COUNTOF(g_rpc_controller_group_create_decoders),
			&req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request,
						SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						"spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = ssam_create_group_qos(&req, &group_qos_cfg);
	if (rc != 0) {
		SPDK_ERRLOG("ssam create group qos limit failed\n");
		spdk_jsonrpc_send_error_response(request,
					SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					spdk_strerror(-rc));
		goto cleanup;
	}

	spdk_jsonrpc_send_bool_response(request, true);
	SPDK_NOTICELOG("create group(%s) done.\n", req.group_name);

cleanup:
	if (req.group_name)
		free(req.group_name);

	for (int i = 0; i < SSAM_QOS_MAX; i++) {
		if (req.rw_iops_cfg.qos_str[i] != NULL)
			free(req.rw_iops_cfg.qos_str[i]);
		if (req.rw_bw_cfg.qos_str[i] != NULL)
			free(req.rw_bw_cfg.qos_str[i]);
	}
}

SPDK_RPC_REGISTER("controller_create_group_qos_limit",
			rpc_ssam_controller_create_group_qos_limit, SPDK_RPC_RUNTIME)

static void
rpc_ssam_controller_delete_group_qos_limit(struct spdk_jsonrpc_request *request,
	const struct spdk_json_val *params)
{
	/* only use element group_name of struct rpc_ssam_group_qos */
	struct rpc_ssam_group_qos req = {0};
	int rc = 0;

	if (params == NULL) {
		SPDK_ERRLOG("delete group qos limit params is null\n");
		spdk_jsonrpc_send_error_response(request,
						SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						"paras is null");
		goto cleanup;
	}

	if (spdk_json_decode_object(params,
				g_rpc_controller_get_group_qos_decoders,
				SPDK_COUNTOF(g_rpc_controller_get_group_qos_decoders),
				&req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request,
				SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
				"spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = ssam_delete_group_qos(req.group_name);
	if (rc != 0) {
		SPDK_ERRLOG("ssam delete group qos failed\n");
		spdk_jsonrpc_send_error_response(request,
				SPDK_JSONRPC_ERROR_INVALID_PARAMS,
				spdk_strerror(-rc));
		goto cleanup;
	}

	spdk_jsonrpc_send_bool_response(request, true);
	SPDK_NOTICELOG("delete group(%s) done.\n", req.group_name);

cleanup:
	if (req.group_name)
		free(req.group_name);

	for (int i = 0; i < SSAM_QOS_MAX; i++) {
		if (req.rw_iops_cfg.qos_str[i] != NULL)
			free(req.rw_iops_cfg.qos_str[i]);
		if (req.rw_bw_cfg.qos_str[i] != NULL)
			free(req.rw_bw_cfg.qos_str[i]);
	}
}

SPDK_RPC_REGISTER("controller_delete_group_qos_limit",
			rpc_ssam_controller_delete_group_qos_limit, SPDK_RPC_RUNTIME)

static void
rpc_ssam_controller_qos_add_function_to_group(struct spdk_jsonrpc_request *request,
	const struct spdk_json_val *params)
{
	struct rpc_ssam_controller_group_qos_map req = {0};
	int rc = 0;
	
	if (params == NULL) {
		SPDK_ERRLOG("qos add function to group params is NULL\n");
		spdk_jsonrpc_send_error_response(request,
						SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						"paras is null");
		goto cleanup;
	}

	if (spdk_json_decode_object(params, g_rpc_group_qos_map_decoders,
				SPDK_COUNTOF(g_rpc_group_qos_map_decoders),
				&req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request,
				SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
				"spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = ssam_qos_add_function_to_group(&req);
	if (rc != 0) {
		SPDK_ERRLOG("ssam qos add function to group failed\n");
		spdk_jsonrpc_send_error_response(request,
					SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					spdk_strerror(-rc));
		goto cleanup;
	}

	spdk_jsonrpc_send_bool_response(request, true);
	SPDK_NOTICELOG("add func(%u) to group(%s) done.\n", req.func_id, req.group_name);

cleanup:
	if (req.group_name)
		free(req.group_name);
}

SPDK_RPC_REGISTER("controller_qos_add_function_to_group",
			rpc_ssam_controller_qos_add_function_to_group,
			SPDK_RPC_RUNTIME)

static void
rpc_ssam_controller_qos_delete_function_from_group(struct spdk_jsonrpc_request *request,
	const struct spdk_json_val *params)
{
	struct rpc_ssam_controller_group_qos_map req = {0};
	int rc = 0;

	if (params == NULL) {
		SPDK_ERRLOG("qos delete function from group params is NULL\n");
		spdk_jsonrpc_send_error_response(request,
						SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						"paras is null");
		goto cleanup;
	}

	if (spdk_json_decode_object(params, g_rpc_group_qos_map_decoders,
				SPDK_COUNTOF(g_rpc_group_qos_map_decoders),
				&req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request,
				SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
				"spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = ssam_qos_delete_function_from_group(&req);
	if (rc != 0) {
		SPDK_ERRLOG("ssam qos delete function from group failed\n");
		spdk_jsonrpc_send_error_response(request,
					SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					spdk_strerror(-rc));
		goto cleanup;
	}

	spdk_jsonrpc_send_bool_response(request, true);
	SPDK_NOTICELOG("delete func(%u) from group(%s) done.\n", req.func_id, req.group_name);

cleanup:
	if (req.group_name)
		free(req.group_name);
}

SPDK_RPC_REGISTER("controller_qos_delete_function_from_group",
			rpc_ssam_controller_qos_delete_function_from_group,
			SPDK_RPC_RUNTIME)

static void
rpc_ssam_controller_qos_get_function_of_group(struct spdk_jsonrpc_request *request,
	const struct spdk_json_val *params)
{
	struct ssam_qos_group_func result = {0};
	/* only use element group_name of struct rpc_ssam_group_qos */
	struct rpc_ssam_group_qos req = {0};
	struct spdk_json_write_ctx *w = NULL;
	int rc = 0;
	uint32_t i;

	if (params == NULL) {
		SPDK_ERRLOG("qos delete function from group params is NULL\n");
		spdk_jsonrpc_send_error_response(request,
						SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						"paras is null");
		goto cleanup;
	}

	if (spdk_json_decode_object(params,
			g_rpc_controller_get_group_qos_decoders,
			SPDK_COUNTOF(g_rpc_controller_get_group_qos_decoders),
			&req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request,
				SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
				"spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = ssam_qos_get_function_of_group(req.group_name, &result);
	if (rc != 0) {
		SPDK_ERRLOG("ssam qos get function of group failed\n");
		spdk_jsonrpc_send_error_response(request,
					SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					spdk_strerror(-rc));
		goto cleanup;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "group_name",
		req.group_name);
	spdk_json_write_named_uint32(w, "group id", result.group_id);
	spdk_json_write_named_array_begin(w, "function list");
	for (i = 0; i < result.func_info.func_num; i++)
		spdk_json_write_uint16(w, result.func_info.func_list[i]);
	spdk_json_write_array_end(w);
	spdk_json_write_object_end(w);
	spdk_jsonrpc_end_result(request, w);

cleanup:
	if (req.group_name)
		free(req.group_name);

	for (i = 0; i < SSAM_QOS_MAX; i++) {
		if (req.rw_iops_cfg.qos_str[i] != NULL)
			free(req.rw_iops_cfg.qos_str[i]);
		if (req.rw_bw_cfg.qos_str[i] != NULL)
			free(req.rw_bw_cfg.qos_str[i]);
	}
}

SPDK_RPC_REGISTER("controller_qos_get_function_of_group",
			rpc_ssam_controller_qos_get_function_of_group,
			SPDK_RPC_RUNTIME)

static void
rpc_ssam_controller_group_set_qos_limit(struct spdk_jsonrpc_request *request,
				const struct spdk_json_val *params)
{
	struct rpc_ssam_qos_type group_qos_cfg = {{0}, DOUBLE_BUCKET_QOS_NUM};
	struct rpc_ssam_group_qos req = {0};
	struct ssam_qos_cfg *cfg = NULL;
	int rc = 0;

	if (params == NULL) {
		SPDK_ERRLOG("group qos set params is NULL\n");
		spdk_jsonrpc_send_error_response(request,
					SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
					"paras is null");
		goto cleanup;
	}

	if (spdk_json_decode_object(params,
			g_rpc_controller_group_qos_decoders,
			SPDK_COUNTOF(g_rpc_controller_group_qos_decoders),
			&req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request,
				SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
				"spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = ssam_set_group_qos(&req, &group_qos_cfg);
	if (rc != 0) {
		SPDK_ERRLOG("ssam create group qos limit failed\n");
		spdk_jsonrpc_send_error_response(request,
					SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					spdk_strerror(-rc));
		goto cleanup;
	}

	spdk_jsonrpc_send_bool_response(request, true);
	cfg = &group_qos_cfg.cfg;
	SPDK_NOTICELOG("set group(%s) qos cfg(IOPS):pps_cir(%u), pps_cbs(%u), pps_pir(%u), pps_pbs(%u).\n",
	req.group_name, cfg->pps_cir, cfg->pps_cbs, cfg->pps_pir, cfg->pps_pbs);
	SPDK_NOTICELOG("set group(%s) qos cfg(BW):mbps_cir(%lu), mbps_cbs(%lu), mbps_pir(%lu), mbps_pbs(%lu).\n",
	req.group_name, cfg->bps_cir / MIBITS_TO_KIBITS, cfg->bps_cbs / MIBITS_TO_KIBITS, cfg->bps_pir / MIBITS_TO_KIBITS, cfg->bps_pbs / MIBITS_TO_KIBITS);

cleanup:
	if (req.group_name)
		free(req.group_name);

	for (int i = 0; i < SSAM_QOS_MAX; i++) {
		if (req.rw_iops_cfg.qos_str[i] != NULL)
			free(req.rw_iops_cfg.qos_str[i]);
		if (req.rw_bw_cfg.qos_str[i] != NULL)
			free(req.rw_bw_cfg.qos_str[i]);
	}
}

SPDK_RPC_REGISTER("controller_group_set_qos_limit",
			rpc_ssam_controller_group_set_qos_limit, SPDK_RPC_RUNTIME)

static void
rpc_ssam_controller_group_get_qos_limit(struct spdk_jsonrpc_request *request,
				const struct spdk_json_val *params)
{
	struct ssam_qos_cfg group_qos_cfg = {0};
	struct rpc_ssam_group_qos req = {0};
	struct spdk_json_write_ctx *w = NULL;
	int rc = 0;

	if (params == NULL) {
		SPDK_ERRLOG("group qos get params is null\n");
		spdk_jsonrpc_send_error_response(request,
						SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						"paras is null");
		goto cleanup;
	}

	if (spdk_json_decode_object(params,
			g_rpc_controller_get_group_qos_decoders,
			SPDK_COUNTOF(g_rpc_controller_get_group_qos_decoders),
			&req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request,
				SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
				"spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = ssam_get_group_qos(req.group_name, &group_qos_cfg);
	if (rc != 0) {
		SPDK_ERRLOG("ssam get group qos failed\n");
		spdk_jsonrpc_send_error_response(request,
					SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					spdk_strerror(-rc));
		goto cleanup;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "group_name",
		req.group_name);
	rpc_dump_qos_cfg(w, &group_qos_cfg);
	spdk_json_write_object_end(w);
	spdk_jsonrpc_end_result(request, w);

cleanup:
	if (req.group_name)
		free(req.group_name);

	for (int i = 0; i < SSAM_QOS_MAX; i++) {
		if (req.rw_iops_cfg.qos_str[i] != NULL)
			free(req.rw_iops_cfg.qos_str[i]);
		if (req.rw_bw_cfg.qos_str[i] != NULL)
			free(req.rw_bw_cfg.qos_str[i]);
	}
}

SPDK_RPC_REGISTER("controller_group_get_qos_limit",
			rpc_ssam_controller_group_get_qos_limit, SPDK_RPC_RUNTIME)


static void
rpc_ssam_controller_host_set_qos_limit(
	struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_ssam_qos_request req = {0};
	struct rpc_ssam_qos_type host_qos_cfg = {{0}, SINGLE_BUCKET_QOS_NUM};
	struct ssam_qos_cfg *cfg = NULL;
	int rc = 0;

	if (params == NULL) {
		SPDK_ERRLOG("host qos set params is null\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "paras is null");
		goto cleanup;
	}

	if (spdk_json_decode_object(params, g_rpc_qos_decoders, SPDK_COUNTOF(g_rpc_qos_decoders), &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = ssam_create_host_qos(&req, &host_qos_cfg);
	if (rc != 0) {
		SPDK_ERRLOG("ssam create host qos failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, spdk_strerror(-rc));
		goto cleanup;
	}

	spdk_jsonrpc_send_bool_response(request, true);
	cfg = &host_qos_cfg.cfg;

	SPDK_NOTICELOG("set host qos cfg:pps_cir(%u), pps_cbs(%u), mbps_cir(%lu), mbps_cbs(%lu) .\n",
	cfg->pps_cir, cfg->pps_cbs, cfg->bps_cir / MIBITS_TO_KIBITS, cfg->bps_cbs / MIBITS_TO_KIBITS);

cleanup:
	for (int i = 0; i < SSAM_QOS_MAX; i++) {
		if (req.rw_iops_cfg.qos_str[i] != NULL)
			free(req.rw_iops_cfg.qos_str[i]);
		if (req.rw_bw_cfg.qos_str[i] != NULL)
			free(req.rw_bw_cfg.qos_str[i]);
	}
}

SPDK_RPC_REGISTER("controller_host_set_qos_limit",
		rpc_ssam_controller_host_set_qos_limit, SPDK_RPC_RUNTIME)

static void
rpc_ssam_controller_host_get_qos_limit(struct spdk_jsonrpc_request *request,
				const struct spdk_json_val *params)
{
	struct ssam_qos_cfg host_qos_cfg = {0};
	struct spdk_json_write_ctx *w = NULL;
	int rc = 0;

	rc = ssam_get_host_qos(&host_qos_cfg);
	if (rc != 0) {
		SPDK_ERRLOG("ssam get host qos failed\n");
		spdk_jsonrpc_send_error_response(request,
					SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					spdk_strerror(-rc));
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_object_begin(w);
	rpc_dump_host_qos_cfg(w, &host_qos_cfg);
	spdk_json_write_object_end(w);
	spdk_jsonrpc_end_result(request, w);
}

SPDK_RPC_REGISTER("controller_host_get_qos_limit",
		rpc_ssam_controller_host_get_qos_limit, SPDK_RPC_RUNTIME)


static void
rpc_ssam_clear_qos_limit(struct spdk_jsonrpc_request *request,
				const struct spdk_json_val *params)
{
	int rc = 0;

	rc = ssam_clear_qos_cfg();
	if (rc != 0) {
		SPDK_ERRLOG("ssam clear qos cfg failed\n");
		spdk_jsonrpc_send_error_response(request,
					SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					spdk_strerror(-rc));
		return;
	}
	SPDK_NOTICELOG("ssam clear qos cfg success\n");
	spdk_jsonrpc_send_bool_response(request, true);
}

SPDK_RPC_REGISTER("controller_clear_qos_limit",
		rpc_ssam_clear_qos_limit, SPDK_RPC_RUNTIME)
