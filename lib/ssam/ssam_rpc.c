/*
 *   BSD LICENSE
 *   Copyright (c) Huawei Technologies Co., Ltd. 2021-2021. All rights reserved.
 *
 */

#include "spdk/string.h"
#include "spdk/env.h"
#include "spdk/bdev_module.h"
#include "spdk/ssam.h"
#include "spdk/bdev.h"

#include "ssam_internal.h"
#include "ssam_config.h"
#include "rte_malloc.h"

#include "ssam_fs_internal.h"

static int ssam_rpc_get_gfunc_id_by_dbdf(char *dbdf, uint16_t *gfunc_id);

struct rpc_ssam_blk_ctrlr {
	char *dev_name;
	char *index;
	bool readonly;
	char *serial;
};

static const struct spdk_json_object_decoder g_rpc_construct_ssam_blk_ctrlr[] = {
	{"dev_name", offsetof(struct rpc_ssam_blk_ctrlr, dev_name), spdk_json_decode_string},
	{"index", offsetof(struct rpc_ssam_blk_ctrlr, index), spdk_json_decode_string},
	{"readonly", offsetof(struct rpc_ssam_blk_ctrlr, readonly), spdk_json_decode_bool, true},
	{"serial", offsetof(struct rpc_ssam_blk_ctrlr, serial), spdk_json_decode_string, true},
};

static void
free_rpc_ssam_blk_ctrlr(struct rpc_ssam_blk_ctrlr *req)
{
	if (req->dev_name != NULL) {
		free(req->dev_name);
		req->dev_name = NULL;
	}

	if (req->index != NULL) {
		free(req->index);
		req->index = NULL;
	}

	if (req->serial != NULL) {
		free(req->serial);
		req->serial = NULL;
	}
}

static int
ssam_rpc_para_check(uint16_t gfunc_id)
{
	int rc;

	rc = ssam_check_gfunc_id(gfunc_id);
	if (rc != 0) {
		return rc;
	}

	return 0;
}

static int
ssam_rpc_para_check_type(uint16_t gfunc_id, enum ssam_device_type target_type)
{
	int rc;
	enum ssam_device_type type;

	rc = ssam_rpc_para_check(gfunc_id);
	if (rc != 0) {
		return rc;
	}

	type = ssam_get_virtio_type(gfunc_id);
	if (type == target_type) {
		return 0;
	}
	SPDK_ERRLOG("Invalid virtio type, need type %d, actually %d\n", target_type, type);

	return -EINVAL;
}

static void
rpc_ssam_send_response_cb(void *arg, int rsp)
{
	struct spdk_jsonrpc_request *request = arg;

	if (rsp != 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 spdk_strerror(-rsp));
	} else {
		spdk_jsonrpc_send_bool_response(request, true);
	}
	return;
}

struct ssam_log_command_info {
	char *user_name;
	char *event;
	char *src_addr;
};

static const struct spdk_json_object_decoder g_rpc_construct_log_command_info[] = {
	{"user_name", offsetof(struct ssam_log_command_info, user_name), spdk_json_decode_string},
	{"event", offsetof(struct ssam_log_command_info, event), spdk_json_decode_string},
	{"src_addr", offsetof(struct ssam_log_command_info, src_addr), spdk_json_decode_string},
};

static void
free_rpc_ssam_log_command_info(struct ssam_log_command_info *req)
{
	if (req->user_name != NULL) {
		free(req->user_name);
		req->user_name = NULL;
	}
	if (req->event != NULL) {
		free(req->event);
		req->event = NULL;
	}
	if (req->src_addr != NULL) {
		free(req->src_addr);
		req->src_addr = NULL;
	}
}

static void
rpc_ssam_log_command_info(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct ssam_log_command_info req = {0};
	int rc;

	if (params == NULL) {
		SPDK_ERRLOG("log info params error, skip\n");
		goto invalid;
	}

	rc = spdk_json_decode_object(params, g_rpc_construct_log_command_info,
				     SPDK_COUNTOF(g_rpc_construct_log_command_info), &req);
	if (rc != 0) {
		SPDK_ERRLOG("decode cmd info failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	SPDK_NOTICELOG("log event: from %s user %s event %s\n", req.src_addr, req.user_name, req.event);

invalid:
	free_rpc_ssam_log_command_info(&req);
	spdk_jsonrpc_send_bool_response(request, true);
	return;
}
SPDK_RPC_REGISTER("log_command_info", rpc_ssam_log_command_info,
		  SPDK_RPC_RUNTIME)

static int
rpc_ssam_session_reg_response_cb(struct spdk_ssam_session *smsession,
				 struct spdk_jsonrpc_request *request)
{
	if (smsession->rsp_fn != NULL) {
		return -1;
	}
	smsession->rsp_fn = rpc_ssam_send_response_cb;
	smsession->rsp_ctx = request;
	return 0;
}

static void
rpc_init_session_reg_info(struct spdk_ssam_session_reg_info *info,
			  uint16_t queues, uint16_t gfunc_id, struct spdk_jsonrpc_request *request)
{
	info->queues = queues;
	info->gfunc_id = gfunc_id;
	info->rsp_ctx = (void *)request;
	info->rsp_fn = rpc_ssam_send_response_cb;
}

static void
free_rpc_ssam_session_reg_info(struct spdk_ssam_session_reg_info *info)
{
	if (info->name != NULL) {
		free(info->name);
		info->name = NULL;
	}
	if (info->dbdf != NULL) {
		free(info->dbdf);
		info->dbdf = NULL;
	}
}

static uint16_t
rpc_ssam_get_gfunc_id_by_index(char *index)
{
	uint16_t gfunc_id, i;
	int rc;
	if (strlen(index) <= 0x5) {
		for (i = 0; i < strlen(index); i++) {
			if (!isdigit(index[i])) {
				return SPDK_INVALID_GFUNC_ID;
			}
		}
		gfunc_id = spdk_strtol(index, 10) > SPDK_INVALID_GFUNC_ID ? SPDK_INVALID_GFUNC_ID : spdk_strtol(
				   index, 10);
	} else {
		rc = ssam_rpc_get_gfunc_id_by_dbdf(index, &gfunc_id);
		if (rc != 0) {
			return SPDK_INVALID_GFUNC_ID;
		}
	}
	return gfunc_id;
}

static void
rpc_ssam_create_blk_controller(struct spdk_jsonrpc_request *request,
			       const struct spdk_json_val *params)
{
	struct spdk_ssam_session_reg_info info = {0};
	struct rpc_ssam_blk_ctrlr req = {0};
	uint16_t gfunc_id = SPDK_INVALID_GFUNC_ID;
	uint16_t queues;
	int rc;

	if (params == NULL) {
		SPDK_ERRLOG("rpc_ssam_create_blk_controller params null\n");
		rc = -EINVAL;
		goto invalid;
	}

	rc = spdk_json_decode_object(params, g_rpc_construct_ssam_blk_ctrlr,
				     SPDK_COUNTOF(g_rpc_construct_ssam_blk_ctrlr), &req);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	gfunc_id = rpc_ssam_get_gfunc_id_by_index(req.index);
	rc = ssam_rpc_para_check_type(gfunc_id, SSAM_DEVICE_VIRTIO_BLK);
	if (rc != 0) {
		goto invalid;
	}

	if (req.dev_name == NULL) {
		rc = -ENODEV;
		goto invalid;
	}

	queues = ssam_get_queues();
	if (queues > SPDK_SSAM_MAX_VQUEUES) {
		SPDK_ERRLOG("Queue number out of range, need less or equal than %u, actually %u.\n",
			    SPDK_SSAM_MAX_VQUEUES, queues);
		rc = -EINVAL;
		goto invalid;
	}

	rpc_init_session_reg_info(&info, queues, gfunc_id, request);

	rc = ssam_blk_construct(&info, req.dev_name, req.readonly, req.serial);
	if (rc < 0) {
		goto invalid;
	}

	free_rpc_ssam_blk_ctrlr(&req);
	free_rpc_ssam_session_reg_info(&info);
	return;

invalid:
	free_rpc_ssam_blk_ctrlr(&req);
	free_rpc_ssam_session_reg_info(&info);
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));
	return;
}
SPDK_RPC_REGISTER("create_blk_controller", rpc_ssam_create_blk_controller,
		  SPDK_RPC_RUNTIME)

struct rpc_delete_ssam_ctrlr {
	char *index;
};

static const struct spdk_json_object_decoder g_rpc_delete_ssam_ctrlr_decoder[] = {
	{"index", offsetof(struct rpc_delete_ssam_ctrlr, index), spdk_json_decode_string},
};

static void
free_rpc_delete_ssam_ctrlr(struct rpc_delete_ssam_ctrlr *req)
{
	if (req->index != NULL) {
		free(req->index);
		req->index = NULL;
	}
}

