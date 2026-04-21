/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021-2025 Huawei Technologies Co.
 *   All rights reserved.
 */

 #include "ssam_qos.h"
 #include "spdk/init.h"

static struct ssam_qos_cfg  g_ssam_global_qos_cfg __attribute__((__unused__));

static char *g_group_qos_name[MAX_GROUP_QOS_ID + 1] = {NULL};
static int g_group_qos_cnt;
static char *pattern_special = "[@#$&*]";
static regex_t regex;

static int
_check_iops_qos_limit(struct rpc_ssam_qos_type *qos_cfg)
{
	uint32_t cir = qos_cfg->cfg.pps_cir;
	uint32_t cbs = qos_cfg->cfg.pps_cbs;
	uint32_t pir = qos_cfg->cfg.pps_pir;
	uint32_t pbs = qos_cfg->cfg.pps_pbs;

	/* check iops step size for cir */
	if (cir >= PPS_STEP_SPLIT_1 && cir < PPS_STEP_SPLIT_2) {
		if ((cir - PPS_STEP_SPLIT_1) % STEP_PPS_CIR != 0) {
			SPDK_ERRLOG("step size of iops cir should be 1K when cir is in [1000, 128000000). cir: %"PRIu32"\n", cir);
			return -1;
		}
	}

	/* check SINGLE BUCKET QoS cfg */
	if (qos_cfg->qos_cfg_num == SINGLE_BUCKET_QOS_NUM) {
		if (cir == 0 && cbs == 0) {
			return 0;
		}
		if (cir >= MAX_PPS_CIR || cir < MIN_PPS_CIR) {
			SPDK_ERRLOG("iops cir is out of range. cir: %" PRIu32 ", range [minimum: %lu, maximum: %lu)\n",
				cir, MIN_PPS_CIR, MAX_PPS_CIR);
			return -1;
		}

		if (cbs > MAX_PPS_CBS || cbs < MIN_PPS_CBS) {
		SPDK_ERRLOG("iops cbs is out of range. cbs:%" PRIu32 ", range [minimum: %lu, maximum: %lu]\n",
				cbs, MIN_PPS_CBS, MAX_PPS_CBS);
			return -1;
		}
		return 0;
	}

	/* check iops step size for pir */
	if (pir >= PPS_STEP_SPLIT_1 && pir < PPS_STEP_SPLIT_2) {
		if ((pir - PPS_STEP_SPLIT_1) % STEP_PPS_CIR != 0) {
			SPDK_ERRLOG("step size of iops pir should be 1K when pir is in [1000, 128000000). pir: %"PRIu32"\n", pir);
			return -1;
		}
	}

	/* check DOUBLE BUCKET QoS cfg */
	if (qos_cfg->qos_cfg_num == DOUBLE_BUCKET_QOS_NUM) {
		if (cir == 0 && cbs == 0 && pir == 0 && pbs == 0) {
			return 0;
		}
		if (pir >= MAX_PPS_CIR || pir < MIN_PPS_CIR) {
			SPDK_ERRLOG("iops pir is out of range. pir:%" PRIu32 ", range [minimum: %lu, maximum: %lu)\n",
				pir, MIN_PPS_CIR, MAX_PPS_CIR);
			return -1;
		}
		if (cir > pir) {
			SPDK_ERRLOG("iops cir is out of range. cir(%" PRIu32 ") < pir(%" PRIu32 ")\n", cir, pir);
			return -1;
		}
		if (cir >= MAX_PPS_CIR || cir < MIN_PPS_CIR) {
			SPDK_ERRLOG("iops cir is out of range. cir: %" PRIu32 ", range [minimum: %lu, maximum: %lu)\n",
				cir, MIN_PPS_CIR, MAX_PPS_CIR);
			return -1;
		}
		if (pbs > MAX_PPS_CBS || pbs < MIN_PPS_CBS) {
			SPDK_ERRLOG("iops pbs is out of range. pbs:%" PRIu32 ", range [minimum:%lu, maximum: %lu]\n",
				pbs, MIN_PPS_CBS, MAX_PPS_CBS);
			return -1;
		}
		if (cbs > pbs) {
			SPDK_ERRLOG("iops cbs is out of range. cbs(%" PRIu32 ") < pbs(%" PRIu32 ")\n", cbs, pbs);
			return -1;
		}
		if (cbs > MAX_PPS_CBS || cbs < MIN_PPS_CBS) {
			SPDK_ERRLOG("iops cbs is out of range. cbs:%" PRIu32 ", range [minimum:%lu, maximum: %lu]\n",
				cbs, MIN_PPS_CBS, MAX_PPS_CBS);
			return -1;
		}

		return 0;
	}

	SPDK_ERRLOG("iops qos_cfg_num failed, qos_cfg_num :%lu\n", qos_cfg->qos_cfg_num);
	return -1;
}

