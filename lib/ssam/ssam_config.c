/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021-2025 Huawei Technologies Co.
 *   All rights reserved.
 */

#include <rte_malloc.h>
#include <linux/virtio_blk.h>
#include <linux/virtio_scsi.h>

#include "spdk/string.h"
#include "spdk/file.h"
#include "ssam_internal.h"

#define SSAM_JSON_DEFAULT_MEMPOOL_SIZE   1024
#define SSAM_JSON_MAX_MEMPOOL_SIZE       10240
#define HPD_CONFIG_POLLER_PERIOD		 (1000 * 1000)

enum ssam_dma_queue_num {
	SSAM_DMA_QUEUE_NUM_DISABLE = 0,
	SSAM_DMA_QUEUE_NUM_SMALL_IO = 1,
	SSAM_DMA_QUEUE_NUM_DEFAULT = 2,
	SSAM_DMA_QUEUE_NUM_LARGE_IO = 4,
};

struct ssam_user_config {
	char *cfg_file_name;
	uint32_t mempool_size;
	uint32_t queues;
	uint32_t dma_queue_num;
	char *mode;
	uint8_t hash_mode;
};

struct ssam_config {
	struct ssam_user_config user_config;
	struct ssam_hostep_info ep_info;
	uint32_t core_num;
	bool shm_created;
	bool en_hpd;
	struct spdk_poller *hpd_poller;
};

static struct ssam_config g_ssam_config;

static const struct spdk_json_object_decoder g_ssam_user_config_decoders[] = {
	{"mempool_size_mb", offsetof(struct ssam_user_config, mempool_size), spdk_json_decode_uint32},
	{"queues", offsetof(struct ssam_user_config, queues), spdk_json_decode_uint32},
	{"mode", offsetof(struct ssam_user_config, mode), spdk_json_decode_string},
};

static int
ssam_heap_malloc(const char *type, size_t size, int socket_arg,
		 unsigned int flags, size_t align, size_t bound, bool contig, struct ssam_melem *mem)
{
	void *addr = NULL;
	unsigned long long pg_size;
	int socket_id;
	int rc;
	uint64_t iova;

	addr = rte_malloc_socket(type, size, align, socket_arg);
	if (addr == NULL) {
		return -ENOMEM;
	}

	rc = ssam_malloc_elem_from_addr(addr, &pg_size, &socket_id);
	if (rc != 0) {
		ssam_free_ex(addr);
		return -ENOMEM;
	}

	iova = rte_malloc_virt2iova(addr);
	if (iova == RTE_BAD_IOVA) {
		ssam_free_ex(addr);
		return -ENOMEM;
	}

	mem->addr = addr;
	mem->iova = iova;
	mem->page_sz = pg_size;
	mem->socket_id = socket_id;
	return 0;
}

static int
ssam_heap_free(void *addr)
{
	return ssam_free_ex(addr);
}

static uint8_t
ssam_get_dma_queue_num_by_mode(void)
{
	if (g_ssam_config.user_config.mode == NULL) {
		return SSAM_DMA_QUEUE_NUM_DISABLE;
	}

	if (!strcasecmp(g_ssam_config.user_config.mode, "default")) {
		return SSAM_DMA_QUEUE_NUM_DEFAULT;
	} else if (!strcasecmp(g_ssam_config.user_config.mode, "small-IO")) {
		return SSAM_DMA_QUEUE_NUM_SMALL_IO;
	} else if (!strcasecmp(g_ssam_config.user_config.mode, "large-IO")) {
		return SSAM_DMA_QUEUE_NUM_LARGE_IO;
	}
	return SSAM_DMA_QUEUE_NUM_DISABLE;
}

static void
ssam_get_ssam_lib_init_config(struct ssam_lib_args *cfg)
{
	uint32_t core_num = g_ssam_config.core_num;
	if (spdk_ssam_get_hot_upgrade_status() == SSAM_HOT_UPGRADE_BEGIN) {
		cfg->role = SSAM_HOT_SWAP_PROCESS;
	} else {
		cfg->role = SSAM_NORMAL_PROCESS;
	}

	cfg->dma_queue_num = g_ssam_config.user_config.dma_queue_num;
	cfg->ssam_heap_malloc = ssam_heap_malloc;
	cfg->ssam_heap_free = ssam_heap_free;
	cfg->hash_mode = g_ssam_config.user_config.hash_mode;

	/* The number of tid is 1 greater than the number of cores. */
	cfg->core_num = core_num;
}

