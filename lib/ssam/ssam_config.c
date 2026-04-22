/*
 *   BSD LICENSE
 *   Copyright (c) Huawei Technologies Co., Ltd. 2021-2021. All rights reserved.
 *
 */

#include <rte_malloc.h>
#include <linux/virtio_blk.h>
#include <linux/virtio_scsi.h>

#include "spdk/string.h"
#include "spdk/file.h"
#include "ssam_internal.h"

/* dma queue must be 1 with 15 core */
#define SSAM_DEFAULT_DMA_QUEUE_NUM       1
#define SSAM_DEFAULT_MEMPOOL_SIZE        1024
#define SSAM_DEFAULT_FS_MEMPOOL_SIZE     256
#define SSAM_MAX_DMA_QUEUE_NUM           4
#define SPDK_SSAM_VIRTIO_BLK_DEFAULT_FEATURE     0x3f11001046
#define SPDK_SSAM_VIRTIO_SCSI_DEFAULT_FEATURE    0x3f11000007
#define SPDK_SSAM_VIRTIO_FS_DEFAULT_FEATURE      0x3f19000000

struct ssam_user_config {
	uint32_t mempool_size;
	uint32_t queues;
	uint32_t extra_size;
	uint32_t dma_queue_num;
	uint32_t fs_mempool_size;
};

struct ssam_config {
	struct ssam_user_config user_config;
	struct ssam_hostep_info ep_info;
	uint32_t core_num;
	bool shm_created;
	bool virtio_fs_enable;
};

struct ssam_fs_config {
	uint16_t queue_id;
	ssam_mempool_t *mp;
};

static struct ssam_config g_ssam_config;

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

static void
ssam_get_ssam_lib_init_config(struct ssam_lib_args *cfg)
{
	uint32_t core_num = g_ssam_config.core_num;

	cfg->role = 0;
	cfg->dma_queue_num = g_ssam_config.user_config.dma_queue_num;
	cfg->ssam_heap_malloc = ssam_heap_malloc;
	cfg->ssam_heap_free = ssam_heap_free;

	/* The number of tid is 1 greater than the number of cores. */
	cfg->core_num = core_num;
}

void spdk_ssam_set_shm_created(bool shm_created)
{
	g_ssam_config.shm_created = shm_created;
}