static int
_check_bw_qos_limit(struct rpc_ssam_qos_type *qos_cfg)
{
	uint32_t cir = qos_cfg->cfg.bps_cir;
	uint32_t cbs = qos_cfg->cfg.bps_cbs;
	uint32_t pir = qos_cfg->cfg.bps_pir;
	uint32_t pbs = qos_cfg->cfg.bps_pbs;

	/* check bw step size for cir */
	if (cir > BPS_STEP_SPLIT) {
		if ((cir - BPS_STEP_SPLIT) % STEP_BPS_CIR != 0) {
			SPDK_ERRLOG("step size of bw cir should be 100M when cir is in [1000Mbps, 400000Mbps). cir:%"PRIu32" Mbps\n", cir);
			return -1;
		}
	}

	/* check SINGLE BUCKET QoS cfg */
	if (qos_cfg->qos_cfg_num == SINGLE_BUCKET_QOS_NUM) {
		if (cir == 0 && cbs == 0) {
			return 0;
		}
		if (cir >= MAX_BPS_CIR || cir < MIN_BPS_CIR) {
			SPDK_ERRLOG("bw cir is out of range. cir: %" PRIu32 ", range [minimum: %lu, maximum: %lu)\n",
				cir, MIN_BPS_CIR, MAX_BPS_CIR);
			return -1;
		}
		if (cbs > MAX_BPS_CBS || cbs < MIN_BPS_CBS) {
			SPDK_ERRLOG("bw cbs is out of range. cbs:%" PRIu32 ", range [minimum: %lu, maximum: %lu]\n",
				cbs, MIN_BPS_CBS, MAX_BPS_CBS);
			return -1;
		}

		qos_cfg->cfg.bps_cir = qos_cfg->cfg.bps_cir * MIBITS_TO_KIBITS;
		qos_cfg->cfg.bps_cbs = qos_cfg->cfg.bps_cbs * MIBITS_TO_KIBITS;

		return 0;

	}

	/* check bw step size for pir */
	if (pir > BPS_STEP_SPLIT) {
		if ((pir - BPS_STEP_SPLIT) % STEP_BPS_CIR != 0) {
			SPDK_ERRLOG("step size of bw pir should be 100M when pir is in [1000Mbps, 400000Mbps). pir:%"PRIu32" Mbps\n", pir);
			return -1;
		}
	}

	/* check DOUBLE BUCKET QoS cfg */
	if (qos_cfg->qos_cfg_num == DOUBLE_BUCKET_QOS_NUM) {
		if (cir == 0 && cbs == 0 && pir == 0 && pbs == 0) {
			return 0;
		}
		if (pir >= MAX_BPS_CIR || pir < MIN_BPS_CIR) {
			SPDK_ERRLOG("bw pir is out of range. pir:%" PRIu32 ", range(Mbps) [minimum: %lu, maximum: %lu)\n",
				pir, MIN_BPS_CIR, MAX_BPS_CIR);
			return -1;
		}
		if (cir > pir) {
			SPDK_ERRLOG("bw cir is out of range. cir(%" PRIu32 ") < pir(%" PRIu32 ")\n", cir, pir);
			return -1;
		}
		if (cir >= MAX_BPS_CIR || cir < MIN_BPS_CIR) {
			SPDK_ERRLOG("bw cir is out of range. cir:%" PRIu32 ", range(Mbps) [minimum: %lu, maximum: %lu)\n",
				cbs, MIN_BPS_CIR, MAX_BPS_CIR);
			return -1;
		}
		if (pbs > MAX_BPS_CBS || pbs < MIN_BPS_CBS) {
			SPDK_ERRLOG("bw pbs is out of range. pbs:%" PRIu32 ", range(Mbps) [minimum: %lu, maximum: %lu]\n",
				pbs, MIN_BPS_CBS, MAX_BPS_CBS);
			return -1;
		}
		if (cbs > pbs) {
			SPDK_ERRLOG("bw cbs is out of range. cbs(%" PRIu32 ") < pbs(%" PRIu32 ")\n", cbs, pbs);
			return -1;
		}

		if (cbs > MAX_BPS_CBS || cbs < MIN_BPS_CBS) {
			SPDK_ERRLOG("bw cbs is out of range. cbs:%" PRIu32 ", range(Mbps) [minimum: %lu, maximum: %lu)\n",
				cbs, MIN_BPS_CBS, MAX_BPS_CBS);
			return -1;
		}

		qos_cfg->cfg.bps_cir = qos_cfg->cfg.bps_cir * MIBITS_TO_KIBITS;
		qos_cfg->cfg.bps_cbs = qos_cfg->cfg.bps_cbs * MIBITS_TO_KIBITS;
		qos_cfg->cfg.bps_pir = qos_cfg->cfg.bps_pir * MIBITS_TO_KIBITS;
		qos_cfg->cfg.bps_pbs = qos_cfg->cfg.bps_pbs * MIBITS_TO_KIBITS;
		return 0;
	}

	SPDK_ERRLOG("bw qos_cfg_num failed, qos_cfg_num :%lu\n", qos_cfg->qos_cfg_num);
	return -1;
}