void
spdk_ssam_set_shm_created(bool shm_created)
{
	g_ssam_config.shm_created = shm_created;
}

bool
spdk_ssam_get_shm_created(void)
{
	return g_ssam_config.shm_created;
}

bool
ssam_get_en_hpd(void)
{
	return g_ssam_config.en_hpd;
}

int
ssam_set_core_num(uint32_t core_num)
{
	if (core_num > SSAM_MAX_CORE_NUM) {
		SPDK_ERRLOG("Invalid coremask, total cores need less or equal than %d, "
			    "actually %u, please check startup item.\n",
			    SSAM_MAX_CORE_NUM, core_num);
		return -EINVAL;
	}
	if (g_ssam_config.user_config.dma_queue_num == SSAM_DMA_QUEUE_NUM_LARGE_IO
	    && core_num > SSAM_MAX_CORE_NUM_WITH_LARGE_IO) {
		SPDK_ERRLOG("Invalid coremask, total cores need less or equal than %d, "
			    "actually %u, please check startup item.\n",
			    SSAM_MAX_CORE_NUM_WITH_LARGE_IO, core_num);
		return -EINVAL;
	}
	g_ssam_config.core_num = core_num;
	return 0;
}

uint16_t
ssam_get_core_num(void)
{
	return (uint16_t)g_ssam_config.core_num;
}

uint32_t
ssam_get_mempool_size(void)
{
	return g_ssam_config.user_config.mempool_size;
}

uint16_t
ssam_get_queues(void)
{
	uint16_t cfg_queues = (uint16_t)g_ssam_config.user_config.queues;

	if (cfg_queues == 0) {
		SPDK_INFOLOG(ssam_config, "Use default queues number: %u.\n", SPDK_SSAM_DEFAULT_VQUEUES);
		return SPDK_SSAM_DEFAULT_VQUEUES;
	}
	return cfg_queues;
}

uint8_t
ssam_get_hash_mode(void)
{
	return g_ssam_config.user_config.hash_mode;
}

enum ssam_device_type
ssam_get_virtio_type(uint16_t gfunc_id) {
	uint16_t vf_start, vf_end;
	struct ssam_pf_list *pf = g_ssam_config.ep_info.host_pf_list;

	for (uint32_t i = 0; i < SSAM_HOSTEP_NUM_MAX; i++)
	{
		if (pf[i].pf_funcid == UINT16_MAX) {
			continue;
		}
		if (gfunc_id == pf[i].pf_funcid) {
			return pf[i].pf_type;
		}

		vf_start = pf[i].vf_funcid_start;
		if (((uint32_t)vf_start + (uint32_t)pf[i].vf_num) > UINT16_MAX) {
			SPDK_ERRLOG("vf_start %u + vf_num %u out of range, need less or equal than %u.\n",
				    vf_start, pf[i].vf_num, UINT16_MAX);
			continue;
		}
		vf_end = vf_start + pf[i].vf_num;
		if ((gfunc_id >= vf_start) && (gfunc_id < vf_end)) {
			return pf[i].pf_type;
		}
	}

	return  SSAM_DEVICE_VIRTIO_MAX;
}

static void
ssam_get_virtio_blk_config(struct ssam_virtio_config *cfg)
{
	struct virtio_blk_config *dev_cfg = (struct virtio_blk_config *)cfg->device_config;

	cfg->device_feature = SPDK_SSAM_VIRTIO_BLK_DEFAULT_FEATURE;
	cfg->queue_num = g_ssam_config.user_config.queues;
	cfg->config_len = sizeof(struct virtio_blk_config);

	memset(dev_cfg, 0, cfg->config_len);
	dev_cfg->blk_size = 0x200;
	dev_cfg->min_io_size = 0;
	dev_cfg->capacity = 0;
	dev_cfg->num_queues = cfg->queue_num;
	dev_cfg->seg_max = 0x7d;
	dev_cfg->size_max = 0x200000;
	cfg->queue_size = VIRITO_DEFAULT_QUEUE_SIZE;

	return;
}