static void
rpc_ssam_delete_controller(struct spdk_jsonrpc_request *request,
			   const struct spdk_json_val *params)
{
	struct rpc_delete_ssam_ctrlr req = {0};
	uint16_t gfunc_id = SPDK_INVALID_GFUNC_ID;
	struct spdk_ssam_session *smsession;
	int rc;
	enum ssam_device_type type;

	if (params == NULL) {
		SPDK_ERRLOG("rpc_ssam_delete_controller params null\n");
		rc = -EINVAL;
		goto invalid;
	}

	rc = spdk_json_decode_object(params, g_rpc_delete_ssam_ctrlr_decoder,
				     SPDK_COUNTOF(g_rpc_delete_ssam_ctrlr_decoder), &req);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	gfunc_id = rpc_ssam_get_gfunc_id_by_index(req.index);
	rc = ssam_rpc_para_check(gfunc_id);
	if (rc != 0) {
		goto invalid;
	}

	type = ssam_get_virtio_type(gfunc_id);
	if (type == SSAM_DEVICE_VIRTIO_FS) {
		SPDK_ERRLOG("should use fs_controller_delte to delete fs_controller\n");
		goto invalid;
	}

	ssam_lock();
	smsession = ssam_session_find(gfunc_id);
	if (smsession == NULL) {
		SPDK_ERRLOG("Couldn't find session with function id %d.\n", gfunc_id);
		rc = -ENODEV;
		ssam_unlock();
		goto invalid;
	}

	rc = rpc_ssam_session_reg_response_cb(smsession, request);
	if (rc != 0) {
		SPDK_ERRLOG("The controller is being operated.\n");
		rc = -EALREADY;
		ssam_unlock();
		goto invalid;
	}

	rc = ssam_session_unregister(smsession);
	if (rc != 0) {
		/*
		 * Unregitster response cb to avoid use request in the cb function,
		 * because if error happend, request will be responsed immediately
		 */
		ssam_session_unreg_response_cb(smsession);
		ssam_unlock();
		goto invalid;
	}
	ssam_unlock();

	free_rpc_delete_ssam_ctrlr(&req);
	return;

invalid:
	free_rpc_delete_ssam_ctrlr(&req);
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));
	return;
}
SPDK_RPC_REGISTER("delete_controller", rpc_ssam_delete_controller, SPDK_RPC_RUNTIME)

struct rpc_delete_ssam_scsi_ctrlr {
	char *name;
};

static const struct spdk_json_object_decoder g_rpc_delete_ssam_scsi_ctrlr_decoder[] = {
	{"name", offsetof(struct rpc_delete_ssam_scsi_ctrlr, name), spdk_json_decode_string},
};

static void
free_rpc_delete_ssam_scsi_ctrlrs(struct rpc_delete_ssam_scsi_ctrlr *req)
{
	if (req->name != NULL) {
		free(req->name);
		req->name = NULL;
	}
}

static void
rpc_ssam_delete_scsi_controller(struct spdk_jsonrpc_request *request,
				const struct spdk_json_val *params)
{
	struct rpc_delete_ssam_scsi_ctrlr req = {0};
	uint16_t gfunc_id = SPDK_INVALID_GFUNC_ID;
	struct spdk_ssam_session *smsession;
	int rc;

	if (params == NULL) {
		SPDK_ERRLOG("rpc_ssam_delete_controller params null\n");
		rc = -EINVAL;
		goto invalid;
	}

	rc = spdk_json_decode_object(params, g_rpc_delete_ssam_scsi_ctrlr_decoder,
				     SPDK_COUNTOF(g_rpc_delete_ssam_scsi_ctrlr_decoder), &req);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	gfunc_id = ssam_get_gfunc_id_by_name(req.name);
	rc = ssam_rpc_para_check_type(gfunc_id, SSAM_DEVICE_VIRTIO_SCSI);
	if (rc != 0) {
		goto invalid;
	}

	ssam_lock();
	smsession = ssam_session_find(gfunc_id);
	if (smsession == NULL) {
		SPDK_ERRLOG("Couldn't find session with function id %d.\n", gfunc_id);
		rc = -ENODEV;
		ssam_unlock();
		goto invalid;
	}

	rc = rpc_ssam_session_reg_response_cb(smsession, request);
	if (rc != 0) {
		SPDK_ERRLOG("The controller is being operated.\n");
		rc = -EALREADY;
		ssam_unlock();
		goto invalid;
	}

	rc = ssam_session_unregister(smsession);
	if (rc != 0) {
		/*
		 * Unregitster response cb to avoid use request in the cb function,
		 * because if error happend, request will be responsed immediately
		 */
		ssam_session_unreg_response_cb(smsession);
		ssam_unlock();
		goto invalid;
	}
	ssam_unlock();

	free_rpc_delete_ssam_scsi_ctrlrs(&req);
	return;

invalid:
	free_rpc_delete_ssam_scsi_ctrlrs(&req);
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));
	return;
}
SPDK_RPC_REGISTER("delete_scsi_controller", rpc_ssam_delete_scsi_controller, SPDK_RPC_RUNTIME)

struct rpc_get_ssam_ctrlrs {
	uint32_t function_id;
	char *dbdf;
};

static const struct spdk_json_object_decoder g_rpc_get_ssam_ctrlrs_decoder[] = {
	{"function_id", offsetof(struct rpc_get_ssam_ctrlrs, function_id), spdk_json_decode_uint32, true},
	{"dbdf", offsetof(struct rpc_get_ssam_ctrlrs, dbdf), spdk_json_decode_string, true},
};

static void
free_rpc_get_ssam_ctrlrs(struct rpc_get_ssam_ctrlrs *req)
{
	if (req->dbdf != NULL) {
		free(req->dbdf);
		req->dbdf = NULL;
	}
}

static void
_rpc_get_ssam_controller(struct spdk_json_write_ctx *w,
			 struct spdk_ssam_dev *smdev, uint16_t gfunc_id)
{
	ssam_dump_info_json(smdev, gfunc_id, w);
}

static int
rpc_ssam_show_controllers(struct spdk_jsonrpc_request *request, uint16_t gfunc_id)
{
	struct spdk_ssam_dev *smdev = NULL;
	struct spdk_json_write_ctx *w = NULL;
	struct spdk_ssam_session *smsession = NULL;

	ssam_lock();
	if (gfunc_id != SPDK_INVALID_GFUNC_ID) {
		smsession = ssam_session_find(gfunc_id);
		if (smsession == NULL) {
			ssam_unlock();
			return -ENODEV;
		}

		w = spdk_jsonrpc_begin_result(request);
		spdk_json_write_array_begin(w);
		smdev = ssam_dev_next(NULL);
		while (smdev != NULL) {
			_rpc_get_ssam_controller(w, smdev, gfunc_id);
			smdev = ssam_dev_next(smdev);
		}
		ssam_unlock();
		spdk_json_write_array_end(w);
		spdk_jsonrpc_end_result(request, w);
		return 0;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);

	smdev = ssam_dev_next(NULL);
	while (smdev != NULL) {
		_rpc_get_ssam_controller(w, smdev, gfunc_id);
		smdev = ssam_dev_next(smdev);
	}
	ssam_unlock();
	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);

	return 0;
}

static int
rpc_ssam_show_scsi_controllers(struct spdk_jsonrpc_request *request, uint16_t gfunc_id)
{
	struct spdk_ssam_dev *smdev = NULL;
	struct spdk_json_write_ctx *w = NULL;
	struct spdk_ssam_session *smsession = NULL;

	ssam_lock();
	if (gfunc_id != SPDK_INVALID_GFUNC_ID) {
		smsession = ssam_session_find(gfunc_id);
		if (smsession == NULL) {
			ssam_unlock();
			return -ENODEV;
		} else if (smsession->backend->type != VIRTIO_TYPE_SCSI) {
			ssam_unlock();
			return -EINVAL;
		}

		smdev = smsession->smdev;

		w = spdk_jsonrpc_begin_result(request);
		spdk_json_write_array_begin(w);

		smsession = smdev->smsessions[gfunc_id];
		smsession->backend->dump_info_json(smsession, w);
		ssam_unlock();

		spdk_json_write_array_end(w);
		spdk_jsonrpc_end_result(request, w);
		return 0;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);

	smdev = ssam_dev_next(NULL);
	while (smdev != NULL) {
		smsession = ssam_sessions_next(smdev->smsessions, NULL);
		while (smsession != NULL) {
			if (smsession->backend->type == VIRTIO_TYPE_SCSI) {
				smsession->backend->dump_info_json(smsession, w);
			}
			smsession = ssam_sessions_next(smdev->smsessions, smsession);
		}
		smdev = ssam_dev_next(smdev);
	}
	ssam_unlock();
	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);

	return 0;
}