static int
_check_qos_limit(struct rpc_ssam_qos_type *qos_cfg)
{
	if (qos_cfg->cfg.qos_type == SSAM_QOS_PPS)
		return _check_iops_qos_limit(qos_cfg);

	if (qos_cfg->cfg.qos_type == SSAM_QOS_BPS)
		return _check_bw_qos_limit(qos_cfg);

	if (qos_cfg->cfg.qos_type == SSAM_QOS_ALL) {
		if (_check_iops_qos_limit(qos_cfg) != 0) {
			return -1;
		}
		return _check_bw_qos_limit(qos_cfg);
	}
	return -1;
}

static int
_parse_bw_qos_info(struct rpc_ssam_qos_request *req, struct rpc_ssam_qos_type *qos_cfg)
{
	char *endptr = NULL;

	qos_cfg->cfg.bps_cir = strtoul(req->rw_bw_cfg.qos_str[SSAM_QOS_CIR], &endptr, DECIMAL_BASE);
	if (endptr == NULL || *endptr != '\0') {
		SPDK_ERRLOG("bw cir parse failed, cir string is %s\n", req->rw_bw_cfg.qos_str[SSAM_QOS_CIR]);
		return -1;
	}

	endptr = NULL;
	qos_cfg->cfg.bps_cbs = strtoul(req->rw_bw_cfg.qos_str[SSAM_QOS_CBS], &endptr, DECIMAL_BASE);
	if (endptr == NULL || *endptr != '\0') {
		SPDK_ERRLOG("bw cbs parse failed, cbs string is %s\n", req->rw_bw_cfg.qos_str[SSAM_QOS_CBS]);
		return -1;
	}

	if (req->rw_bw_cfg.qos_cfg_num > SINGLE_BUCKET_QOS_NUM) {
		endptr = NULL;
		qos_cfg->cfg.bps_pir = strtoul(req->rw_bw_cfg.qos_str[SSAM_QOS_PIR], &endptr, DECIMAL_BASE);
		if (endptr == NULL || *endptr != '\0') {
			SPDK_ERRLOG("bw pir parse failed, pir string is %s\n", req->rw_bw_cfg.qos_str[SSAM_QOS_PIR]);
			return -1;
		}

		endptr = NULL;
		qos_cfg->cfg.bps_pbs = strtoul(req->rw_bw_cfg.qos_str[SSAM_QOS_PBS], &endptr, DECIMAL_BASE);
		if (endptr == NULL || *endptr != '\0') {
			SPDK_ERRLOG("bw pbs parse failed, pbs string is %s\n", req->rw_bw_cfg.qos_str[SSAM_QOS_PBS]);
			return -1;
		}
	}

	return 0;
}