static void
ssam_get_virtio_scsi_config(struct ssam_virtio_config *cfg)
{
	struct virtio_scsi_config *dev_cfg = (struct virtio_scsi_config *)cfg->device_config;

	cfg->device_feature = SPDK_SSAM_VIRTIO_SCSI_DEFAULT_FEATURE;
	cfg->queue_num = g_ssam_config.user_config.queues;
	cfg->config_len = sizeof(struct virtio_scsi_config);

	memset(dev_cfg, 0, sizeof(struct virtio_scsi_config));
	dev_cfg->num_queues = 0x04;
	dev_cfg->seg_max = 0x6f;
	dev_cfg->max_sectors = 0x1ff;
	dev_cfg->cmd_per_lun = 0x80;
	dev_cfg->event_info_size = 0;
	dev_cfg->sense_size = 0x60;
	dev_cfg->cdb_size = 0x20;
	dev_cfg->max_channel = 0;
	dev_cfg->max_target = SPDK_SSAM_SCSI_CTRLR_MAX_DEVS;
	dev_cfg->max_lun = 0xff;
	cfg->queue_size = VIRITO_DEFAULT_QUEUE_SIZE;

	return;
}

static int
ssam_virtio_config_get(struct ssam_pf_list *pf, struct ssam_function_config *cfg)
{
	int ret = 0;

	cfg->gfunc_id = pf->pf_funcid;
	cfg->type = pf->pf_type;
	switch (cfg->type) {
	case SSAM_DEVICE_VIRTIO_BLK:
		ssam_get_virtio_blk_config(&cfg->virtio_config);
		break;
	case SSAM_DEVICE_VIRTIO_SCSI:
		ssam_get_virtio_scsi_config(&cfg->virtio_config);
		break;
	default: {
		SPDK_ERRLOG("function config init fail (%d|%d)\n", cfg->gfunc_id, cfg->type);
		ret = -EINVAL;
		break;
	}
	}

	return ret;
}

static int
ssam_setup_pf(struct ssam_pf_list *pf, struct ssam_function_config *cfg)
{
	int rc;

	rc = ssam_setup_function(pf->pf_funcid, pf->vf_num, pf->pf_type);
	if (rc != 0) {
		SPDK_ERRLOG("ssam init function(%u) failed:%s\n", pf->pf_funcid, spdk_strerror(-rc));
		return rc;
	}

	if (g_ssam_config.en_hpd == false) {
		rc = ssam_write_function_config(cfg);
		if (rc != 0) {
			SPDK_ERRLOG("ssam write function(%d) config failed:%s\n", cfg->gfunc_id, spdk_strerror(-rc));
			return rc;
		}
	}

	return 0;
}

static int
ssam_hotplug_cfg_poller(void *ctx)
{
	ssam_hotplug_cfg();
	return SPDK_POLLER_BUSY;
}

static int
ssam_virtio_config_init(struct ssam_hostep_info *ep_info)
{
	int rc = 0;
	uint32_t i;
	struct ssam_function_config cfg = {0};
	struct ssam_pf_list *pf = ep_info->host_pf_list;

	g_ssam_config.en_hpd = ssam_hotplug_enable_check();
	if (g_ssam_config.en_hpd) {
		g_ssam_config.hpd_poller = SPDK_POLLER_REGISTER(ssam_hotplug_cfg_poller, NULL, HPD_CONFIG_POLLER_PERIOD);
	}

	if (spdk_ssam_get_shm_created()) {
		/* If server is crashed from last time, no need setup config this time */
		return 0;
	}

	/**
	 * During chip initialization, the vq and msix resources are initialized.
	 * However, the ssam configuration may be different from the initialization configuration.
	 * In the scene of virtio-blk, resources will be alloced at the function `ssam_blk_controller_set_vqueue`.
	 * Therefore, the original resources need to be released before negotiation with the host end.
	 */
	for (i = 0; i < SSAM_HOSTEP_NUM_MAX; i++) {
		if (pf[i].pf_funcid == UINT16_MAX || pf[i].pf_type != SSAM_DEVICE_VIRTIO_BLK) {
			continue;
		}
		rc = ssam_virtio_blk_release_resource(i);
		if (rc != 0) {
			SPDK_WARNLOG("virtio blk release vq failed.\n");
		}
	}

	for (i = 0; i < SSAM_HOSTEP_NUM_MAX; i++) {
		if (pf[i].pf_funcid == UINT16_MAX) {
			continue;
		}
		rc = ssam_virtio_config_get(&pf[i], &cfg);
		if (rc != 0) {
			return rc;
		}
		rc = ssam_setup_pf(&pf[i], &cfg);
		if (rc != 0) {
			return rc;
		}
	}

	return rc;
}

