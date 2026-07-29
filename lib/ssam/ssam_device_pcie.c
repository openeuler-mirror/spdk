/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021-2025 Huawei Technologies Co.
 *   All rights reserved.
 */

#include "spdk/string.h"
#include "spdk/file.h"
#include "ssam_internal.h"

#define SSAM_KEY_MAX_LEN                16
#define SSAM_TYPE_MAX_LEN               12
#define SSAM_DBDF_MAX_LEN               16

struct ssam_device_pcie_info {
	uint32_t func_id;
	char type[SSAM_TYPE_MAX_LEN];
	char dbdf[SSAM_DBDF_MAX_LEN];
};

struct ssam_device_pcie_list {
	uint32_t size;
	struct ssam_device_pcie_info *device_pcie_list;
};

static struct ssam_device_pcie_list g_ssam_device_pcie_list = {
	.size = 0,
	.device_pcie_list = NULL,
};

void
ssam_deinit_device_pcie_list(void)
{
	if (g_ssam_device_pcie_list.device_pcie_list != NULL) {
		free(g_ssam_device_pcie_list.device_pcie_list);
		g_ssam_device_pcie_list.device_pcie_list = NULL;
	}
}

static int
ssam_alloc_device_pcie_list(struct spdk_json_val *values, size_t num_values)
{
	size_t i;
	uint32_t size = 0;

	for (i = 0; i < num_values; i++) {
		if (values[i].type == SPDK_JSON_VAL_OBJECT_END) {
			size++;
		}
	}

	if (g_ssam_device_pcie_list.device_pcie_list == NULL) {
		g_ssam_device_pcie_list.size = size;
		g_ssam_device_pcie_list.device_pcie_list = calloc(size, sizeof(struct ssam_device_pcie_info));
		if (g_ssam_device_pcie_list.device_pcie_list == NULL) {
			SPDK_ERRLOG("Unable to allocate enough memory for device_pcie_list\n");
			return -ENOMEM;
		}
	}
	return 0;
}

static void
ssam_set_device_pcie_index(struct spdk_json_val *value, uint32_t cur_index)
{
	char val[16];
	uint32_t gfunc_id;
	if (value->type != SPDK_JSON_VAL_NUMBER || value->len > 5) {
		SPDK_ERRLOG("device pcie gfunc id is invalid, type: %u, len: %u\n", value->type, value->len);
		return;
	}

	memset(val, 0, 16);
	memcpy(val, value->start, value->len);
	gfunc_id = spdk_strtol(val, 10);
	if (gfunc_id >= SPDK_INVALID_GFUNC_ID) {
		SPDK_ERRLOG("device pcie gfunc id(%u) is more than %u\n", gfunc_id, SPDK_INVALID_GFUNC_ID);
		return;
	}
	g_ssam_device_pcie_list.device_pcie_list[cur_index].func_id = gfunc_id;
}

static void
ssam_set_device_pcie_dbdf(struct spdk_json_val *value, uint32_t cur_index)
{
	if (value->type != SPDK_JSON_VAL_STRING || value->len >= SSAM_DBDF_MAX_LEN) {
		SPDK_ERRLOG("device pcie dbdf is invalid, type: %u, len: %u\n", value->type, value->len);
		return;
	}

	memset(g_ssam_device_pcie_list.device_pcie_list[cur_index].dbdf, 0, SSAM_DBDF_MAX_LEN);
	memcpy(g_ssam_device_pcie_list.device_pcie_list[cur_index].dbdf, value->start, value->len);
}

static void
ssam_set_device_pcie_type(struct spdk_json_val *value, uint32_t cur_index)
{
	if (value->type != SPDK_JSON_VAL_STRING || value->len >= SSAM_TYPE_MAX_LEN) {
		SPDK_ERRLOG("device pcie type is invalid, type: %u, len: %u\n", value->type, value->len);
		return;
	}

	memset(g_ssam_device_pcie_list.device_pcie_list[cur_index].type, 0, SSAM_TYPE_MAX_LEN);
	memcpy(g_ssam_device_pcie_list.device_pcie_list[cur_index].type, value->start, value->len);
}