static int
rpc_ssam_show_fs_controllers(struct spdk_jsonrpc_request *request, uint16_t gfunc_id)
{
	struct spdk_ssam_dev *smdev = NULL;
	struct spdk_json_write_ctx *w = NULL;
	struct spdk_ssam_session *smsession = NULL;

	ssam_lock();
	if (gfunc_id != SPDK_INVALID_GFUNC_ID) {
		smsession = ssam_session_find(gfunc_id);
		if (smsession == NULL) {
			ssam_unlock();
			return -ENODEV;
		}

		w = spdk_jsonrpc_begin_result(request);
		spdk_json_write_array_begin(w);

		smdev = ssam_dev_next(NULL);
		while (smdev != NULL) {
			bool is_smsession_exit = 0;
			smsession = ssam_sessions_next(smdev->smsessions, NULL);
			while (smsession != NULL) {
				if (smsession->backend->type == VIRTIO_TYPE_FS && smsession->gfunc_id == gfunc_id) {
					if (is_smsession_exit == 0) {
						is_smsession_exit = 1;
						spdk_json_write_object_begin(w);
						spdk_json_write_named_string(w, "ctrlr", ssam_dev_get_name(smdev));
						spdk_json_write_named_string_fmt(w, "cpumask", "0x%s",
										 spdk_cpuset_fmt(spdk_thread_get_cpumask(smdev->thread)));
						spdk_json_write_named_uint32(w, "session_num", (uint32_t)smdev->active_session_num);
						spdk_json_write_named_object_begin(w, "backend_specific");
						spdk_json_write_named_array_begin(w, "session");
					}
					smsession->backend->dump_info_json(smsession, w);
				}
				smsession = ssam_sessions_next(smdev->smsessions, smsession);
			}
			if (is_smsession_exit == 0) {
				smdev = ssam_dev_next(smdev);
				continue;
			}
			if (is_smsession_exit == 1) {
				spdk_json_write_array_end(w);
				spdk_json_write_object_end(w);
				spdk_json_write_object_end(w);
			}
			smdev = ssam_dev_next(smdev);
		}

		ssam_unlock();
		spdk_json_write_array_end(w);
		spdk_jsonrpc_end_result(request, w);
		return 0;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);

	smdev = ssam_dev_next(NULL);
	while (smdev != NULL) {
		bool is_smsession_exit = 0;
		smsession = ssam_sessions_next(smdev->smsessions, NULL);
		while (smsession != NULL) {
			if (smsession->backend->type == VIRTIO_TYPE_FS) {
				is_smsession_exit = 1;
				break;
			}
			smsession = ssam_sessions_next(smdev->smsessions, smsession);
		}
		if (is_smsession_exit == 0) {
			smdev = ssam_dev_next(smdev);
			continue;
		}
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "ctrlr", ssam_dev_get_name(smdev));
		spdk_json_write_named_string_fmt(w, "cpumask", "0x%s",
						 spdk_cpuset_fmt(spdk_thread_get_cpumask(smdev->thread)));
		spdk_json_write_named_uint32(w, "session_num", (uint32_t)smdev->active_session_num);
		spdk_json_write_named_object_begin(w, "backend_specific");
		spdk_json_write_named_array_begin(w, "session");
		smsession = ssam_sessions_next(smdev->smsessions, NULL);
		while (smsession != NULL) {
			if (smsession->backend->type == VIRTIO_TYPE_FS) {
				smsession->backend->dump_info_json(smsession, w);
			}
			smsession = ssam_sessions_next(smdev->smsessions, smsession);
		}
		spdk_json_write_array_end(w);
		spdk_json_write_object_end(w);
		spdk_json_write_object_end(w);
		smdev = ssam_dev_next(smdev);
	}
	ssam_unlock();
	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);

	return 0;
}

static void
rpc_ssam_get_controllers(struct spdk_jsonrpc_request *request,
			 const struct spdk_json_val *params)
{
	struct rpc_get_ssam_ctrlrs req = {
		.function_id = SPDK_INVALID_GFUNC_ID,
		.dbdf = NULL,
	};
	uint16_t gfunc_id = SPDK_INVALID_GFUNC_ID;
	int rc;

	if (params != NULL) {
		rc = spdk_json_decode_object(params, g_rpc_get_ssam_ctrlrs_decoder,
					     SPDK_COUNTOF(g_rpc_get_ssam_ctrlrs_decoder), &req);
		if (rc != 0) {
			SPDK_ERRLOG("spdk_json_decode_object failed\n");
			rc = -EINVAL;
			goto invalid;
		}
	}

	if (req.function_id != SPDK_INVALID_GFUNC_ID && req.dbdf != NULL) {
		SPDK_ERRLOG("get_controllers can have at most one parameter\n");
		rc = -EINVAL;
		goto invalid;
	}

	if (req.function_id != SPDK_INVALID_GFUNC_ID) {
		gfunc_id = req.function_id;
		rc = ssam_rpc_para_check(gfunc_id);
		if (rc != 0) {
			goto invalid;
		}
	}

	if (req.dbdf != NULL) {
		rc = ssam_rpc_get_gfunc_id_by_dbdf(req.dbdf, &gfunc_id);
		if (rc != 0) {
			goto invalid;
		}
		rc = ssam_rpc_para_check(gfunc_id);
		if (rc != 0) {
			goto invalid;
		}
	}

	rc = rpc_ssam_show_controllers(request, gfunc_id);
	if (rc != 0) {
		goto invalid;
	}

	free_rpc_get_ssam_ctrlrs(&req);
	return;

invalid:
	free_rpc_get_ssam_ctrlrs(&req);
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
					 spdk_strerror(-rc));
	return;
}
SPDK_RPC_REGISTER("get_controllers", rpc_ssam_get_controllers, SPDK_RPC_RUNTIME)

struct rpc_get_ssam_scsi_ctrlrs {
	char *name;
};

static const struct spdk_json_object_decoder g_rpc_get_ssam_scsi_ctrlrs_decoder[] = {
	{"name", offsetof(struct rpc_get_ssam_scsi_ctrlrs, name), spdk_json_decode_string, true},
};

static void
free_rpc_ssam_ctrlrs(struct rpc_get_ssam_scsi_ctrlrs *req)
{
	if (req->name != NULL) {
		free(req->name);
		req->name = NULL;
	}
}

static void
rpc_ssam_get_scsi_controllers(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params)
{
	struct rpc_get_ssam_scsi_ctrlrs req = {
		.name = NULL,
	};
	uint16_t gfunc_id = SPDK_INVALID_GFUNC_ID;
	int rc;

	if (params != NULL) {
		rc = spdk_json_decode_object(params, g_rpc_get_ssam_scsi_ctrlrs_decoder,
					     SPDK_COUNTOF(g_rpc_get_ssam_scsi_ctrlrs_decoder), &req);
		if (rc != 0) {
			SPDK_ERRLOG("spdk_json_decode_object failed\n");
			rc = -EINVAL;
			goto invalid;
		}
	}

	if (req.name != NULL) {
		gfunc_id = ssam_get_gfunc_id_by_name(req.name);
		rc = ssam_rpc_para_check(gfunc_id);
		if (rc != 0) {
			goto invalid;
		}
	}

	rc = rpc_ssam_show_scsi_controllers(request, gfunc_id);
	if (rc != 0) {
		goto invalid;
	}

	free_rpc_ssam_ctrlrs(&req);
	return;

invalid:
	free_rpc_ssam_ctrlrs(&req);
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
					 spdk_strerror(-rc));
	return;
}
SPDK_RPC_REGISTER("get_scsi_controllers", rpc_ssam_get_scsi_controllers, SPDK_RPC_RUNTIME)

struct rpc_ssam_controller_get_iostat {
	uint32_t function_id;
	char *dbdf;
};

static const struct spdk_json_object_decoder g_rpc_ssam_controller_get_iostat_decoder[] = {
	{"function_id", offsetof(struct rpc_ssam_controller_get_iostat, function_id), spdk_json_decode_uint32, true},
	{"dbdf", offsetof(struct rpc_ssam_controller_get_iostat, dbdf), spdk_json_decode_string, true},
};

static void
free_rpc_ssam_controller_get_iostat(struct rpc_ssam_controller_get_iostat *req)
{
	if (req->dbdf != NULL) {
		free(req->dbdf);
		req->dbdf = NULL;
	}
}