static int
ssam_virtio_init(void)
{
	struct ssam_lib_args ssam_args = { 0 };
	struct ssam_hostep_info *ep_info = &g_ssam_config.ep_info;
	int rc;

	ssam_get_ssam_lib_init_config(&ssam_args);

	rc = ssam_lib_init(&ssam_args, ep_info);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to init ssam:%s\n", spdk_strerror(-rc));
		return rc;
	}

	rc = ssam_virtio_config_init(ep_info);
	if (rc != 0) {
		SPDK_ERRLOG("ssam virtio device init failed:%s\n", spdk_strerror(-rc));
		if (ssam_lib_exit() != 0) {
			SPDK_WARNLOG("ssam lib exit failed\n");
		}
		return rc;
	}

	return 0;
}

static int
ssam_user_config_default(void)
{
	struct ssam_user_config *user_config = &g_ssam_config.user_config;

	user_config->mempool_size = SSAM_JSON_DEFAULT_MEMPOOL_SIZE;
	/**
	 * If file param json file is not exist, queue number will be
	 * set default value SPDK_SSAM_DEFAULT_VQUEUES when user create controller.
	 */
	user_config->queues = SPDK_SSAM_DEFAULT_VQUEUES;
	user_config->dma_queue_num = SSAM_DMA_QUEUE_NUM_DEFAULT;
	user_config->mode = NULL;
	user_config->hash_mode = SSAM_VQ_HASH_MODE;

	return -ENOENT;
}

static int
ssam_user_config_file_read(const char *config_file, size_t *file_len,
			   void **json, ssize_t *value_size)
{
	FILE *read_json = fopen(config_file, "r");
	ssize_t ret;
	void *end = NULL;

	if (read_json == NULL) {
		if (errno != ENOENT) {
			SPDK_ERRLOG("Read JSON configuration file \"%s\" failed\n", config_file);
			return -1;
		}
		SPDK_WARNLOG("JSON config file:%s does not exist! Use default configuration.\n",
			     config_file);
		return ssam_user_config_default();
	}

	void *load = spdk_posix_file_load(read_json, file_len);
	fclose(read_json);
	if (load == NULL) {
		return -1;
	}

	ret = spdk_json_parse(load, *file_len, NULL, 0, &end, SPDK_JSON_PARSE_FLAG_ALLOW_COMMENTS);
	if (ret < 0) {
		SPDK_ERRLOG("Parsing JSON configuration file \"%s\" failed (%zd)\n", config_file, ret);
		free(load);
		load = NULL;
		if (ret == -ENOENT) {   /* json file exists, but content is null */
			SPDK_ERRLOG("json file exists, but content is null\n");
			ret = -1;
		}
		return ret;
	}
	*json = load;
	*value_size = ret;

	return 0;
}

static void
ssam_user_config_free(struct ssam_user_config *user_config)
{
	if (user_config->mode != NULL) {
		free(user_config->mode);
		user_config->mode = NULL;
	}
}