static int
_parse_iops_qos_info(struct rpc_ssam_qos_request *req, struct rpc_ssam_qos_type *qos_cfg)
{
	char *endptr = NULL;

	qos_cfg->cfg.pps_cir = strtoul(req->rw_iops_cfg.qos_str[SSAM_QOS_CIR], &endptr, DECIMAL_BASE);
	if (endptr == NULL || *endptr != '\0') {
		SPDK_ERRLOG("iops cir parse failed, cir string is %s\n", req->rw_iops_cfg.qos_str[SSAM_QOS_CIR]);
		return -1;
	}

	endptr = NULL;
	qos_cfg->cfg.pps_cbs = strtoul(req->rw_iops_cfg.qos_str[SSAM_QOS_CBS], &endptr, DECIMAL_BASE);
	if (endptr == NULL || *endptr != '\0') {
		SPDK_ERRLOG("iops cbs parse failed, cbs string is %s\n", req->rw_iops_cfg.qos_str[SSAM_QOS_CBS]);
		return -1;
	}

	if (req->rw_iops_cfg.qos_cfg_num > SINGLE_BUCKET_QOS_NUM) {
		endptr = NULL;
		qos_cfg->cfg.pps_pir = strtoul(req->rw_iops_cfg.qos_str[SSAM_QOS_PIR], &endptr, DECIMAL_BASE);
		if (endptr == NULL || *endptr != '\0') {
			SPDK_ERRLOG("iops pir parse failed, pir string is %s\n", req->rw_iops_cfg.qos_str[SSAM_QOS_PIR]);
			return -1;
		}

		endptr = NULL;
		qos_cfg->cfg.pps_pbs = strtoul(req->rw_iops_cfg.qos_str[SSAM_QOS_PBS], &endptr, DECIMAL_BASE);
		if (endptr == NULL || *endptr != '\0') {
			SPDK_ERRLOG("iops pbs parse failed, pbs string is %s\n", req->rw_iops_cfg.qos_str[SSAM_QOS_PBS]);
			return -1;
		}
	}

	return 0;
}

static int
_construct_qos_info(struct rpc_ssam_qos_request *req, struct rpc_ssam_qos_type *qos_cfg)
{
	int rc;
	size_t pps_cfg_num = req->rw_iops_cfg.qos_cfg_num;
	size_t bps_cfg_num = req->rw_bw_cfg.qos_cfg_num;
	size_t qos_cfg_num = qos_cfg->qos_cfg_num;
	uint16_t qos_type;

	if (pps_cfg_num == 0 && bps_cfg_num == 0) {
		SPDK_ERRLOG("iops and bw qos cfg info is null\n");
		return -1;
	}

	if (pps_cfg_num == 0) {
		qos_cfg->cfg.qos_type = SSAM_QOS_BPS;
	} else if (bps_cfg_num == 0) {
		qos_cfg->cfg.qos_type = SSAM_QOS_PPS;
	} else {
		qos_cfg->cfg.qos_type = SSAM_QOS_ALL;
	}

	qos_type = qos_cfg->cfg.qos_type;
	if (qos_type == SSAM_QOS_PPS || qos_type == SSAM_QOS_ALL) {
		if (pps_cfg_num != qos_cfg_num) {
			SPDK_ERRLOG(
				"iops qos cfg error, pps_cfg_num :%lu, need num :%lu\n", pps_cfg_num, qos_cfg_num);
			return -1;
		}

		rc = _parse_iops_qos_info(req, qos_cfg);
		if (rc != 0) {
			return -1;
		}
	}

	if (qos_type == SSAM_QOS_BPS || qos_type == SSAM_QOS_ALL) {
		if (bps_cfg_num != qos_cfg_num) {
			SPDK_ERRLOG(
				"bw qos cfg error, bps_cfg_num :%lu, need num :%lu\n", bps_cfg_num, qos_cfg_num);
			return -1;
		}

		rc = _parse_bw_qos_info(req, qos_cfg);
		if (rc != 0) {
			return -1;
		}
	}

	return 0;
}

/* To be compatible with both virtualization use case and bare metal use case,
 * a rough check is performed.
 */
static int
_check_function_id(uint32_t func_id)
{
	if (func_id > QOS_MAX_FUNCTION_ID)
		return -1;
	ssam_lock();
	struct spdk_ssam_session *smsession = ssam_session_find(func_id);
	ssam_unlock();
	if (smsession == NULL) {
		SPDK_ERRLOG("No controller for func_id: %u\n", func_id);
		return -1;
	}
	
	return 0;
}