bool spdk_ssam_get_shm_created(void)
{
	return g_ssam_config.shm_created;
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
	if (g_ssam_config.user_config.dma_queue_num == SSAM_MAX_DMA_QUEUE_NUM
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

bool
ssam_get_virtio_fs_enable(void)
{
	return g_ssam_config.virtio_fs_enable;
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

static struct ssam_fs_config g_ssam_fs_config_map[SSAM_HOSTEP_NUM_MAX];

uint16_t
ssam_get_queue_id(uint32_t func_id)
{
	if (func_id >= SSAM_HOSTEP_NUM_MAX) {
		return 0;
	}
	return g_ssam_fs_config_map[func_id].queue_id;
}

ssam_mempool_t *
ssam_get_fs_mp(uint32_t func_id)
{
	if (func_id >= SSAM_HOSTEP_NUM_MAX) {
		return 0;
	}
	return g_ssam_fs_config_map[func_id].mp;
}

static int
ssam_virtio_fs_config_init(struct ssam_hostep_info *ep_info)
{
	int rc = 0;
	uint32_t i;
	uint16_t queue_id;
	struct ssam_pf_list *pf = ep_info->host_pf_list;

	for (i = 0; i < SSAM_HOSTEP_NUM_MAX; i++) {
		if (pf[i].pf_funcid == UINT16_MAX || pf[i].pf_type != SSAM_DEVICE_VIRTIO_FS) {
			continue;
		}
		rc = ssam_vmio_rxq_create(&queue_id);
		if (rc < 0) {
			SPDK_ERRLOG("Failed to create vmio rx queue: %d\n", rc);
			return -1;
		}
		g_ssam_fs_config_map[pf[i].pf_funcid].queue_id = queue_id;
		g_ssam_fs_config_map[pf[i].pf_funcid].mp =
			ssam_mempool_create(g_ssam_config.user_config.fs_mempool_size * SSAM_MB,
					    SSAM_DEFAULT_MEMPOOL_EXTRA_SIZE);
		if (g_ssam_fs_config_map[pf[i].pf_funcid].mp == NULL) {
			SPDK_ERRLOG("ssam create fs mempool failed, mempool_size = %uMB.\n",
				    g_ssam_config.user_config.fs_mempool_size);
			return -ENOMEM;
		}
		g_ssam_config.virtio_fs_enable = true;
	}

	return 0;
}

static int
ssam_get_virtio_fs_config(struct ssam_virtio_config *cfg, uint32_t func_id)
{
	int ret = 0;
	uint32_t *buf = (uint32_t *)cfg->device_config;

	cfg->device_feature = SPDK_SSAM_VIRTIO_FS_DEFAULT_FEATURE;
	cfg->queue_num = g_ssam_config.user_config.queues;
	cfg->queue_size = VIRITO_FS_DEFAULT_QUEUE_SIZE;
	cfg->rx_queue_id = ssam_get_queue_id(func_id);
	cfg->config_len = VIRTIO_FS_DEFAULT_CONFIG_LEN;

	memset(buf, 0, sizeof(cfg->device_config));
	ret = snprintf((char *)buf, VIRTIO_FS_DEFAULT_TAG_LEN, "FS_%u", func_id);
	if (ret < 0 || ret >= VIRTIO_FS_DEFAULT_TAG_LEN) {
		SPDK_ERRLOG("Failed to init tag of func_id: %u\n", func_id);
		return -EINVAL;
	}
	*(buf + VIRTIO_FS_DEFAULT_CONFIG_QUEUE_OFFSET) = g_ssam_config.user_config.queues;

	return 0;
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
	case SSAM_DEVICE_VIRTIO_FS:
		ret = ssam_get_virtio_fs_config(&cfg->virtio_config, cfg->gfunc_id);
		if (ret != 0) {
			return ret;
		}
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
	rc = ssam_write_function_config(cfg);
	if (rc != 0) {
		SPDK_ERRLOG("ssam write function(%d) config failed:%s\n", cfg->gfunc_id, spdk_strerror(-rc));
		return rc;
	}

	return 0;
}

static int
ssam_setup_vf(struct ssam_pf_list *pf, struct ssam_function_config *cfg)
{
	struct ssam_function_config l_cfg;
	uint16_t vf_funcid_start = pf->vf_funcid_start;
	uint16_t vf_num = pf->vf_num;
	int rc;
	uint16_t i;

	if (((uint32_t)vf_funcid_start + (uint32_t)vf_num) > UINT16_MAX) {
		SPDK_ERRLOG("vf_funcid_start %u or vf_num %u out of range.\n",
			    vf_funcid_start, vf_num);
		return -1;
	}

	memcpy(&l_cfg, cfg, sizeof(struct ssam_function_config));
	for (i = vf_funcid_start; i < vf_funcid_start + vf_num; i++) {
		l_cfg.gfunc_id = i;
		rc = ssam_write_function_config(&l_cfg);
		if (rc != 0) {
			SPDK_ERRLOG("ssam write function(%u) config failed:%s\n", i, spdk_strerror(-rc));
			return rc;
		}
	}

	return 0;
}

static int
ssam_virtio_config_init(struct ssam_hostep_info *ep_info)
{
	int rc = 0;
	uint32_t i;
	struct ssam_function_config cfg = {0};
	struct ssam_pf_list *pf = ep_info->host_pf_list;

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
		rc = ssam_setup_vf(&pf[i], &cfg);
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

	rc = ssam_virtio_fs_config_init(ep_info);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to init virtio fs config:%s\n", spdk_strerror(-rc));
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

void
spdk_ssam_user_config_init(void)
{
	struct ssam_user_config *user_config = &g_ssam_config.user_config;

	user_config->queues = SPDK_SSAM_DEFAULT_VQUEUES;
	user_config->dma_queue_num = SSAM_DEFAULT_DMA_QUEUE_NUM;
	user_config->mempool_size = SSAM_DEFAULT_MEMPOOL_SIZE;
	user_config->fs_mempool_size = SSAM_DEFAULT_FS_MEMPOOL_SIZE;
	user_config->extra_size = SSAM_DEFAULT_MEMPOOL_EXTRA_SIZE;
	g_ssam_config.virtio_fs_enable = false;
}

static void
ssam_virtio_exit(void)
{
	int rc;
	int i;

	rc = ssam_lib_exit();
	if (rc != 0) {
		SPDK_WARNLOG("ssam lib exit failed\n");
	}

	for (i = 0; i < SSAM_HOSTEP_NUM_MAX; i++) {
		if (g_ssam_fs_config_map[i].mp != NULL) {
			ssam_mempool_destroy(g_ssam_fs_config_map[i].mp);
			g_ssam_fs_config_map[i].mp = NULL;
			ssam_update_virtio_device_used(i, 0);
		}
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