static int
rpc_ssam_show_iostat(struct spdk_jsonrpc_request *request, uint16_t gfunc_id)
{
	struct spdk_ssam_dev *smdev = NULL;
	struct spdk_json_write_ctx *w = NULL;
	struct spdk_ssam_session *smsession = NULL;

	ssam_lock();
	if (gfunc_id != SPDK_INVALID_GFUNC_ID) {
		smsession = ssam_session_find(gfunc_id);
		if (smsession == NULL) {
			ssam_unlock();
			return -ENODEV;
		}

		w = spdk_jsonrpc_begin_result(request);
		spdk_json_write_object_begin(w);
		spdk_json_write_named_uint64(w, "tick_rate", spdk_get_ticks_hz());
		spdk_json_write_named_array_begin(w, "dbdfs");

		if (smsession->backend->show_iostat_json != NULL) {
			smsession->backend->show_iostat_json(smsession, SPDK_SSAM_SCSI_CTRLR_MAX_DEVS, w);
		}

		ssam_unlock();

		spdk_json_write_array_end(w);
		spdk_json_write_object_end(w);
		spdk_jsonrpc_end_result(request, w);
		return 0;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_object_begin(w);
	spdk_json_write_named_uint64(w, "tick_rate", spdk_get_ticks_hz());
	spdk_json_write_named_array_begin(w, "dbdfs");

	smdev = ssam_dev_next(NULL);
	while (smdev != NULL) {
		smsession = ssam_sessions_next(smdev->smsessions, NULL);
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "name", smdev->name);
		spdk_json_write_named_uint64(w, "flight_io", smdev->io_num);
		spdk_json_write_named_uint64(w, "discard_io_num", smdev->discard_io_num);
		spdk_json_write_named_uint64(w, "wait_io", smdev->io_wait_cnt);
		spdk_json_write_named_uint64(w, "wait_io_r", smdev->io_wait_r_cnt);
		spdk_json_write_object_end(w);
		while (smsession != NULL) {
			if (smsession->backend->show_iostat_json != NULL) {
				smsession->backend->show_iostat_json(smsession, SPDK_SSAM_SCSI_CTRLR_MAX_DEVS, w);
			}
			smsession = ssam_sessions_next(smdev->smsessions, smsession);
		}
		smdev = ssam_dev_next(smdev);
	}

	ssam_unlock();
	spdk_json_write_array_end(w);
	spdk_json_write_object_end(w);
	spdk_jsonrpc_end_result(request, w);
	return 0;
}

static void
rpc_ssam_controller_get_iostat(struct spdk_jsonrpc_request *request,
			       const struct spdk_json_val *params)
{
	struct rpc_ssam_controller_get_iostat req = {
		.function_id = SPDK_INVALID_GFUNC_ID,
		.dbdf = NULL,
	};
	uint16_t gfunc_id = SPDK_INVALID_GFUNC_ID;
	int rc;

	if (params != NULL) {
		rc = spdk_json_decode_object(params, g_rpc_ssam_controller_get_iostat_decoder,
					     SPDK_COUNTOF(g_rpc_ssam_controller_get_iostat_decoder), &req);
		if (rc != 0) {
			SPDK_ERRLOG("spdk_json_decode_object failed\n");
			rc = -EINVAL;
			goto invalid;
		}
	}

	if (req.function_id != SPDK_INVALID_GFUNC_ID && req.dbdf != NULL) {
		SPDK_ERRLOG("controller_get_iostat can have at most one parameter\n");
		rc = -EINVAL;
		goto invalid;
	}

	if (req.function_id != SPDK_INVALID_GFUNC_ID) {
		gfunc_id = req.function_id;
		rc = ssam_rpc_para_check(gfunc_id);
		if (rc != 0) {
			goto invalid;
		}
	}

	if (req.dbdf != NULL) {
		rc = ssam_rpc_get_gfunc_id_by_dbdf(req.dbdf, &gfunc_id);
		if (rc != 0) {
			goto invalid;
		}
		rc = ssam_rpc_para_check(gfunc_id);
		if (rc != 0) {
			goto invalid;
		}
	}

	rc = rpc_ssam_show_iostat(request, gfunc_id);
	if (rc != 0) {
		goto invalid;
	}

	free_rpc_ssam_controller_get_iostat(&req);
	return;

invalid:
	free_rpc_ssam_controller_get_iostat(&req);
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
					 spdk_strerror(-rc));
	return;
}
SPDK_RPC_REGISTER("controller_get_iostat", rpc_ssam_controller_get_iostat, SPDK_RPC_RUNTIME)

struct rpc_ssam_clear_iostat {
	char *type;
};

static const struct spdk_json_object_decoder g_rpc_ssam_clear_iostat_decoder[] = {
	{"type", offsetof(struct rpc_ssam_clear_iostat, type), spdk_json_decode_string, true},
};

static void
free_rpc_ssam_clear_iostat(struct rpc_ssam_clear_iostat *req)
{
	if (req->type != NULL) {
		free(req->type);
		req->type = NULL;
	}
}

static void
rpc_ssam_clear_iostat(int typenum)
{
	struct spdk_ssam_dev *smdev = NULL;
	struct spdk_ssam_session *smsession = NULL;

	ssam_lock();
	smdev = ssam_dev_next(NULL);
	while (smdev != NULL) {
		smsession = ssam_sessions_next(smdev->smsessions, NULL);
		while (smsession != NULL) {
			if (typenum == VIRTIO_TYPE_UNKNOWN) {
				if (smsession->backend->clear_iostat_json != NULL) {
					smsession->backend->clear_iostat_json(smsession);
				}
			} else {
				if (smsession->backend->clear_iostat_json != NULL && (int)smsession->backend->type == typenum) {
					smsession->backend->clear_iostat_json(smsession);
				}
			}

			smsession = ssam_sessions_next(smdev->smsessions, smsession);
		}
		smdev = ssam_dev_next(smdev);
	}
	ssam_unlock();
}

static void
rpc_ssam_controller_clear_iostat(struct spdk_jsonrpc_request *request,
				 const struct spdk_json_val *params)
{
	struct rpc_ssam_clear_iostat req = {
		.type = NULL,
	};
	int rc;
	int typenum = VIRTIO_TYPE_UNKNOWN;

	if (params != NULL) {
		rc = spdk_json_decode_object(params, g_rpc_ssam_clear_iostat_decoder,
					     SPDK_COUNTOF(g_rpc_ssam_clear_iostat_decoder), &req);
		if (rc != 0) {
			SPDK_ERRLOG("spdk_json_decode_object failed\n");
			rc = -EINVAL;
			goto invalid;
		}
	}

	if (req.type != NULL) {
		if (strcmp(req.type, SPDK_SESSION_TYPE_FS) == 0) {
			typenum = VIRTIO_TYPE_FS;
		} else if (strcmp(req.type, SPDK_SESSION_TYPE_SCSI) == 0) {
			typenum = VIRTIO_TYPE_SCSI;
		} else if (strcmp(req.type, SPDK_SESSION_TYPE_BLK) == 0) {
			typenum = VIRTIO_TYPE_BLK;
		} else {
			rc = -EINVAL;
			goto invalid;
		}
	}

	rpc_ssam_clear_iostat(typenum);
	free_rpc_ssam_clear_iostat(&req);
	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	free_rpc_ssam_clear_iostat(&req);
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));
}
SPDK_RPC_REGISTER("controller_clear_iostat", rpc_ssam_controller_clear_iostat, SPDK_RPC_RUNTIME)

struct rpc_bdev_resize {
	uint32_t function_id;
	uint64_t new_size_in_mb;
};

static const struct spdk_json_object_decoder g_rpc_bdev_resize[] = {
	{"function_id", offsetof(struct rpc_bdev_resize, function_id), spdk_json_decode_uint32},
	{"new_size_in_mb", offsetof(struct rpc_bdev_resize, new_size_in_mb), spdk_json_decode_uint64},
};

static int
ssam_bdev_resize(struct spdk_bdev *bdev, uint64_t new_size_in_mb)
{
	char *bdev_name = bdev->name;
	int rc;
	uint64_t current_size_in_mb;
	uint64_t new_size_in_byte;

	if (bdev->blocklen == 0) {
		SPDK_ERRLOG("The blocklen of bdev %s is zero\n", bdev_name);
		return -EINVAL;
	}

	if (UINT64_MAX / bdev->blockcnt < bdev->blocklen) {
		SPDK_ERRLOG("The old size of bdev is too large, blockcnt: %lu, blocklen: %u\n",
			    bdev->blockcnt, bdev->blocklen);
		return -EINVAL;
	}

	if (new_size_in_mb == 0) {
		goto end;
	}

	current_size_in_mb = bdev->blocklen * bdev->blockcnt / SSAM_MB;
	if (new_size_in_mb < current_size_in_mb) {
		SPDK_ERRLOG("The new bdev size must not be smaller than current bdev size\n");
		return -EINVAL;
	}

	if (UINT64_MAX / new_size_in_mb < SSAM_MB) {
		SPDK_ERRLOG("The new bdev size is too large\n");
		return -EINVAL;
	}

end:
	new_size_in_byte = new_size_in_mb * SSAM_MB;

	rc = spdk_bdev_notify_blockcnt_change(bdev, new_size_in_byte / bdev->blocklen);
	if (rc != 0) {
		SPDK_ERRLOG("failed to notify block cnt change\n");
		return -EINVAL;
	}
	SPDK_NOTICELOG("bdev %s resize %lu(mb) done.\n", bdev->name, new_size_in_mb);

	return 0;
}