static bool
_name_check(char *group_name)
{
	size_t len = strlen(group_name);
	int rc = 0;
	regmatch_t pmatch[1];
	if (len > MAX_GROUP_NAME_LEN) {
		SPDK_ERRLOG("Limit the group name len to %u\n", MAX_GROUP_NAME_LEN);
		return false;
	}
	rc = regexec(&regex, group_name, 1, pmatch, 0);
	if (!rc) {
		int pos = pmatch[0].rm_so;
		SPDK_ERRLOG("invalid parameter: %c\n", group_name[pos]);
		return false;
	}

	return true;
}

int
ssam_create_function_qos(struct rpc_ssam_function_qos *req,
	struct rpc_ssam_qos_type *function_qos_cfg)
{
	struct rpc_ssam_qos_request qos_cfg = {0};
	int rc = 0;

	if (req == NULL) {
		SPDK_ERRLOG("construct qos cfg failed, invalid req parameter.\n");
		return -1;
	}

	if (function_qos_cfg == NULL) {
		SPDK_ERRLOG("construct qos cfg failed, invalid qos cfg parameter.\n");
		return -1;
	}

	memcpy(&qos_cfg.rw_iops_cfg, &req->rw_iops_cfg, sizeof(qos_cfg.rw_iops_cfg));
	memcpy(&qos_cfg.rw_bw_cfg, &req->rw_bw_cfg, sizeof(qos_cfg.rw_bw_cfg));

	rc = _construct_qos_info(&qos_cfg, function_qos_cfg);
	if (rc != 0) {
		SPDK_ERRLOG("construct qos cfg failed\n");
		return -1;
	}

	rc = _check_qos_limit(function_qos_cfg);
	if (rc != 0) {
		SPDK_ERRLOG("qos info check failed\n");
		return -1;
	}

	rc = _check_function_id(req->func_id);
	if (rc != 0) {
		SPDK_ERRLOG("invalid function id.\n");
		return -1;
	}

	return ssam_qos_set_limit(SSAM_QOS_LEVEL_FUNCTION, req->func_id, &function_qos_cfg->cfg);
}

int
ssam_get_function_qos(uint32_t func_id, struct ssam_qos_cfg *qos_cfg)
{
	int rc = 0;

	rc = _check_function_id(func_id);
	if (rc < 0) {
		SPDK_ERRLOG("function id is invalid.\n");
		return -1;
	}

	/* return all qos configuration */
	qos_cfg->qos_type = SSAM_QOS_ALL;
	rc = ssam_qos_get_limit(SSAM_QOS_LEVEL_FUNCTION, func_id, qos_cfg);
	if (rc != 0) {
		SPDK_ERRLOG("get qos cfg failed\n");
		return -1;
	}
	qos_cfg->bps_cir /= MIBITS_TO_KIBITS;
	qos_cfg->bps_cbs /= MIBITS_TO_KIBITS;
	qos_cfg->bps_pir /= MIBITS_TO_KIBITS;
	qos_cfg->bps_pbs /= MIBITS_TO_KIBITS;

	return 0;
}

static int
_alloc_qos_group_id(char *name, uint32_t *group_id)
{
	uint32_t i;

	if (name == NULL) {
		SPDK_ERRLOG("ssam alloc qos group id failed, group name is NULL.\n");
		return -1;
	}

	if (!_name_check(name)) {
		SPDK_ERRLOG("invalid name parameters\n");
		return -1;
	}

	if (group_id == NULL) {
		SPDK_ERRLOG("ssam alloc qos group id failed, group id is NULL.\n");
		return -1;
	}

	if (g_group_qos_cnt == MAX_GROUP_QOS_NUM) {
		SPDK_ERRLOG("ssam alloc qos group id failed\n");
		SPDK_ERRLOG("the number of group QoS entries reaching the upper limit.\n");
		return -1;
	}

	for (i = MIN_GROUP_QOS_ID; i <= MAX_GROUP_QOS_ID; i++) {
		if (g_group_qos_name[i] == NULL)
			continue;
		if (strcmp(name, g_group_qos_name[i]) == 0) {
			SPDK_ERRLOG("group QoS name: %s already exists.\n",
				name);
			*group_id = i;
			return -1;
		}
	}

	for (i = MIN_GROUP_QOS_ID; i <= MAX_GROUP_QOS_ID; i++) {
		if (g_group_qos_name[i] == NULL) {
			g_group_qos_name[i] = strdup(name);
			*group_id = i;
			g_group_qos_cnt++;
			return 0;
		}
	}

	return -1;
}

