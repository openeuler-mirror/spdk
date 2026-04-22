/*
 *   BSD LICENSE
 *   Copyright (c) Huawei Technologies Co., Ltd. 2021-2021. All rights reserved.
 *
 */

#ifndef DPAK_SSAM_H
#define DPAK_SSAM_H

#include "spdk/stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SSAM_HOSTEP_NUM_MAX         32
#define SSAM_MAX_REQ_POLL_SIZE      16
#define SSAM_MAX_RESP_POLL_SIZE     10
#define SSAM_VIRTIO_HEAD_LEN        64
#define SSAM_DEV_CFG_MAX_LEN        60
#define SSAM_DBDF_STR_MAX_LEN       13
#define SSAM_MB                     (uint64_t)(1 << 20)
#define SSAM_SERVER_NAME            "ssam"

enum ssam_device_type {
	SSAM_DEVICE_NVME = 0,           /* NVMe device */
	SSAM_DEVICE_VIRTIO_BLK = 2,     /* virtio-blk device */
	SSAM_DEVICE_VIRTIO_SCSI = 3,    /* virtio-scsi device */
	SSAM_DEVICE_VIRTIO_FS = 5,      /* virtio-fs device */
	SSAM_DEVICE_VIRTIO_MAX = 6      /* virtio device type upper boundary */
};

enum ssam_mount_type {
	SSAM_MOUNT_DUMMY = 0,           /* mount virtio to dummy function */
	SSAM_MOUNT_NORMAL               /* mount virtio to normal function */
};

enum ssam_function_mount_status {
	SSAM_MOUNT_OK,                  /* mount ok */
	SSAM_MOUNT_VOLUME_NOT_FOUND,    /* mount volume not found */
	SSAM_MOUNT_PARAMETERS_ERROR,    /* mount parameter error */
	SSAM_MOUNT_UNKNOWN_ERROR        /* unknow error */
};

enum ssam_io_type {
	SSAM_VIRTIO_BLK_IO = 2,         /* virtio-blk IO */
	SSAM_VIRTIO_SCSI_IO,            /* virtio-scsi normal IO */
	SSAM_VIRTIO_SCSI_CTRL,          /* virtio-scsi control IO */
	SSAM_VIRTIO_SCSI_EVT,           /* virtio-scsi event IO */
	SSAM_VIRTIO_VSOCK_IO,           /* virtio-vsock IO */
	SSAM_VIRTIO_VSOCK_EVT,          /* virtio-vsock event */
	SSAM_VIRTIO_FUNC_STATUS,        /* virtio function status change */
	SSAM_VIRTIO_FS_IO,              /* virtio-fs normal IO */
	SSAM_VIRTIO_FS_HIPRI,           /* virtio-fs high priority IO */
	SSAM_VIRTIO_TYPE_RSVD,          /* virtio type rsvd */
};

enum ssam_io_status {
	SSAM_IO_STATUS_OK,              /* ok */
	SSAM_IO_STATUS_EMPTY,           /* poll return empty */
	SSAM_IO_STATUS_ERROR            /* error */
};

enum ssam_function_action {
	SSAM_FUNCTION_ACTION_START,         /* start */
	SSAM_FUNCTION_ACTION_STOP,          /* stop */
	SSAM_FUNCTION_ACTION_RESET,         /* reset */
	SSAM_FUNCTION_ACTION_CONFIG_CHANGE, /* config change report */
	SSAM_FUNCTION_ACTION_SCSI_EVENT,    /* SCSI event report */
	SSAM_FUNCTION_ACTION_MAX
};

enum ssam_function_status {
	SSAM_FUNCTION_STATUS_START,     /* start */
	SSAM_FUNCTION_STATUS_STOP,      /* stop */
	SSAM_FUNCTION_EVENT_MIGRATE     /* migrate */
};

enum data_request_dma_type {
	SSAM_REQUEST_DATA_LOAD = 0,     /* load data from host->CPU DDR */
	SSAM_REQUEST_DATA_STORE = 1,    /* store data frome CPU DDR->host */
	SSAM_REQUEST_DATA_MAX
};