static void
rpc_ssam_bdev_resize(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct rpc_bdev_resize req = {0};
	uint16_t gfunc_id = SPDK_INVALID_GFUNC_ID;
	struct spdk_ssam_session *smsession = NULL;
	struct spdk_bdev *bdev = NULL;
	int rc;

	if (params == NULL) {
		SPDK_ERRLOG("rpc_ssam_bdev_resize params null\n");
		rc = -EINVAL;
		goto invalid;
	}

	rc = spdk_json_decode_object(params, g_rpc_bdev_resize,
				     SPDK_COUNTOF(g_rpc_bdev_resize), &req);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	gfunc_id = req.function_id;
	rc = ssam_rpc_para_check_type(gfunc_id, SSAM_DEVICE_VIRTIO_BLK);
	if (rc != 0) {
		goto invalid;
	}

	ssam_lock();
	smsession = ssam_session_find(gfunc_id);
	if (smsession == NULL) {
		SPDK_ERRLOG("Before resize target, there need to create controller.\n");
		rc = -ENODEV;
		ssam_unlock();
		goto invalid;
	}

	if (smsession->backend->get_bdev != NULL) {
		bdev = smsession->backend->get_bdev(smsession, 0);
	}
	if (bdev == NULL) {
		SPDK_ERRLOG("The controller hasn't correlated to a bdev.\n");
		rc = -ENODEV;
		ssam_unlock();
		goto invalid;
	}

	ssam_unlock();

	rc = ssam_bdev_resize(bdev, req.new_size_in_mb);
	if (rc != 0) {
		goto invalid;
	}

	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));
}

SPDK_RPC_REGISTER("bdev_resize", rpc_ssam_bdev_resize, SPDK_RPC_RUNTIME)

struct rpc_scsi_bdev_resize {
	char *name;
	uint32_t tgt_id;
	uint64_t new_size_in_mb;
};

static const struct spdk_json_object_decoder g_rpc_scsi_bdev_resize[] = {
	{"name", offsetof(struct rpc_scsi_bdev_resize, name), spdk_json_decode_string},
	{"tgt_id", offsetof(struct rpc_scsi_bdev_resize, tgt_id), spdk_json_decode_uint32},
	{"new_size_in_mb", offsetof(struct rpc_scsi_bdev_resize, new_size_in_mb), spdk_json_decode_uint64},
};

static void
free_rpc_scsi_bdev_resize(struct rpc_scsi_bdev_resize *req)
{
	if (req->name != NULL) {
		free(req->name);
		req->name = NULL;
	}
}

static void
rpc_ssam_scsi_bdev_resize(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct rpc_scsi_bdev_resize req = {0};
	uint16_t gfunc_id = SPDK_INVALID_GFUNC_ID;
	struct spdk_ssam_session *smsession = NULL;
	struct spdk_bdev *bdev = NULL;
	int rc;

	if (params == NULL) {
		SPDK_ERRLOG("rpc_ssam_bdev_resize params null\n");
		rc = -EINVAL;
		goto invalid;
	}

	rc = spdk_json_decode_object(params, g_rpc_scsi_bdev_resize,
				     SPDK_COUNTOF(g_rpc_scsi_bdev_resize), &req);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	gfunc_id = ssam_get_gfunc_id_by_name(req.name);
	rc = ssam_rpc_para_check_type(gfunc_id, SSAM_DEVICE_VIRTIO_SCSI);
	if (rc != 0) {
		goto invalid;
	}

	ssam_lock();
	smsession = ssam_session_find(gfunc_id);
	if (smsession == NULL) {
		SPDK_ERRLOG("Before resize target, there need to create controller.\n");
		rc = -ENODEV;
		ssam_unlock();
		goto invalid;
	}

	if (smsession->backend->get_bdev != NULL) {
		bdev = smsession->backend->get_bdev(smsession, req.tgt_id);
	}
	if (bdev == NULL) {
		rc = -ENODEV;
		ssam_unlock();
		goto invalid;
	}

	ssam_unlock();

	rc = ssam_bdev_resize(bdev, req.new_size_in_mb);
	if (rc != 0) {
		goto invalid;
	}

	free_rpc_scsi_bdev_resize(&req);
	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	free_rpc_scsi_bdev_resize(&req);
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));
}

SPDK_RPC_REGISTER("scsi_bdev_resize", rpc_ssam_scsi_bdev_resize, SPDK_RPC_RUNTIME)

struct rpc_bdev_aio_resize {
	char *name;
	uint64_t new_size_in_mb;
};

static const struct spdk_json_object_decoder g_rpc_bdev_aio_resize[] = {
	{"name", offsetof(struct rpc_bdev_aio_resize, name), spdk_json_decode_string},
	{"new_size_in_mb", offsetof(struct rpc_bdev_aio_resize, new_size_in_mb), spdk_json_decode_uint64},
};

static void
free_rpc_ssam_bdev_aio_resize(struct rpc_bdev_aio_resize *req)
{
	if (req->name != NULL) {
		free(req->name);
		req->name = NULL;
	}
}

static void
rpc_ssam_bdev_aio_resize(struct spdk_jsonrpc_request *request,
			 const struct spdk_json_val *params)
{
	struct rpc_bdev_aio_resize req = {0};
	struct spdk_bdev *bdev = NULL;
	int rc;

	if (params == NULL) {
		SPDK_ERRLOG("rpc_ssam_bdev_resize params null\n");
		rc = -EINVAL;
		goto invalid;
	}

	rc = spdk_json_decode_object(params, g_rpc_bdev_aio_resize,
				     SPDK_COUNTOF(g_rpc_bdev_aio_resize), &req);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	if (req.name) {
		bdev = spdk_bdev_get_by_name(req.name);
		if (bdev == NULL) {
			SPDK_ERRLOG("bdev '%s' does not exist\n", req.name);
			rc = -EINVAL;
			goto invalid;
		}
	}

	rc = ssam_bdev_resize(bdev, req.new_size_in_mb);
	if (rc != 0) {
		goto invalid;
	}

	free_rpc_ssam_bdev_aio_resize(&req);
	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	free_rpc_ssam_bdev_aio_resize(&req);
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));
}

SPDK_RPC_REGISTER("bdev_aio_resize", rpc_ssam_bdev_aio_resize, SPDK_RPC_RUNTIME)

static void
rpc_os_ready(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	int rc = 0;
	int fd;
	char *enable = "1";

	fd = open(SSAM_STORAGE_READY_FILE, O_RDWR);
	if (fd < 0) {
		SPDK_ERRLOG("Open storage ready file failed.\n");
		rc = EPERM;
		goto invalid;
	}

	rc = write(fd, enable, strlen(enable));
	if (rc < 0) {
		SPDK_ERRLOG("Write storage ready file failed.\n");
		close(fd);
		goto invalid;
	}

	close(fd);
	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));
}

SPDK_RPC_REGISTER("os_ready", rpc_os_ready, SPDK_RPC_RUNTIME)

struct rpc_create_scsi_controller {
	char *dbdf;
	char *name;
};

static const struct spdk_json_object_decoder g_rpc_create_scsi_controller[] = {
	{"dbdf", offsetof(struct rpc_create_scsi_controller, dbdf), spdk_json_decode_string},
	{"name", offsetof(struct rpc_create_scsi_controller, name), spdk_json_decode_string},
};

static void
free_rpc_ssam_create_scsi_controller(struct rpc_create_scsi_controller *req)
{
	if (req->name != NULL) {
		free(req->name);
		req->name = NULL;
	}
	if (req->dbdf != NULL) {
		free(req->dbdf);
		req->dbdf = NULL;
	}
}

static int
ssam_rpc_get_gfunc_id_by_dbdf(char *dbdf, uint16_t *gfunc_id)
{
	int rc;
	uint32_t dbdf_num;

	rc = ssam_dbdf_str2num(dbdf, &dbdf_num);
	if (rc != 0) {
		SPDK_ERRLOG("convert dbdf(%s) to num failed, rc: %d.\n", dbdf, rc);
		return -EINVAL;
	}

	rc = ssam_get_funcid_by_dbdf(dbdf_num, gfunc_id);
	if (rc != 0) {
		SPDK_ERRLOG("find gfuncid by dbdf(%u) failed, rc: %d.\n", dbdf_num, rc);
		return -EINVAL;
	}

	return 0;
}

static int
ssam_rpc_para_check_name(char *name)
{
	uint16_t gfunc_id = ssam_get_gfunc_id_by_name(name);
	if (gfunc_id == SPDK_INVALID_GFUNC_ID) {
		return 0;
	}

	return -EEXIST;
}