static void
_free_group_qos_id(uint32_t group_id)
{
	if (g_group_qos_name[group_id] == NULL) {
		return;
	}

	free(g_group_qos_name[group_id]);
	g_group_qos_name[group_id] = NULL;
	g_group_qos_cnt--;

	return;
}

int
ssam_create_group_qos(struct rpc_ssam_group_qos *req,
	struct rpc_ssam_qos_type *group_qos_cfg)
{
	uint32_t group_id;
	int rc = 0;

	if (req == NULL) {
		SPDK_ERRLOG("construct qos cfg failed, invalid parameter.\n");
		return -1;
	}

	if (group_qos_cfg == NULL) {
		SPDK_ERRLOG("construct qos cfg failed, invalid parameter.\n");
		return -1;
	}

	rc = _alloc_qos_group_id(req->group_name, &group_id);
	if (rc != 0) {
		SPDK_ERRLOG("alloc group QoS id failed.\n");
		return -1;
	}

	group_qos_cfg->cfg.qos_type = SSAM_QOS_ALL;
	rc = ssam_qos_set_limit(SSAM_QOS_LEVEL_GROUP, group_id,
					&group_qos_cfg->cfg);
	if (rc != 0) {
		_free_group_qos_id(group_id);
	}
	return rc;
}


int
ssam_create_host_qos(struct rpc_ssam_qos_request *req,
	struct rpc_ssam_qos_type *host_qos_cfg)
{
	int rc = 0;

	if (req == NULL) {
		SPDK_ERRLOG("set host qos failed, invaild parameter.\n");
		return -1;
	}

	if (host_qos_cfg == NULL) {
		SPDK_ERRLOG("set host qos failed, invaild parmeter.\n");
		return -1;
	}

	rc = _construct_qos_info(req, host_qos_cfg);
	if (rc != 0) {
		SPDK_ERRLOG("construct qos cfg failed\n");
		return -1;
	}

	rc = _check_qos_limit(host_qos_cfg);
	if (rc != 0) {
		SPDK_ERRLOG("qos info check failed\n");
		return -1;
	}

	return ssam_qos_set_limit(SSAM_QOS_LEVEL_HOST, 0, &host_qos_cfg->cfg);
}

static int
_qos_group_name2id(char *name, uint32_t *group_id)
{
	uint32_t i;

	if (name == NULL) {
		SPDK_ERRLOG("ssam alloc qos group id failed, group name is NULL.\n");
		return -1;
	}

	if (!_name_check(name)) {
		SPDK_ERRLOG("invalid name parameters\n");
		return -1;
	}

	if (group_id == NULL) {
		SPDK_ERRLOG("ssam alloc qos group id failed, group id is NULL.\n");
		return -1;
	}

	for (i = MIN_GROUP_QOS_ID; i <= MAX_GROUP_QOS_ID; i++) {
		if (g_group_qos_name[i] == NULL)
			continue;
		if (strcmp(name, g_group_qos_name[i]) == 0) {
			*group_id = i;
			return 0;
		}
	}

	return -1;
}

int
ssam_delete_group_qos(char *name)
{
	struct ssam_qos_func_list list = {0};
	uint32_t group_id;
	int rc = 0;
	uint32_t i;

	if (name == NULL) {
		SPDK_ERRLOG("group name is NULL.\n");
		return -1;
	}

	rc = _qos_group_name2id(name, &group_id);
	if (rc != 0) {
		SPDK_ERRLOG("group name %s not found, please create a group with the name before using the group.\n",
			name);
		return -1;
	}

	rc = ssam_qos_get_func_of_group(group_id, &list);
	if (rc != 0) {
		SPDK_ERRLOG("ssam qos get func of group failed.\n");
		return -1;
	}

	for (i = 0; i < list.func_num; i++) {
		rc = ssam_qos_group_func_unmmap(group_id, list.func_list[i]);
		if (rc != 0) {
			SPDK_ERRLOG("ssam qos group %u unmmap function %u failed.\n",
				group_id, list.func_list[i]);
			return -1;
		}
	}
	struct ssam_qos_cfg cfg = {0};
	cfg.qos_type = SSAM_QOS_ALL;
	rc = ssam_qos_set_limit(SSAM_QOS_LEVEL_GROUP, group_id, &cfg);
	if (rc != 0) {
		SPDK_ERRLOG("ssam qos group %u set to unlimited failed.\n",
			group_id);
		return -1;
	}

	_free_group_qos_id(group_id);

	return 0;
}