struct ssam_melem {
	void *addr;                     /* virtual address */
	uint64_t iova;                  /* IO address */
	uint64_t page_sz;               /* page size of underlying memory */
	int socket_id;                  /* NUMA socket ID */
	int rsvd;
};

enum ssam_blk_hash_mode {
	SSAM_PF_HASH_MODE = 0,
	SSAM_VQ_HASH_MODE,
	SSAM_IO_HASH_MODE,
};

struct ssam_lib_args {
	uint8_t role;                   /* reserved */
	uint8_t core_num;               /* core num that polled by SPDK thread */
	uint8_t dma_queue_num;          /* host dma queue num per channel */
	uint8_t hash_mode;              /* hash mode: BLK:0-1bits SCSI:2-3bits FS:4-5bits NVMe:6-7bits */
	uint8_t rsvd[32];               /* for rsvd */
	/* register DPDK function rte_malloc_heap_alloc */
	int (*ssam_heap_malloc)(const char *type, size_t size,
				int socket_arg, unsigned int flags, size_t align,
				size_t bound, bool contig, struct ssam_melem *mem);
	int (*ssam_heap_free)(void *addr); /* register DPDK function rte_malloc_heap_free */
};

struct ssam_pf_list {
	uint16_t pf_funcid;             /* pf_funcid = -1 means invalid */
	uint16_t pf_type;               /* refer to enum ssam_device_type */
	uint16_t vf_funcid_start;       /* the start function id of vf */
	uint16_t vf_num;                /* the number of vf that have been configured */
	uint16_t vf_max;                /* the max number of vf that can be configured */
};

/* the host side all pf/vf end point info */
struct ssam_hostep_info {
	struct ssam_pf_list host_pf_list[SSAM_HOSTEP_NUM_MAX];
};

struct ssam_virtio_config {
	uint64_t device_feature;        /* the virtio device feature */
	uint16_t queue_num;             /* the queue number of virtio device */
	uint16_t config_len;            /* the actual length of device_config */
	uint8_t device_config[SSAM_DEV_CFG_MAX_LEN];    /* the virtio device configure */
	uint16_t queue_size;
	uint16_t rx_queue_id;
};

/* ssam function config */
struct ssam_function_config {
	int gfunc_id;                   /* pf or vf funcion id */
	enum ssam_device_type type;     /* pf or vf type */
	struct ssam_virtio_config virtio_config; /* pf or vf configure */
};

struct ssam_virt_request {
	uint16_t vq_idx;
	uint16_t req_idx;
};

struct ssam_nvme_request {
	void *data;
};

struct ssam_io_message {
	uint32_t header_len;                   /* io header length */
	uint8_t header[SSAM_VIRTIO_HEAD_LEN];  /* refer to struct virtio_blk_outhdr */
	uint32_t iovcnt;                       /* io vector count */
	struct iovec *iovs;                    /* io vectors, max 1MB IO */
	uint8_t writable;                      /* 0 : write io, 1 : read io */
	uint8_t rsvd[3];                       /* for byte alignment */
	union {
		struct ssam_virt_request virtio;
		struct ssam_nvme_request nvme;
	};
};

/**
 * @brief function event structure
 */
struct ssam_func_event {
	enum ssam_function_status status; /* function status */
	uint32_t data;                    /* virtio version: 0--v0.95 1--v1.0 2--v1.1 */
};

struct ssam_request {
	uint16_t gfunc_id;              /* function id vf id number */
	uint16_t rsvd;
	uint32_t iocb_id;               /* response need */
	enum ssam_io_type type;
	union {
		struct ssam_io_message cmd;     /* VMIO command structure */
		struct ssam_func_event event;   /* report function event */
	} req;
	enum ssam_io_status status;     /* request status */
	uint32_t flr_seq;               /* response need */
};

struct ssam_request_poll_opt {
	struct iovec
		*sge1_iov; /**< output for req->req.cmd.iovs[1] (per VMIO req). Actual data length set in iov_len */
	uint16_t queue_id;      /**< (optional) poll a queue id instead of using 'tid' parameter to calculate the queue */
	uint8_t rsvd[54];
};