static void
rpc_ssam_create_scsi_controller(struct spdk_jsonrpc_request *request,
				const struct spdk_json_val *params)
{
	struct spdk_ssam_session_reg_info info = {0};
	struct rpc_create_scsi_controller req = {0};
	uint16_t gfunc_id = SPDK_INVALID_GFUNC_ID;
	int rc;
	uint16_t queues;

	if (params == NULL) {
		SPDK_ERRLOG("rpc_ssam_create_scsi_controller params null\n");
		rc = -EINVAL;
		goto invalid;
	}

	rc = spdk_json_decode_object(params, g_rpc_create_scsi_controller,
				     SPDK_COUNTOF(g_rpc_create_scsi_controller), &req);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	rc = ssam_rpc_para_check_name(req.name);
	if (rc != 0) {
		SPDK_ERRLOG("controller name(%s) is existed\n", req.name);
		goto invalid;
	}

	rc = ssam_rpc_get_gfunc_id_by_dbdf(req.dbdf, &gfunc_id);
	if (rc != 0) {
		goto invalid;
	}

	rc = ssam_rpc_para_check_type(gfunc_id, SSAM_DEVICE_VIRTIO_SCSI);
	if (rc != 0) {
		goto invalid;
	}

	queues = ssam_get_queues();
	if (queues > SPDK_SSAM_MAX_VQUEUES) {
		SPDK_ERRLOG("Queue number out of range, need less or equal than %u, actually %u.\n",
			    SPDK_SSAM_MAX_VQUEUES, queues);
		rc = -EINVAL;
		goto invalid;
	}

	rpc_init_session_reg_info(&info, queues, gfunc_id, request);

	info.name = strdup(req.name);
	if (info.name == NULL) {
		SPDK_ERRLOG("Failed to create name(%s) for ssam session reg info.\n", req.name);
		rc = -EINVAL;
		goto invalid;
	}

	info.dbdf = strdup(req.dbdf);
	if (info.dbdf == NULL) {
		SPDK_ERRLOG("Failed to create dbdf(%s) for ssam session reg info.\n", req.dbdf);
		rc = -EINVAL;
		goto invalid;
	}

	rc = ssam_scsi_construct(&info);
	if (rc < 0) {
		goto invalid;
	}

	free_rpc_ssam_create_scsi_controller(&req);
	free_rpc_ssam_session_reg_info(&info);
	return;

invalid:
	free_rpc_ssam_create_scsi_controller(&req);
	free_rpc_ssam_session_reg_info(&info);
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));
	return;
}

SPDK_RPC_REGISTER("create_scsi_controller", rpc_ssam_create_scsi_controller, SPDK_RPC_RUNTIME)

struct rpc_scsi_controller_add_target {
	char *name;
	int32_t scsi_tgt_num;
	char *bdev_name;
};

static const struct spdk_json_object_decoder g_rpc_scsi_controller_add_target[] = {
	{"name", offsetof(struct rpc_scsi_controller_add_target, name), spdk_json_decode_string},
	{"scsi_tgt_num", offsetof(struct rpc_scsi_controller_add_target, scsi_tgt_num), spdk_json_decode_uint32},
	{"bdev_name", offsetof(struct rpc_scsi_controller_add_target, bdev_name), spdk_json_decode_string},
};

static void
free_rpc_ssam_scsi_ctrlr_add_target(struct rpc_scsi_controller_add_target *req)
{
	if (req->name != NULL) {
		free(req->name);
		req->name = NULL;
	}
	if (req->bdev_name != NULL) {
		free(req->bdev_name);
		req->bdev_name = NULL;
	}
}

static void
rpc_ssam_scsi_controller_add_target(struct spdk_jsonrpc_request *request,
				    const struct spdk_json_val *params)
{
	struct rpc_scsi_controller_add_target req = {0};
	uint16_t gfunc_id = SPDK_INVALID_GFUNC_ID;
	struct spdk_ssam_session *smsession;
	int rc;

	if (params == NULL) {
		SPDK_ERRLOG("rpc_ssam_scsi_controller_add_target params null\n");
		rc = -EINVAL;
		goto invalid;
	}

	rc = spdk_json_decode_object(params, g_rpc_scsi_controller_add_target,
				     SPDK_COUNTOF(g_rpc_scsi_controller_add_target), &req);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	gfunc_id = ssam_get_gfunc_id_by_name(req.name);
	rc = ssam_rpc_para_check_type(gfunc_id, SSAM_DEVICE_VIRTIO_SCSI);
	if (rc != 0) {
		goto invalid;
	}

	ssam_lock();
	smsession = ssam_session_find(gfunc_id);
	if (smsession == NULL) {
		SPDK_ERRLOG("Before adding a SCSI target, there should be a SCSI controller.\n");
		rc = -ENODEV;
		ssam_unlock();
		goto invalid;
	}

	rc = rpc_ssam_session_reg_response_cb(smsession, request);
	if (rc != 0) {
		SPDK_ERRLOG("The controller is being operated.\n");
		rc = -EALREADY;
		ssam_unlock();
		goto invalid;
	}

	rc = ssam_scsi_dev_add_tgt(smsession, req.scsi_tgt_num, req.bdev_name);
	if (rc != 0) {
		/*
		 * Unregitster response cb to avoid use request in the cb function,
		 * because if error happend, request will be responsed immediately
		 */
		ssam_session_unreg_response_cb(smsession);
		ssam_unlock();
		goto invalid;
	}
	ssam_unlock();

	free_rpc_ssam_scsi_ctrlr_add_target(&req);
	return;

invalid:
	free_rpc_ssam_scsi_ctrlr_add_target(&req);
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));
}

SPDK_RPC_REGISTER("scsi_controller_add_target", rpc_ssam_scsi_controller_add_target,
		  SPDK_RPC_RUNTIME)

struct rpc_scsi_controller_remove_target {
	char *name;
	int32_t scsi_tgt_num;
};

static const struct spdk_json_object_decoder g_rpc_scsi_controller_remove_target[] = {
	{"name", offsetof(struct rpc_scsi_controller_remove_target, name), spdk_json_decode_string},
	{"scsi_tgt_num", offsetof(struct rpc_scsi_controller_remove_target, scsi_tgt_num), spdk_json_decode_int32},
};

static void
free_rpc_scsi_controller_remove_target(struct rpc_scsi_controller_remove_target *req)
{
	if (req->name != NULL) {
		free(req->name);
		req->name = NULL;
	}
}

static void
rpc_ssam_scsi_controller_remove_target(struct spdk_jsonrpc_request *request,
				       const struct spdk_json_val *params)
{
	struct rpc_scsi_controller_remove_target req = {0};
	uint16_t gfunc_id = SPDK_INVALID_GFUNC_ID;
	int rc;
	struct spdk_ssam_session *smsession = NULL;

	if (params == NULL) {
		SPDK_ERRLOG("rpc_ssam_scsi_controller_remove_target params null\n");
		rc = -EINVAL;
		goto invalid;
	}

	rc = spdk_json_decode_object(params, g_rpc_scsi_controller_remove_target,
				     SPDK_COUNTOF(g_rpc_scsi_controller_remove_target), &req);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	gfunc_id = ssam_get_gfunc_id_by_name(req.name);
	rc = ssam_rpc_para_check_type(gfunc_id, SSAM_DEVICE_VIRTIO_SCSI);
	if (rc != 0) {
		goto invalid;
	}

	ssam_lock();

	smsession = ssam_session_find(gfunc_id);
	if (smsession == NULL) {
		rc = -ENODEV;
		ssam_unlock();
		goto invalid;
	}

	rc = rpc_ssam_session_reg_response_cb(smsession, request);
	if (rc != 0) {
		SPDK_ERRLOG("The controller is being operated.\n");
		rc = -EALREADY;
		ssam_unlock();
		goto invalid;
	}

	rc = ssam_scsi_dev_remove_tgt(smsession, req.scsi_tgt_num,
				      rpc_ssam_send_response_cb, request);
	if (rc != 0) {
		/*
		 * Unregitster response cb to avoid use request in the cb function,
		 * because if error happend, request will be responsed immediately
		 */
		ssam_session_unreg_response_cb(smsession);
		ssam_unlock();
		goto invalid;
	}
	ssam_unlock();
	free_rpc_scsi_controller_remove_target(&req);
	return;

invalid:
	free_rpc_scsi_controller_remove_target(&req);
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));
}

SPDK_RPC_REGISTER("scsi_controller_remove_target", rpc_ssam_scsi_controller_remove_target,
		  SPDK_RPC_RUNTIME)

struct rpc_ssam_scsi_device_iostat {
	char *name;
	int32_t scsi_tgt_num;
};