int
ssam_qos_add_function_to_group(struct rpc_ssam_controller_group_qos_map *req)
{
	uint32_t group_id = 0;
	int rc = 0;
	struct ssam_qos_func_list list = {0};

	if (req == NULL) {
		SPDK_ERRLOG("add function to group failed, invalid parameter.\n");
		return -1;
	}

	if (req->group_name == NULL) {
		SPDK_ERRLOG("group name is NULL.\n");
		return -1;
	}

	if (ssam_qos_get_group_id(req->func_id, &group_id) == 0 && group_id != 0) {
		if (group_id <= MAX_GROUP_QOS_ID) {
			SPDK_ERRLOG("func id(%u) has been added to group id(%u) name(%s)\n",
			req->func_id, group_id, g_group_qos_name[group_id]);
		} else {
			SPDK_ERRLOG("func id(%u) has been added to group id(%u), but out of group id range\n",
			req->func_id, group_id);
		}
		return -1;
	}

	rc = _qos_group_name2id(req->group_name, &group_id);
	if (rc != 0) {
		SPDK_ERRLOG("group name not found, please create a group with the name before using the group.\n");
		return -1;
	}

	rc = ssam_qos_get_func_of_group(group_id, &list);
	if (rc != 0) {
		SPDK_ERRLOG("ssam qos get func of group failed.\n");
		return -1;
	}
	if (list.func_num >= QOS_MAX_FUNCTION_NUM_PER_GROUP) {
		SPDK_ERRLOG("each group is limited to %u func.\n", QOS_MAX_FUNCTION_NUM_PER_GROUP);
		return -1;
	}

	rc = _check_function_id(req->func_id);
	if (rc < 0) {
		SPDK_ERRLOG("function id is invalid.\n");
		return -1;
	}

	return ssam_qos_group_func_mmap(group_id, req->func_id);
}

int
ssam_qos_delete_function_from_group(struct rpc_ssam_controller_group_qos_map *req)
{
	uint32_t group_id;
	int rc = 0;

	if (req == NULL) {
		SPDK_ERRLOG("delete function from group failed, invalid parameter\n");
		return -1;
	}

	if (req->group_name == NULL) {
		SPDK_ERRLOG("group name is NULL.\n");
		return -1;
	}

	rc = _qos_group_name2id(req->group_name, &group_id);
	if (rc != 0) {
		SPDK_ERRLOG("group name not found, please create a group with the name before using the group.\n");
		return -1;
	}

	rc = _check_function_id(req->func_id);
	if (rc < 0) {
		SPDK_ERRLOG("function id is invalid.\n");
		return -1;
	}

	return ssam_qos_group_func_unmmap(group_id, req->func_id);
}

int
ssam_qos_get_function_of_group(char *group_name,
	struct ssam_qos_group_func *result)
{
	uint32_t group_id;
	int rc = 0;
	struct ssam_qos_func_list *func_info = &result->func_info;

	if (group_name == NULL) {
		SPDK_ERRLOG("group name is NULL.\n");
		return -1;
	}

	if (result == NULL) {
		SPDK_ERRLOG("invalid parameter.\n");
		return -1;
	}

	rc = _qos_group_name2id(group_name, &group_id);
	if (rc != 0) {
		SPDK_ERRLOG("group name: %s does not exist.\n", group_name);
		return -1;
	}
	result->group_id = group_id;

	return ssam_qos_get_func_of_group(group_id, func_info);
}

int
ssam_set_group_qos(struct rpc_ssam_group_qos *req,
	struct rpc_ssam_qos_type *group_qos_cfg)
{
	struct rpc_ssam_qos_request qos_cfg = {0};
	uint32_t group_id;
	int rc = 0;