struct ssam_virtio_res {
	struct iovec *iovs;             /* rsp io vectors */
	void *rsp;                      /* data of rsp */
	uint32_t rsp_len;               /* length of rsp */
	uint32_t iovcnt;                /* rsp vector count */
};

struct ssam_io_response {
	uint16_t gfunc_id;              /* global function id in chip */
	uint16_t rsvd;
	uint32_t iocb_id;               /* copy from struct ssam_request */
	struct ssam_virtio_res data;
	struct ssam_request *req;       /* corresponding to struct vmio_request */
	enum ssam_io_status status;     /* IO status, copy from struct ssam_request */
	uint32_t flr_seq;               /* copy from struct ssam_request */
};

struct ssam_dma_request {
	uint16_t gfunc_id;
	uint16_t direction;
	uint32_t flr_seq;
	uint32_t src_num;               /* source sge number */
	uint32_t dst_num;               /* dest sge number */
	struct iovec *src;              /* source buffer address, gpa mode */
	struct iovec *dst;              /* dest buffer address, va mode */
	uint32_t data_len;
	void *cb;
};

struct ssam_dma_rsp {
	void *cb;
	uint32_t status;             /* process status, 0--OK, 1--ERR */
	uint32_t last_flag;           /* data copy finish until receive this last flag */
};

struct memory_info_stats {
	size_t total_size;         /* Total bytes of mempool */
	size_t free_size;          /* Total free bytes of mempool */
	size_t greatest_free_size; /* Size in bytes of largest free block */
	unsigned free_count;       /* Number of free elements of mempool */
	unsigned alloc_count;      /* Number of allocated elements of mempool */
	size_t used_size;          /* Total allocated bytes of mempool */
};

/**
 * Init ssam lib, set ssam work mode, set core num, set functions, get host pf/vf endpoint info.
 *
 * \param args_in input work mode, core num, functions.
 * \param eps_out output host pf/vf endpoint info.
 *
 * \return 0 for succeed or not 0 for failed
 */
int ssam_lib_init(struct ssam_lib_args *args_in, struct ssam_hostep_info *eps_out);

/**
 * Exit ssam lib when not use ssam any more.
 *
 * \return 0 for succeed or not 0 for failed
 */
int ssam_lib_exit(void);

typedef void ssam_mempool_t;

/**
 * Create the memory pool, the memory is allocated by spdk_dma_malloc.
 *
 * \param size the memory pool size.
 * \param extra_size_limit the memory size that can alloc in addition to the memory pool
 *
 * \return a pointer to memory pool when succeed or null when failed
 */
ssam_mempool_t *ssam_mempool_create(uint64_t size, uint64_t extra_size_limit);

/**
 * Allocate one piece of memory from the memory pool.
 *
 * \param mp the memory pool.
 * \param size the memory size that want to allocate.
 * \param phys_addr save the physical address of the allocated memory,
 * if allocate failed, will not change the value.
 *
 * \return the allocated memory's start virtual address when succeed or null when failed
 */
void *ssam_mempool_alloc(ssam_mempool_t *mp, uint64_t size, uint64_t *phys_addr);

/**
 * Free the memory back to the memory pool.
 *
 * \param mp the memory pool.
 * \param ptr the memory virtual address that return by ssam_mempool_alloc.
 */
void ssam_mempool_free(ssam_mempool_t *mp, void *ptr);

/**
 * Destroy the memory pool, when this done, the memory pool cannot be used again.
 *
 * \param mp the memory pool.
 */
void ssam_mempool_destroy(ssam_mempool_t *mp);

/**
 * get the memory pool info status.
 *
 * \param mp the memory pool.
 * \param info the mempool info status.
 */
int ssam_get_mempool_info(ssam_mempool_t *mp, struct memory_info_stats *info);

/**
 * ssam recover module preinit.
 *
 * \return 0 for succeed, 1 for config file exist, and less then 0 for failed.
 */
int spdk_ssam_rc_preinit(void);

/**
 * Get recover json file path.
 *
 * \return a file path string
 */