static const struct spdk_json_object_decoder g_rpc_ssam_scsi_device_iostat[] = {
	{"name", offsetof(struct rpc_ssam_scsi_device_iostat, name), spdk_json_decode_string},
	{"scsi_tgt_num", offsetof(struct rpc_ssam_scsi_device_iostat, scsi_tgt_num), spdk_json_decode_int32},
};

static void
free_rpc_ssam_scsi_device_iostat(struct rpc_ssam_scsi_device_iostat *req)
{
	if (req->name != NULL) {
		free(req->name);
		req->name = NULL;
	}
}

static int
rpc_ssam_show_scsi_iostat(struct spdk_jsonrpc_request *request, uint16_t gfunc_id,
			  uint16_t scsi_tgt_num)
{
	struct spdk_json_write_ctx *w = NULL;
	struct spdk_ssam_session *smsession = NULL;

	ssam_lock();
	smsession = ssam_session_find(gfunc_id);
	if (smsession == NULL) {
		ssam_unlock();
		return -ENODEV;
	} else if (smsession->backend->type != VIRTIO_TYPE_SCSI) {
		ssam_unlock();
		return -EINVAL;
	}

	w = spdk_jsonrpc_begin_result(request);

	if (smsession->backend->show_iostat_json != NULL) {
		smsession->backend->show_iostat_json(smsession, scsi_tgt_num, w);
	}

	ssam_unlock();

	spdk_jsonrpc_end_result(request, w);
	return 0;
}

static void
rpc_ssam_scsi_device_iostat(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	struct rpc_ssam_scsi_device_iostat req = {0};
	uint16_t gfunc_id = SPDK_INVALID_GFUNC_ID;
	int rc;

	if (params == NULL) {
		SPDK_ERRLOG("rpc_ssam_scsi_device_iostat params null\n");
		rc = -EINVAL;
		goto invalid;
	}

	rc = spdk_json_decode_object(params, g_rpc_ssam_scsi_device_iostat,
				     SPDK_COUNTOF(g_rpc_ssam_scsi_device_iostat), &req);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	if (req.scsi_tgt_num < 0 || req.scsi_tgt_num > SPDK_SSAM_SCSI_CTRLR_MAX_DEVS) {
		SPDK_ERRLOG("scsi_tgt_num is out of range\n");
		rc = -EINVAL;
		goto invalid;
	}

	gfunc_id = ssam_get_gfunc_id_by_name(req.name);
	rc = ssam_rpc_para_check(gfunc_id);
	if (rc != 0) {
		goto invalid;
	}

	rc = rpc_ssam_show_scsi_iostat(request, gfunc_id, req.scsi_tgt_num);
	if (rc != 0) {
		goto invalid;
	}

	free_rpc_ssam_scsi_device_iostat(&req);
	return;

invalid:
	free_rpc_ssam_scsi_device_iostat(&req);
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
					 spdk_strerror(-rc));
	return;
}
SPDK_RPC_REGISTER("scsi_device_iostat", rpc_ssam_scsi_device_iostat, SPDK_RPC_RUNTIME)

struct rpc_limit_log_interval {
	int interval;
};

static void
rpc_ssam_device_pcie_list(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w = NULL;
	int rc;
	uint32_t size = ssam_get_device_pcie_list_size();
	if (size == 0) {
		rc = ssam_init_device_pcie_list();
		if (rc != 0) {
			SPDK_ERRLOG("init device_pcie_list failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 spdk_strerror(-rc));
			return;
		}
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_object_begin(w);

	ssam_dump_device_pcie_list(w);

	spdk_json_write_object_end(w);
	spdk_jsonrpc_end_result(request, w);
	return;
}

SPDK_RPC_REGISTER("device_pcie_list", rpc_ssam_device_pcie_list, SPDK_RPC_RUNTIME)

struct rpc_fs_controller_create {
	char *dbdf;
	char *name;
	char *fsdev_name;
	uint16_t max_threads;
};

static const struct spdk_json_object_decoder g_rpc_fs_controller_create[] = {
	{"dbdf", offsetof(struct rpc_fs_controller_create, dbdf), spdk_json_decode_string},
	{"name", offsetof(struct rpc_fs_controller_create, name), spdk_json_decode_string},
	{"fsdev_name", offsetof(struct rpc_fs_controller_create, fsdev_name), spdk_json_decode_string},
	{"max_threads", offsetof(struct rpc_fs_controller_create, max_threads), spdk_json_decode_uint16, true},
};

static void
free_rpc_ssam_fs_controller_create(struct rpc_fs_controller_create *req)
{
	if (req->dbdf != NULL) {
		free(req->dbdf);
		req->dbdf = NULL;
	}
	if (req->name != NULL) {
		free(req->name);
		req->name = NULL;
	}
	if (req->fsdev_name != NULL) {
		free(req->fsdev_name);
		req->fsdev_name = NULL;
	}
}

static void
rpc_ssam_fs_controller_create(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params)
{
	struct ssam_fs_construct_info info = {
		.gfunc_id = SPDK_INVALID_GFUNC_ID,
		.dbdf = NULL,
		.name = NULL,
		.fsdev_name = NULL,
		.max_threads = SSAM_FS_DEFAULT_THREADS,
	};
	struct rpc_fs_controller_create req = {
		.max_threads = SPDK_INVALID_MAX_THREADS,
	};
	int rc;

	if (params == NULL) {
		SPDK_ERRLOG("rpc_ssam_fs_controller_create params null\n");
		rc = -EINVAL;
		goto invalid;
	}

	rc = spdk_json_decode_object(params, g_rpc_fs_controller_create,
				     SPDK_COUNTOF(g_rpc_fs_controller_create), &req);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	rc = ssam_rpc_para_check_name(req.name);
	if (rc != 0) {
		SPDK_ERRLOG("controller name(%s) is existed\n", req.name);
		goto invalid;
	}

	rc = ssam_rpc_get_gfunc_id_by_dbdf(req.dbdf, &info.gfunc_id);
	if (rc != 0) {
		goto invalid;
	}

	rc = ssam_rpc_para_check_type(info.gfunc_id, SSAM_DEVICE_VIRTIO_FS);
	if (rc != 0) {
		goto invalid;
	}

	info.dbdf = req.dbdf;
	info.name = req.name;
	info.fsdev_name = req.fsdev_name;
	if (req.max_threads != SPDK_INVALID_MAX_THREADS) {
		if (req.max_threads == 0 || req.max_threads > ssam_get_core_num()) {
			SPDK_ERRLOG("max_threads out of range, should bewteen 1 and %u.\n", ssam_get_core_num());
			rc = -ERANGE;
			goto invalid;
		}
		info.max_threads = req.max_threads;
	}
	rc = ssam_fs_construct(&info, request, rpc_ssam_send_response_cb);
	if (rc != 0) {
		SPDK_ERRLOG("contruct fs controller failed.\n");
		goto invalid;
	}

	free_rpc_ssam_fs_controller_create(&req);
	return;

invalid:
	free_rpc_ssam_fs_controller_create(&req);
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));
	return;
}

SPDK_RPC_REGISTER("fs_controller_create", rpc_ssam_fs_controller_create, SPDK_RPC_RUNTIME)

struct rpc_fs_controller_delete {
	char *name;
	bool force;
};

static const struct spdk_json_object_decoder g_rpc_fs_controller_delete[] = {
	{"name", offsetof(struct rpc_fs_controller_delete, name), spdk_json_decode_string},
	{"force", offsetof(struct rpc_fs_controller_delete, force), spdk_json_decode_bool},
};

static void
free_rpc_ssam_fs_controller_delete(struct rpc_fs_controller_delete *req)
{
	if (req->name != NULL) {
		free(req->name);
		req->name = NULL;
	}
}

static void
rpc_ssam_fs_controller_delete(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params)
{
	struct rpc_fs_controller_delete req = {0};
	int rc;

	if (params == NULL) {
		SPDK_ERRLOG("rpc_ssam_fs_controller_delete params null\n");
		rc = -EINVAL;
		goto invalid;
	}

	rc = spdk_json_decode_object(params, g_rpc_fs_controller_delete,
				     SPDK_COUNTOF(g_rpc_fs_controller_delete), &req);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	rc = ssam_fs_destory(req.name, req.force, request, rpc_ssam_send_response_cb);
	if (rc != 0) {
		goto invalid;
	}

	free_rpc_ssam_fs_controller_delete(&req);
	return;

invalid:
	free_rpc_ssam_fs_controller_delete(&req);
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));
}

SPDK_RPC_REGISTER("fs_controller_delete", rpc_ssam_fs_controller_delete, SPDK_RPC_RUNTIME)

struct rpc_fs_controller_list {
	char *name;
};

static const struct spdk_json_object_decoder g_rpc_fs_controller_list[] = {
	{"name", offsetof(struct rpc_fs_controller_list, name), spdk_json_decode_string, true},
};