	if (req == NULL) {
		SPDK_ERRLOG("set group qos failed, invalid parameter.\n");
		return -1;
	}

	if (group_qos_cfg == NULL) {
		SPDK_ERRLOG("set group qos failed, invalid parameter.\n");
		return -1;
	}

	rc = _qos_group_name2id(req->group_name, &group_id);
	if (rc != 0) {
		SPDK_ERRLOG("group name: %s not found.\n", req->group_name);
		return -1;
	}

	memcpy(&qos_cfg.rw_iops_cfg, &req->rw_iops_cfg,
		sizeof(qos_cfg.rw_iops_cfg));
	memcpy(&qos_cfg.rw_bw_cfg, &req->rw_bw_cfg, sizeof(qos_cfg.rw_bw_cfg));

	rc = _construct_qos_info(&qos_cfg, group_qos_cfg);
	if (rc != 0) {
		SPDK_ERRLOG("construct qos cfg failed\n");
		return -1;
	}

	rc = _check_qos_limit(group_qos_cfg);
	if (rc != 0) {
		SPDK_ERRLOG("qos info check failed\n");
		return -1;
	}

	return ssam_qos_set_limit(SSAM_QOS_LEVEL_GROUP, group_id,
					&group_qos_cfg->cfg);
}

int
ssam_get_group_qos(char *group_name, struct ssam_qos_cfg *qos_cfg)
{
	uint32_t group_id;
	int rc = 0;

	if (group_name == NULL) {
		SPDK_ERRLOG("group name is NULL.\n");
		return -1;
	}

	if (qos_cfg == NULL) {
		SPDK_ERRLOG("get group qos failed, invalid parameter.\n");
		return -1;
	}

	rc = _qos_group_name2id(group_name, &group_id);
	if (rc != 0) {
		SPDK_ERRLOG("group name: %s does not exist.\n",
			group_name);
		return -1;
	}

	/* return all qos configuration */
	qos_cfg->qos_type = SSAM_QOS_ALL;
	rc = ssam_qos_get_limit(SSAM_QOS_LEVEL_GROUP, group_id, qos_cfg);
	if (rc != 0) {
		SPDK_ERRLOG("get qos cfg failed\n");
		return -1;
	}
	qos_cfg->bps_cir /= MIBITS_TO_KIBITS;
	qos_cfg->bps_cbs /= MIBITS_TO_KIBITS;
	qos_cfg->bps_pir /= MIBITS_TO_KIBITS;
	qos_cfg->bps_pbs /= MIBITS_TO_KIBITS;

	return 0;
}

int
ssam_get_host_qos(struct ssam_qos_cfg *qos_cfg)
{
	int rc = 0;
	if (qos_cfg == NULL) {
		SPDK_ERRLOG("get host qos failed, invalid parameter.\n");
		return -1;
	}

	/* return all qos configuration */
	qos_cfg->qos_type = SSAM_QOS_ALL;
	rc = ssam_qos_get_limit(SSAM_QOS_LEVEL_HOST, 0, qos_cfg);
	if (rc != 0) {
		SPDK_ERRLOG("get qos cfg failed\n");
		return -1;
	}
	qos_cfg->bps_cir /= MIBITS_TO_KIBITS;
	qos_cfg->bps_cbs /= MIBITS_TO_KIBITS;
	qos_cfg->bps_pir /= MIBITS_TO_KIBITS;
	qos_cfg->bps_pbs /= MIBITS_TO_KIBITS;

	return 0;
}

int
ssam_clear_qos_cfg(void)
{
	return ssam_qos_clear();
}

void
ssam_init_qos(void)
{
	int rc = 0;
	rc = regcomp(&regex, pattern_special, 0);
	if (rc != 0) {
		SPDK_WARNLOG("Could not compile regex, rc = %d\n", rc);
	}

	if (spdk_ssam_get_hot_upgrade_status() != SSAM_HOT_UPGRADE_BEGIN) {
		SPDK_NOTICELOG("ssam start clear qos cfg.\n");
		rc = ssam_qos_clear();
		if (rc != 0) {
			SPDK_WARNLOG("clear qos cfg failed, rc = %d\n", rc);
		}
	}

	return;
}

SPDK_LOG_REGISTER_COMPONENT(ssam_qos)