char *ssam_rc_get_recover_json_file_path(void);

/**
 * Get parameter json file path.
 *
 * \return a file path string
 */
char *ssam_rc_get_param_json_file_path(void);

/**
 * Initialize PF (include all VFs belong to this PF) to specific device type.
 * The interface must be called with increasing pf_id. The function is not
 * visible to host after init.
 *
 * \param pf_id PF function id.
 * \param num_vf number of VFs of the PF.
 * \param dev_type PF/VF type.
 *
 * \return 0 for succeed or not 0 for failed
 */
int ssam_setup_function(uint16_t pf_id, uint16_t num_vf, enum ssam_device_type dev_type);

/**
 * Change specific device config.
 *
 * \param cfg new device configuration data.
 *
 * \return 0 for succeed or not 0 for failed
 */
int ssam_write_function_config(struct ssam_function_config *cfg);

/**
 * send action to function. Invoked by SPDK.
 *
 * \param gfunc_id the global function index of the chip
 * \param action the action to take on the function
 * \param data extra action data if used
 * \param data_len extra action data len
 *
 * \return 0 for succeed or not 0 for failed
 */
int ssam_send_action(uint16_t gfunc_id, enum ssam_function_action action, const void *data,
		     uint16_t data_len);

/**
 * Mount ssam volume, synchronous interface.
 *
 * \param gfunc_id the global function id of chip.
 * \param lun_id the lun id of this volume.
 * \param type mount type, refer to enum ssam_mount_type.
 * \param tid it's used as the request queue id per CPU core.
 *
 * \return 0 for succeed or not 0 for failed
 */
int ssam_function_mount(uint16_t gfunc_id, uint32_t lun_id, enum ssam_mount_type type,
			uint16_t tid);

/**
 * Umount ssam volume, synchronous interface.
 *
 * \param gfunc_id the global function id of chip.
 * \param lun_id the lun id of this volume.
 *
 * \return refer to enum ssam_function_mount_status
 */
int ssam_function_umount(uint16_t gfunc_id, uint32_t lun_id);

/**
 * Poll request queue for ssam request.
 *
 * \param tid it's used as the request queue id per CPU core.
 * \param poll_num the number of ssam request that want to be polled.
 * \param io_req output for received request, the buffer is allocated by ssam,
 * and released when IO complete.
 *
 * \return  the number of vmio has been polled, less than 0 or bigger than poll_num for failed
 */
int ssam_request_poll(uint16_t tid, uint16_t poll_num, struct ssam_request **io_req);

/**
 * Poll request queue for ssam request.
 *
 * \param tid it's used as the request queue id per CPU core.
 * \param poll_num the number of ssam request that want to be polled.
 * \param io_req output for received request, the buffer is allocated by ssam,
 * and released when IO complete.
 * \param poll_opt (optional) extra poll options.
 *
 * \return  the number of vmio has been polled, less than 0 or bigger than poll_num for failed
 */
int ssam_request_poll_ext(uint16_t tid, uint16_t poll_num, struct ssam_request **io_req,
			  struct ssam_request_poll_opt *poll_opt);

/**
 * Request ssam data. Hardware will load or store data betweent host and CPU.
 * Asynchronous interface.
 *
 * \param tid it's used as the request queue id per CPU core.
 * \param dma_req request data is here.
 *
 * \return 0 for succeed or not 0 for failed
 */
int ssam_dma_data_request(uint16_t tid, struct ssam_dma_request *dma_req);

/**
 * Poll ssam request data.
 *
 * \param tid it's used as the request queue id per CPU core.
 * \param poll_num the number of ssam request that want to be polled.
 * \param dma_rsp response data is here.
 *
 * \return the number of msg rsp has been polled, less than 0 or bigger than poll_num for failed
 */
int ssam_dma_rsp_poll(uint16_t tid, uint16_t poll_num, struct ssam_dma_rsp *dma_rsp);

/**
 * Send IO complete info to ssam request queue.
 *
 * \param tid it's used as the request queue id per CPU core.
 * \param resp response info is here.
 *
 * \return 0 for succeed or not 0 for failed
 */