static int
ssam_user_config_parse(size_t file_len, void *json, ssize_t value_size)
{
	struct spdk_json_val *value;
	struct ssam_user_config *user_config = &g_ssam_config.user_config;
	ssize_t ret;
	void *end = NULL;
	int rc;

	value = calloc(value_size, sizeof(struct spdk_json_val));
	if (value == NULL) {
		SPDK_ERRLOG("Out of memory\n");
		free(json);
		return -ENOMEM;
	}

	ret = spdk_json_parse(json, file_len, value, value_size, &end, SPDK_JSON_PARSE_FLAG_ALLOW_COMMENTS);
	if (ret != value_size) {
		SPDK_ERRLOG("Parsing JSON configuration file failed\n");
		free(json);
		free(value);
		return -1;
	}

	/* resolve json values to struct spdk_ssam_json_config */

	rc = spdk_json_decode_object(value, g_ssam_user_config_decoders,
				     SPDK_COUNTOF(g_ssam_user_config_decoders), user_config);
	free(json);
	free(value);
	if (rc != 0) {
		SPDK_ERRLOG("decode object failed:%s\n", spdk_strerror(-rc));
		ssam_user_config_free(user_config);
		return -1;
	}
	user_config->hash_mode = SSAM_VQ_HASH_MODE;

	return 0;
}

static int
ssam_user_config_check(void)
{
	struct ssam_user_config *user_config = &g_ssam_config.user_config;

	if (user_config->mempool_size < SSAM_JSON_DEFAULT_MEMPOOL_SIZE) {
		SPDK_ERRLOG("mempool_size_mb value in file %s out of range, need larger or equal than %u MB, actually %u MB.\n",
			    user_config->cfg_file_name, SSAM_JSON_DEFAULT_MEMPOOL_SIZE, user_config->mempool_size);
		return -EINVAL;
	}

	if (user_config->mempool_size > SSAM_JSON_MAX_MEMPOOL_SIZE) {
		SPDK_ERRLOG("mempool_size_mb value in file %s out of range, need less or equal than %u MB, actually %u MB.\n",
			    user_config->cfg_file_name, SSAM_JSON_MAX_MEMPOOL_SIZE, user_config->mempool_size);
		return -EINVAL;
	}

	if (user_config->queues > SPDK_SSAM_MAX_VQUEUES) {
		SPDK_ERRLOG("queues value in file %s out of range, need less or equal than %u, actually %u\n",
			    user_config->cfg_file_name, SPDK_SSAM_MAX_VQUEUES, user_config->queues);
		return -EINVAL;
	}

	if (user_config->queues == 0) {
		SPDK_ERRLOG("queues value in file %s out of range, need not equal to 0\n",
			    user_config->cfg_file_name);
		return -EINVAL;
	}

	user_config->dma_queue_num = ssam_get_dma_queue_num_by_mode();
	if (user_config->dma_queue_num == SSAM_DMA_QUEUE_NUM_DISABLE) {
		SPDK_ERRLOG("Invalid mode in file %s, which should be chosen from default, small-IO, large-IO, "
			    "actually %s\n",
			    user_config->mode, ssam_rc_get_param_json_file_path());
		return -EINVAL;
	}
	return 0;
}

int
spdk_ssam_user_config_init(void)
{
	size_t file_len = 0;
	void *json = NULL;
	ssize_t value_size = 0;
	int rc;
	struct ssam_user_config *user_config = &g_ssam_config.user_config;

	user_config->cfg_file_name = ssam_rc_get_param_json_file_path();
	rc = ssam_user_config_file_read(user_config->cfg_file_name, &file_len, &json, &value_size);
	if (rc != 0) {
		if (rc == -ENOENT) {
			return 0;
		}
		return rc;
	}

	rc = ssam_user_config_parse(file_len, json, value_size);
	if (rc != 0) {
		return rc;
	}

	rc = ssam_user_config_check();
	if (rc != 0) {
		ssam_user_config_free(&g_ssam_config.user_config);
		return rc;
	}

	return 0;
}

static void
ssam_virtio_exit(void)
{
	int rc;

	rc = ssam_lib_exit();
	if (rc != 0) {
		SPDK_WARNLOG("ssam lib exit failed\n");
	}
}

void
ssam_unregister_hpd_poller(void)
{
	if (g_ssam_config.hpd_poller != NULL) {
		spdk_poller_unregister(&g_ssam_config.hpd_poller);
		g_ssam_config.hpd_poller = NULL;
	}
}

int
ssam_config_init(void)
{
	int rc;

	rc = ssam_virtio_init();
	if (rc != 0) {
		return rc;
	}

	return 0;
}

void
ssam_config_exit(void)
{
	ssam_virtio_exit();
}

SPDK_LOG_REGISTER_COMPONENT(ssam_config)