static void
free_rpc_ssam_fs_controller_list(struct rpc_fs_controller_list *req)
{
	if (req->name != NULL) {
		free(req->name);
		req->name = NULL;
	}
}

static void
rpc_ssam_fs_controller_list(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	struct rpc_fs_controller_list req = {
		.name = NULL,
	};
	uint16_t gfunc_id = SPDK_INVALID_GFUNC_ID;
	int rc;

	if (params != NULL) {
		rc = spdk_json_decode_object(params, g_rpc_fs_controller_list,
					     SPDK_COUNTOF(g_rpc_fs_controller_list), &req);
		if (rc != 0) {
			SPDK_ERRLOG("spdk_json_decode_object failed\n");
			rc = -EINVAL;
			goto invalid;
		}
	}

	if (req.name != NULL) {
		gfunc_id = ssam_get_gfunc_id_by_name(req.name);
		rc = ssam_rpc_para_check(gfunc_id);
		if (rc != 0) {
			goto invalid;
		}
	}

	rc = rpc_ssam_show_fs_controllers(request, gfunc_id);
	if (rc != 0) {
		goto invalid;
	}

	free_rpc_ssam_fs_controller_list(&req);
	return;

invalid:
	free_rpc_ssam_fs_controller_list(&req);
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
					 spdk_strerror(-rc));
	return;
}

SPDK_RPC_REGISTER("fs_controller_list", rpc_ssam_fs_controller_list, SPDK_RPC_RUNTIME)

struct rpc_controller_get_fs_iostat {
	char *name;
};

static const struct spdk_json_object_decoder g_rpc_controller_get_fs_iostat_decoder[] = {
	{"name", offsetof(struct rpc_controller_get_fs_iostat, name), spdk_json_decode_string, true},
};

static void
free_rpc_ssam_controller_get_fs_iostat(struct rpc_controller_get_fs_iostat *req)
{
	if (req->name != NULL) {
		free(req->name);
		req->name = NULL;
	}
}

static int
rpc_ssam_show_fs_iostat(struct spdk_jsonrpc_request *request, uint16_t gfunc_id)
{
	struct spdk_ssam_dev *smdev = NULL;
	struct spdk_json_write_ctx *w = NULL;
	struct spdk_ssam_session *smsession = NULL;

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_object_begin(w);
	spdk_json_write_named_array_begin(w, "fs_controllers");
	ssam_lock();

	smdev = ssam_dev_next(NULL);
	while (smdev != NULL) {
		smsession = ssam_sessions_next(smdev->smsessions, NULL);
		while (smsession != NULL) {
			if (smsession->backend->show_iostat_json != NULL && smsession->backend->type == VIRTIO_TYPE_FS &&
			    (gfunc_id == SPDK_INVALID_GFUNC_ID || smsession->gfunc_id == gfunc_id)) {
				smsession->backend->show_iostat_json(smsession, SPDK_SSAM_SCSI_CTRLR_MAX_DEVS, w);
			}
			smsession = ssam_sessions_next(smdev->smsessions, smsession);
		}
		smdev = ssam_dev_next(smdev);
	}

	ssam_unlock();
	spdk_json_write_array_end(w);
	spdk_json_write_object_end(w);
	spdk_jsonrpc_end_result(request, w);
	return 0;
}

static void
rpc_ssam_controller_get_fs_iostat(struct spdk_jsonrpc_request *request,
				  const struct spdk_json_val *params)
{
	struct rpc_controller_get_fs_iostat req = {
		.name = NULL,
	};
	uint16_t gfunc_id = SPDK_INVALID_GFUNC_ID;
	int rc;

	if (params != NULL) {
		rc = spdk_json_decode_object(params, g_rpc_controller_get_fs_iostat_decoder,
					     SPDK_COUNTOF(g_rpc_controller_get_fs_iostat_decoder), &req);
		if (rc != 0) {
			SPDK_ERRLOG("spdk_json_decode_object failed\n");
			rc = -EINVAL;
			goto invalid;
		}
	}

	if (req.name != NULL) {
		gfunc_id = ssam_get_gfunc_id_by_name(req.name);
		rc = ssam_rpc_para_check(gfunc_id);
		if (rc != 0) {
			goto invalid;
		}
	}

	rc = rpc_ssam_show_fs_iostat(request, gfunc_id);
	if (rc != 0) {
		goto invalid;
	}

	free_rpc_ssam_controller_get_fs_iostat(&req);
	return;

invalid:
	free_rpc_ssam_controller_get_fs_iostat(&req);
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
					 spdk_strerror(-rc));
	return;
}
SPDK_RPC_REGISTER("fs_device_iostat", rpc_ssam_controller_get_fs_iostat, SPDK_RPC_RUNTIME)

static void 
_rpc_get_ssam_info(struct spdk_json_write_ctx *w, struct spdk_ssam_dev *smdev)
{
	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "ctrlr", ssam_dev_get_name(smdev));
	spdk_json_write_named_string_fmt(w, "cpumask", "0x%s",
					 spdk_cpuset_fmt(spdk_thread_get_cpumask(smdev->thread)));
	spdk_json_write_named_uint16(w, "io_request_num", (uint16_t)smdev->io_request_num);
	spdk_json_write_named_uint16(w, "dma_response_num", (uint16_t)smdev->dma_response_num);

	spdk_json_write_object_end(w);
	
}

static void 
rpc_get_ssam_info(struct spdk_jsonrpc_request *request,
				  const struct spdk_json_val *params)
{
	struct spdk_ssam_dev *smdev = NULL;
	struct spdk_json_write_ctx *w = NULL;

	w = spdk_jsonrpc_begin_result(request);
    spdk_json_write_array_begin(w);

	ssam_lock();
	smdev = ssam_dev_next(NULL);
	while (smdev != NULL) {
		_rpc_get_ssam_info(w, smdev);
		smdev = ssam_dev_next(smdev);
	}
	ssam_unlock();

	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);
}

SPDK_RPC_REGISTER("get_ssam_info", rpc_get_ssam_info, SPDK_RPC_RUNTIME)

static int
spdk_ssam_get_iocrc_check_flag(const char *toggle)
{
	if (toggle == NULL) {
		SPDK_ERRLOG("get io crc check para toggle null\n");
		return -1;
	}

	if (strcmp(toggle, "disable") == 0) {
		return 0;
	}

	if (strcmp(toggle, "enable") == 0) {
		return 1;
	}

	SPDK_ERRLOG("get io crc check para invalid\n");
	return -1;
}

struct rpc_set_crc_checklog_ctrlr {
	char *toggle;
};

static const struct spdk_json_object_decoder g_rpc_set_crc_checklog_ctrlr_decoder[] = {
	{"toggle", offsetof(struct rpc_set_crc_checklog_ctrlr, toggle), spdk_json_decode_string},
};

static void
free_rpc_set_crc_checklog_ctrlrs(struct rpc_set_crc_checklog_ctrlr *req)
{
	if (req->toggle != NULL) {
		free(req->toggle);
		req->toggle = NULL;
	}
}

static void
set_io_check_crc_para(int check)
{
	if (check == 1) {
		spdk_ssam_set_io_crc_check(true);
	}

	if (check == 0) {
		spdk_ssam_set_io_crc_check(false);
	}
}

static void
rpc_set_crc_checklog(struct spdk_jsonrpc_request *request,
	const struct spdk_json_val *params)
{
	struct rpc_set_crc_checklog_ctrlr req = {0};
	int io_crc_check = -1;
	int rc;

	if (params == NULL) {
		SPDK_ERRLOG("rpc_ssam_delete_controller params null\n");
		rc = -EINVAL;
		goto invalid;
	}

	rc = spdk_json_decode_object(params, g_rpc_set_crc_checklog_ctrlr_decoder,
		SPDK_COUNTOF(g_rpc_set_crc_checklog_ctrlr_decoder), &req);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	io_crc_check = spdk_ssam_get_iocrc_check_flag(req.toggle);
	if (io_crc_check != 0 && io_crc_check != 1) {
		SPDK_ERRLOG("set_crc_checklog invalid toggle\n");
		rc = -EINVAL;
		goto invalid;
	}

	set_io_check_crc_para(io_crc_check); /* 0: disable, 1: enable */

	SPDK_NOTICELOG("ssam set io crc check %s success\n", req.toggle);
	free_rpc_set_crc_checklog_ctrlrs(&req);
	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	free_rpc_set_crc_checklog_ctrlrs(&req);
	spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
	return;
}
SPDK_RPC_REGISTER("set_crc_checklog", rpc_set_crc_checklog, SPDK_RPC_STARTUP | SPDK_RPC_RUNTIME)

SPDK_LOG_REGISTER_COMPONENT(ssam_rpc)