int ssam_io_complete(uint16_t tid, struct ssam_io_response *resp);

/**
 * Create vmio rx queue
 *
 * \param queue_id_out id of the queue create
 *
 * \return 0 for succeed or not 0 for failed
 */
int ssam_vmio_rxq_create(uint16_t *queue_id_out);

/**
 * Update virtio device used or not.
 *
 * \param glb_function_id the global function index of the chip
 * \param device_used virtio device is used or not
 *
 * \return 0: success -1: fail, internal error, others: fail, refer to errno.h
 */
int ssam_update_virtio_device_used(uint16_t glb_function_id, uint64_t device_used);

/**
 * release virtio blk vq resource.
 *
 * \param glb_function_id the global function index of the chip, the related function is virtio_blk
 *
 * \return 0: success -1: fail, internal error, others: fail, refer to errno.h
 */
int ssam_virtio_blk_release_resource(uint16_t glb_function_id);

/**
 * alloc virtio blk vq resource.
 *
 * \param glb_function_id the global function index of the chip, the related function is virtio_blk
 * \param queue_num number of vq
 *
 * \return 0: success -1: fail, internal error, others: fail, refer to errno.h
 */
int ssam_virtio_blk_alloc_resource(uint16_t glb_function_id, uint16_t queue_num);

/**
 * Update virtio blk capacity.
 *
 * \param gfunc_id the global function index of the chip.
 * \param capacity the new capacity.
 *
 * \return 0 for succeed or not 0 for failed
 */
int ssam_virtio_blk_resize(uint16_t gfunc_id, uint64_t capacity);

/**
 * Get global function id by dbdf.
 *
 * \param dbdf the combine of domain bus device function.
 * \param gfunc_id the global function index of the chip.
 *
 * \return 0 for succeed or not 0 for failed
 */
int ssam_get_funcid_by_dbdf(uint32_t dbdf, uint16_t *gfunc_id);

/**
 * Convert dbdf from string format to number.
 *
 * \param str source dbdf string.
 * \param dbdf store result.
 *
 * \return 0 for succeed or not 0 for failed
 */
int ssam_dbdf_str2num(char *str, uint32_t *dbdf);

/**
 * Convert dbdf from number format to string.
 *
 * \param dbdf source dbdf number.
 * \param str store result.
 * \param len the str buffer length.
 *
 * \return 0 for succeed or not 0 for failed
 */
int ssam_dbdf_num2str(uint32_t dbdf, char *str, size_t len);

/**
 * @brief check device ready
 * @param role 0--old process; 1--new process
 * @param proc_type enum proc_type, supoort PROC_TYPE_VBS and PROC_TYPE_BOOT
 * @param ready output_para 0--not ready, 1--ready
 * @return
 *   - 0: success
 *   - <0: fail, refer to errno.h
 */
int ssam_check_device_ready(uint8_t role, uint32_t proc_type, uint8_t *ready);

/**
 * @brief get hot upgrade state
 * @param void
 * @return
 *   - 0:  success
 *   - -1: fail, internal error
 *   - others: fail, refer to errno.h
 */
int ssam_get_hot_upgrade_state(void);

/**
 * @brief sync PF/VF config info to the hpd process in host
 * @param void
 * @return void
 */
void ssam_hotplug_cfg(void);

/**
 * @brief hot insert device interface
 * @param prot_id the number of PF to add
 * @return
 *   - 0:  success
 *   - others: fail, refer to errno.h
 */
int ssam_hotplug_add(uint16_t port_id);

/**
 * @brief hot remove device interface
 * @param prot_id the number of PF to remove
 * @return
 *   - 0:  success
 *   - others: fail, refer to errno.h
 */
int ssam_hotplug_del(uint16_t port_id);

/**
 * @brief get hot upgrade state
 * @param void
 * @return
 *   - 0:  HPD enable
 *   - 1:  HPD disable
 */
bool ssam_hotplug_enable_check(void);


#ifdef __cplusplus
}
#endif

#endif /* DPAK_SSAM_H */