static void
ssam_init_device_pcie_list_by_values(struct spdk_json_val *values, size_t num_values)
{
	char key[SSAM_KEY_MAX_LEN];
	uint32_t cur_index = 0;
	size_t i;

	for (i = 0; i < num_values; i++) {
		if (values[i].type == SPDK_JSON_VAL_OBJECT_END) {
			cur_index++;
		}
		if (values[i].type != SPDK_JSON_VAL_NAME || values[i].len >= SSAM_KEY_MAX_LEN) {
			continue;
		}

		memset(key, 0, SSAM_KEY_MAX_LEN);
		memcpy(key, values[i].start, values[i].len);

		/* point to val */
		i++;
		if (i >= num_values) {
            break;
        }

		if (strcmp(key, "index") == 0) {
			ssam_set_device_pcie_index(&values[i], cur_index);
		} else if (strcmp(key, "dbdf") == 0) {
			ssam_set_device_pcie_dbdf(&values[i], cur_index);
		} else if (strcmp(key, "type") == 0) {
			ssam_set_device_pcie_type(&values[i], cur_index);
		}
	}
}

int
ssam_init_device_pcie_list(void)
{
	FILE *fp = NULL;
	void *buf = NULL;
	ssize_t rc = 0;
	size_t size;
	size_t num_values;
	struct spdk_json_val *values = NULL;

	fp = popen("dpak-smi info -t device_pcie_list -f storage", "r");
	if (fp == NULL) {
		SPDK_ERRLOG("execute dpak-smi failed\n");
		return -EINVAL;
	}

	buf = spdk_posix_file_load(fp, &size);
	if (buf == NULL) {
		SPDK_ERRLOG("get size of json failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	rc = spdk_json_parse(buf, size, NULL, 0, NULL, SPDK_JSON_PARSE_FLAG_ALLOW_COMMENTS);
	if (rc < 0) {
		SPDK_ERRLOG("dpak-smi error: %s\n", (char *)buf);
		goto invalid;
	}
	num_values = (size_t)rc;
	values = calloc(num_values, sizeof(*values));
	if (values == NULL) {
		SPDK_ERRLOG("Unable to allocate enough memory for values\n");
		rc = -ENOMEM;
		goto invalid;
	}

	rc = spdk_json_parse(buf, size, values, num_values, NULL,
			     SPDK_JSON_PARSE_FLAG_ALLOW_COMMENTS | SPDK_JSON_PARSE_FLAG_DECODE_IN_PLACE);
	if (rc <= 0) {
		SPDK_ERRLOG("parse json to values failed\n");
		goto invalid;
	}

	rc = ssam_alloc_device_pcie_list(values, num_values);
	if (rc != 0) {
		goto invalid;
	}

	ssam_init_device_pcie_list_by_values(values, num_values);
	rc = 0;

invalid:
	if (values != NULL) {
		free(values);
		values = NULL;
	}
	if (buf != NULL) {
		free(buf);
		buf = NULL;
	}
	if (fp != NULL) {
		pclose(fp);
		fp = NULL;
	}
	return rc;
}

void
ssam_dump_device_pcie_list(struct spdk_json_write_ctx *w)
{
	uint32_t i;
	spdk_json_write_named_array_begin(w, "device_pcie_list");
	for (i = 0; i < g_ssam_device_pcie_list.size; i++) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_uint32(w, "index", g_ssam_device_pcie_list.device_pcie_list[i].func_id);
		spdk_json_write_named_string(w, "dbdf", g_ssam_device_pcie_list.device_pcie_list[i].dbdf);
		spdk_json_write_named_string(w, "type", g_ssam_device_pcie_list.device_pcie_list[i].type);
		spdk_json_write_object_end(w);
	}
	spdk_json_write_array_end(w);
}

uint32_t
ssam_get_device_pcie_list_size(void)
{
	return g_ssam_device_pcie_list.size;
}
