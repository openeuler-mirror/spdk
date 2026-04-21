/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021-2025 Huawei Technologies Co.
 *   All rights reserved.
 */

#ifndef HIVIO_API_H
#define HIVIO_API_H

#include "spdk/stdinc.h"

#define MEM_ALLOC_SGE_NUM_MAX 512

/**
 * @brief memory descriptor for hvio_mem_alloc.
 */
typedef struct mem_desc {
	uint32_t size; /* *< mem array size */
	struct {
		uint64_t virt; /* *< virtual address */
		uint64_t phys; /* *< physical address */
		uint32_t len;  /* *< length */
	} mem[MEM_ALLOC_SGE_NUM_MAX];
} mem_desc_s;

/**
 * @brief memory descriptor for hvio_heap_malloc.
 */
struct hvio_melem {
	void *addr;       /**< virtual address */
	uint64_t iova;    /**< IO address */
	uint64_t page_sz; /**< page size of underlying memory */
	int socket_id;    /**< NUMA socket ID */
	int rsvd;
};

/**
 * @brief memory-related callbacks.
 */
typedef struct hvio_callback_ops {
	int (*hvio_heap_malloc)(const char *type, size_t size, int socket_arg, unsigned int flags,
				size_t align, size_t bound, bool contig,
				struct hvio_melem *mem); /* register rte_malloc_heap_alloc */
	int (*hvio_heap_free)(void *addr); /* register rte_malloc_heap_free */
	int (*hvio_mem_alloc)(uint32_t size, int phy_contig,
			      mem_desc_s *mem_desc); /* register dma_mem_alloc function */
	int (*hvio_mem_free)(void *virt);   /* register dma_mem_free function */
} hvio_callback_ops_s;

/**
 * @brief proc type definition
 */
enum proc_type {
	PROC_TYPE_VBS = 0,
	PROC_TYPE_BOOT,
	PROC_TYPE_MIGTORBO,
	PROC_TYPE_MAX
};

enum hivio_blk_hash_mode {
	HVIO_PF_HASH_MODE = 0,
	HVIO_VQ_HASH_MODE,
	HVIO_IO_HASH_MODE,
};

/**
 * @brief hivio_lib initialize parameters
 */
typedef struct hvio_lib_args {
	uint8_t role;                    /**< 0--old process; 1--new process */
	uint8_t core_num;                /**< core num that polled by SPDK thread */
	hvio_callback_ops_s cb_ops;      /**< memory-related callbacks  */
	uint32_t proc_type;              /**< enum proc_type */
	uint8_t host_dma_chnl_num;       /**< host dma channel number, used for migtorbo multi chan process */
	uint8_t host_dma_mp_per_chnl;    /**< host dma mempool per channel, 0: disable mp per channel, 1: enable */
	uint8_t host_dma_queue_per_chnl; /**< host dma queue num per channel, 0: disabled-defalt 1, max: 4 */
	uint8_t hash_mode;               /**< HASH MODE: BLK:0-1bits SCSI:2-3bits FS:4-5bits NVMe:6-7bits */
	uint8_t rsvd[56];                /**< for rsvd */
} hvio_lib_args_s;

#define HVIO_HOSTEP_NUM_MAX 32

/**
 * @brief host side storage pf/vf end point info
 */
typedef struct hvio_hostep_info {
	struct {
		uint16_t pf_funcid; /* *< pf_funcid = 0xffff means invalid */
		uint16_t pf_type;   /* *< is config or not */
		uint16_t vf_funcid_start;
		uint16_t vf_num;    /* *< already config vf num */
		uint16_t vf_max;    /* *< max num can be config */
	} host_pf_list[HVIO_HOSTEP_NUM_MAX];
} hvio_hostep_info_s;

/**
 * @brief device type definition
 */
enum device_type {
	DEVICE_NVME,         /* *< NVMe device */
	DEVICE_VIRTIO_NET,   /* *< VirtIO-net device */
	DEVICE_VIRTIO_BLK,   /* *< VirtIO-blk device */
	DEVICE_VIRTIO_SCSI,  /* *< VirtIO-scsi device */
	DEVICE_VIRTIO_VSOCK, /* *< VirtIO-vsock device */
	DEVICE_VIRTIO_FS,    /**< VirtIO-FS device */
	DEVICE_VIRTIO_MAX    /* *< VirtIO-max device */
};

/**
 * @brief configration type definition
 */

struct function_config {
	uint32_t function_id;
	enum device_type type;
	union {
		struct {
			uint64_t device_feature;
			uint16_t queue_num;
			uint16_t config_len;
			uint8_t device_config[60];
			uint16_t queue_size;
			uint16_t rx_queue_id;
		} virtio;
	} config;
};

/**
 * @brief EP operation definition.
 */
enum function_action {
	FUNCTION_ACTION_START,         /* *< start */
	FUNCTION_ACTION_STOP,          /* *< stop */
	FUNCTION_ACTION_RESET,         /* *< reset */
	FUNCTION_ACTION_CONFIG_CHANGE, /* *< config change report */
	FUNCTION_ACTION_SCSI_EVENT,    /* *< SCSI event report */
	FUNCTION_ACTION_MAX
};

/**
 * @brief EP function status definition.
 */
enum function_status {
	FUNCTION_STATUS_START,  /* *< start */
	FUNCTION_STATUS_STOP,   /* *< stop */
	FUNCTION_EVENT_MIGRATE, /* *< migrate */
};

/**
 * @brief VMIO type definition, support nvme and virtio.
 */
enum vmio_type {
	VMIO_TYPE_NVME_IO,            /* *< NVMe normal IO */
	VMIO_TYPE_NVME_ADMIN,         /* *< NVMe admin IO */
	VMIO_TYPE_VIRTIO_BLK_IO,      /* *< VirtIO blk IO */
	VMIO_TYPE_VIRTIO_SCSI_IO,     /* *< VirtIO scsi normal IO */
	VMIO_TYPE_VIRTIO_SCSI_CTRL,   /* *< VirtIO scsi IO */
	VMIO_TYPE_VIRTIO_SCSI_EVT,    /* *< VirtIO scsi event */
	VMIO_TYPE_VIRTIO_VSOCK_IO,    /* *< VirtIO vsock IO */
	VMIO_TYPE_VIRTIO_VSOCK_EVT,   /* *< VirtIO vsock event */
	VMIO_TYPE_VIRTIO_FUNC_STATUS, /* *< VirtIO function status change */
	VMIO_TYPE_VIRTIO_FS_IO,       /* *< VirtIO fs normal IO */
	VMIO_TYPE_VIRTIO_FS_HIPRI,    /* *< VirtIO fs high priority IO */
	VMIO_TYPE_RSVD,               /* *< VMIO type rsvd */
};

struct virtio_req {
	uint16_t vq_idx;  /* *< vq idx */
	uint16_t req_idx; /* *< head desc idx of io */
};

struct nvme_req {
	void *data; /* *< nvme admin input data */
};

/**
 * @brief VMIO cmd structure.
 */
struct vmio_cmd {
	uint32_t cmd_len; /* *< length of VMIO command, fixed to 64B */
	uint8_t cmd[64];  /* *< the specific format according to vmio_type */

	uint32_t iovcnt;    /* *< io vector count */
	struct iovec *iovs; /* *< io vectors, max 1MB IO */
	uint8_t writable;   /* *< 2nd desc->write_flag */
	uint8_t rsvd[3];    /* *< rsvd */
	union {
		struct virtio_req virtio;
		struct nvme_req nvme;
	};
};

/**
 * @brief function event structure.
 */
struct func_event {
	enum function_status status; /* *< function status */
	uint32_t data;               /* *< VirtIO version: 0--v0.95; 1--v1.0; 2--v1.1 */
};

/**
 * @brief VMIO status definition.
 */
enum vmio_status {
	VMIO_STATUS_OK, /* *< ok */
	VMIO_STATUS_VQ_EMPTY, /* *< VQ empty */
	VMIO_STATUS_ERROR, /* *< error */
	VMIO_STATUS_DRIVER_NOT_OK, /* *< frontend driver not ready */
	VMIO_STATUS_VQ_ENGN_NOT_EN, /* *< backend vq not ready */
	VMIO_STATUS_DMA_IO_ERROR, /* *< frontend dma access error */
	VMIO_STATUS_VQ_SOURCE_ERROR, /* *< VQ cache source error */
	VMIO_STATUS_VQ_ERROR /* *< frontend vq status error */
};

/**
 * @brief VMIO request structure.
 */
struct vmio_request {
	uint16_t glb_function_id; /* *< global function id in chip */
	uint16_t nvme_sq_id;      /* *< sq_id in iocb for NVMe vmio */
	uint32_t iocb_id;         /* *< io control block id for ucode */
	enum vmio_type type;      /* *< VMIO type to parse the req format */
	union {
		struct vmio_cmd cmd;     /* *< VMIO command structure */
		struct func_event event; /* *< report function event */
	} req;
	enum vmio_status status; /* *< when flr occurs, set status to error */
	uint32_t flr_seq;        /* *< check whether VMIO is from VF which FLR occurs */
};

typedef struct tag_nvme_cqe {
	uint32_t cmd_spec;
	uint32_t rsvd;

	uint32_t sq_hd : 16;
	uint32_t sq_id : 16;

	uint32_t cmd_id : 16;
	uint32_t p : 1;
	uint32_t status : 15;
} nvme_cqe_s;

/**
 * @brief NVMe response structure
 */
struct nvme_response {
	nvme_cqe_s nvme_cqe;

	uint32_t rsp_len;   /* *< rsp length */
	uint32_t iovcnt;    /* *< rsp vector count */
	struct iovec *iovs; /* *< rsp io vectors */
	void *rsp;          /* *< rsp data */
};

/**
 * @brief VirtIO response structure
 */
struct virtio_response {
	uint32_t used_len;  /* *< length of data has been upload to VM */
	uint32_t rsp_len;   /* *< length of rsp */
	uint32_t iovcnt;    /* *< rsp vector count */
	struct iovec *iovs; /* *< rsp io vectors */
	void *rsp;          /* *< data of rsp */
};

/**
 * @brief VMIO response structure
 */
struct vmio_response {
	uint16_t glb_function_id; /* *< global function id in chip */
	uint16_t rsvd0;           /* *< make sure nvme and virtio offset is 16B aligned */
	uint32_t iocb_id;         /* *< io control block id used by ucode */
	enum vmio_type type;      /* *< VMIO type */
	uint32_t rsvd1;           /* make sure nvme and virtio offset is 16B aligned */

	union {
		struct nvme_response nvme;     /* *< nvme rsp structure */
		struct virtio_response virtio; /* *< virtio rsp structure */
	};

	struct vmio_request *req; /* *< corresponding vmio_request */
	enum vmio_status status;  /* *< VMIO status, copy from vmio_request */
	uint32_t flr_seq;         /* *< copy from vmio_request */
};

/**
 * @brief data structrue for send action request.
 */
typedef struct hvio_send_action_req {
	uint16_t glb_function_id;    /**< global function id in chip */
	uint16_t data_len;           /**< length of request's payload */
	void *data;                  /**< request's payload */
	enum function_action action; /**< action type */
} hvio_send_action_req_s;

/**
 * @brief data structrue for VMIO send request(destination is virtio RQ).
 */
typedef struct hvio_vmio_send_req {
	uint64_t cb;              /**< callback info */
	uint16_t glb_function_id; /**< global function id in chip */
	uint16_t vqn;             /**< function inner vq idx */
	uint32_t sge_num;         /**< data sge number */
	struct iovec *data;       /**< data buffer address, gpa mode, including virtio_hdr and payload */
	uint32_t data_len;        /**< data len, including virtio_hdr len and payload len. */
	enum vmio_type type;      /**< vmio type */
} hvio_vmio_send_req_s;

/**
 * @brief data structrue for ACK of VMIO send request(destination is virtio RQ).
 */
typedef struct hvio_vmio_send_rsp {
	uint64_t cb;              /**< callback info */
	uint32_t status;          /**< refer to enum vmio_status */
} hvio_vmio_send_rsp_s;

/**
 * @brief data structrue for rsp of vsock recovery.
 */
typedef struct hvio_vsock_recovery_rsp {
	uint16_t tx_used_idx; /* *< virtio vsock txq used idx */
	uint16_t rx_used_idx; /* *< virtio vsock rxq used idx */
} hvio_vsock_recovery_rsp_s;

/**
 * @brief host_dma direction.
 */
enum hvio_host_dma_mode {
	READ_HOST_MODE = 0,     /**< read host data and write to SPU */
	WRITE_HOST_MODE = 1,    /**< write data to host */
	HOST_DMA_MODE_MAX
};

/**
 * @brief data structrue for host dma request.
 */
typedef struct hvio_host_dma_req {
	uint16_t glb_function_id; /**< VM global function id */
	uint16_t direction;       /**< host dma direction, format is enum hvio_host_dma_mode */
	uint32_t flr_seq;         /**< check whether the vmio copy request is a leaked request when flr occurs */
	uint32_t ssge_num;        /**< source sge number */
	uint32_t dsge_num;        /**< dest sge number */
	struct iovec *src;        /**< source buffer address, gpa. host buf for read, ddr for write. */
	struct iovec *dst;        /**< dest buffer address, gpa. ddr for read, host buf for write */
	uint32_t data_len;        /**< length for load or store */
	void *cb;                /**< callback info */
} hvio_host_dma_req_s;

/**
 * @brief data structrue for ACK of host dma request.
 */
typedef struct hvio_host_dma_rsp {
	void *cb;             /**< SPDK callback info */
	uint32_t status;      /**< 0 OK, 1 ERROR */
	uint32_t last_flag;
} hvio_host_dma_rsp_s;

/**
 * @brief data structrue for hivio stats.
 */

typedef struct hvio_info_stats {
	uint64_t vmio_req;
	uint64_t vmio_rsp;

	uint64_t vsock_tx_req;
	uint64_t vscok_tx_rsp;
	uint64_t vsock_rx_req;
	uint64_t vsock_rx_rsp;

	uint64_t host_dma_req;
	uint64_t host_dma_sub_req;
	uint64_t host_dma_rsp;

	uint64_t update_blk_cap;
	uint64_t send_action;

	uint64_t rsvd[16];
} hvio_info_stats_s;

typedef struct hvio_warn_stats {
	uint64_t invalid_vmio;
	uint64_t vsock_rx_rsp_status_abnormal;
	uint64_t host_dma_rsp_status_abnormal;

	uint64_t rsvd[16];
} hvio_warn_stats_s;

typedef struct hvio_error_stats {
	uint64_t update_blk_cap_fail;
	uint64_t send_action_fail;
	uint64_t vmio_rsp_fail;
	uint64_t vsock_tx_fail;
	uint64_t vsock_rx_fail;
	uint64_t host_dma_req_fail;

	uint64_t rsvd[16];
} hvio_error_stats_s;

typedef struct hivio_func_ctx_read_rsp {
	uint8_t device_type;
	uint8_t device_status;
	uint16_t num_queues;
	uint8_t flr_status;
	uint8_t rsvd0[3];
	uint32_t device_feature_l;
	uint32_t device_feature_h;
	uint32_t driver_feature_l;
	uint32_t driver_feature_h;
	uint32_t rsvd1[26];
} hivio_func_ctx_read_rsp_s;

struct hvio_mount_para {
	uint32_t algo_type; /* *< VBS:algorithm 0 or 1; IPU:0--dummy; 1--normal */
	uint32_t key[3];    /* *< 0 for rsvd. VBS:key[0] tree_id, key[1] pt_num, key[2] blk_size */
};

/**
 * @brief hivio initialization function
 * @param args_in initialization parameters input
 * @param eps_out host side ep info ouput
 * @return
 *   - 0: success
 *   - -1: fail, internal error
 *   - others: fail, refer to errno.h
 */
int hvio_lib_init(hvio_lib_args_s *args_in, hvio_hostep_info_s *eps_out);

/**
 * @brief hivbs de-initialize function.
 * @param void
 * @return
 *   - 0: success
 */
int hvio_lib_deinit(void);

/**
 * @brief update virtio blk capacity.
 * @param glb_function_id the global function index of the chip
 * @param capacity new capacity
 * @return
 *   - 0: success
 *   - -1: fail, internal error
 *   - others: fail, refer to errno.h
 */
int hvio_update_virtio_blk_capacity(uint16_t glb_function_id, uint64_t capacity);

/**
 * @brief poll RQ for VMIO request.
 * @param tid It's used as the L2NIC RQ id per SPU core.
 * @param poll_num the number of msg rsp want to be polled
 * @param req output for received request. The buffer is allocated by hivbs, and used by SPDK. Release when IO complete.
 * @return
 *   - >=0: the number of vmio_request has been polled
 *   - <0: fail, refer to errno.h
 */
int hvio_vmio_req_poll_batch(uint16_t tid, uint16_t poll_num, struct vmio_request **req);

/**
 * @brief hvio_vmio_req_poll_batch_ext extra poll options
 */
typedef struct hvio_vmio_req_poll_opt {
	struct iovec
		*sge1_iov; /**< output for req->req.cmd.iovs[1] (per VMIO req). Actual data length set in iov_len */
	uint16_t queue_id;      /**< (optional) poll a queue id instead of using 'tid' parameter to calculate the queue */
	uint8_t rsvd[54];
} hvio_vmio_req_poll_opt_s;

/**
 * @brief poll RQ for VMIO request, together with the contents of req->req.cmd.iovs[1].
 * @param tid It's used as the L2NIC RQ id per SPU core.
 * @param poll_num the number of msg rsp want to be polled, if the poll_num > 16, the actual poll num is 16.
 * @param req output for received request. The buffer is allocated by hivbs, and used by SPDK. Release when IO complete.
 * @param poll_opt (optional) extra poll options.
 * @return
 *   - >=0: the number of vmio_request has been polled
 *   - <0: fail, refer to errno.h
 * @note req->req.cmd.writable will be used to specify the first writable index in req->req.cmd.iovs.
 */
int hvio_vmio_req_poll_batch_ext(uint16_t tid, uint16_t poll_num, struct vmio_request **req,
				 hvio_vmio_req_poll_opt_s *poll_opt);

/**
 * @brief send VMIO complete to SQ.
 * @param tid It's used as the L2NIC SQ id per SPU core.
 * @param resp VMIO response
 * @return
 *   - 0: success
 *   - others: fail, refer to errno.h
 */
int hvio_vmio_complete(uint16_t tid, struct vmio_response *resp);

/**
 * @brief create vmio rx queue
 * @param queue_id_out id of the queue create
 * @return
 *   - 0: success
 *   - -1: fail, internal error
 *   - others: fail, refer to errno.h
 */
int hvio_vmio_rxq_create(uint16_t *queue_id_out);

/**
 * @brief initialize PF (include all VFs belong to this PF) to specific device type. For virtio device of the PF and VF
 * can be set to different virtio device_type. The interface must be called with increasing pf_id. The function is not
 * visible to host after init.
 * @param pf_id PF id
 * @param num_vf number of VFs of the PF, they use the same type
 * @param pf_type pf device type
 * @param vf_type vf device type
 * @return
 *   - 0: success
 *   - -1: fail, internal error
 *   - others: fail, refer to errno.h
 */
int hvio_setup_function(uint16_t pf_id, uint16_t num_vf, enum device_type pf_type,
			enum device_type vf_type);

/**
 * @brief change specific device config.
 * @param cfg new device configuration data
 * @return
 *   - 0: success
 *   - -1: fail, internal error
 *   - others: fail, refer to errno.h
 */
int hvio_write_function_config(struct function_config *cfg);

/**
 * @brief get global function index by pcie device dbdf info.
 * @param dbdf pcie device dbdf info(input para)
 * @param glb_function_id the global function index of the chip(output para)
 * @return
 *   - 0: success
 *   - -1: fail, internal error
 *   - others: fail, refer to errno.h
 */
int hvio_get_glb_function_id_by_dbdf(uint32_t dbdf, uint16_t *glb_function_id);

/**
 * @brief send action to function, synchronous interface.
 * @param req send action request
 * @return
 *   - 0: success
 *   - -1: fail, internal error
 *   - others: fail, refer to errno.h
 */
int hvio_send_action(uint16_t glb_function_id, enum function_action action, const void *data,
		     uint16_t data_len);

/**
 * @brief DMA request. hw will load or store data between X86 host and spu ddr, asynchronous interface.
 * @param chnl_id is associated with L2NIC SQ ID.
 * @param req host dma request
 * @return
 *   - 0: success
 *   - <0: fail, refer to errno.h
 */
int hvio_host_dma_request(uint16_t chnl_id, hvio_host_dma_req_s *req);

/**
 * @brief poll RQ for dma response status. device provides DMA response in the same order with DMA request.
 * @param chnl_id is associated with L2NIC RQ ID.
 * @param poll_num the number of rsp want to be polled.
 * @param[out] rsp output for received response.
 * @return
 *   - >=0: the number of host dma rsp has been polled
 *   - <0: fail, refer to errno.h
 */
int hvio_host_dma_rsp_poll(uint16_t chnl_id, uint16_t poll_num, hvio_host_dma_rsp_s *rsp);

union hvio_nvme_config_cmd_info {
	uint32_t cmd[5];
};

/**
 * @brief get hot upgrade state
 * @param void
 * @return
 *   - 0:  success
 *   - -1: fail, internal error
 *   - others: fail, refer to errno.h
 */
int hvio_get_hot_upgrade_state(void);

/**
 * @brief check device ready
 * @param role 0--old process; 1--new process
 * @param proc_type enum proc_type, supoort PROC_TYPE_VBS and PROC_TYPE_BOOT
 * @param ready output_para 0--not ready, 1--ready
 * @return
 *   - 0: success
 *   - <0: fail, refer to errno.h
 */
int hvio_check_device_ready(uint8_t role, uint32_t proc_type, uint8_t *ready);

/**
 * @brief mount VIO volume, synchronous interface. Invoked by VIO.
 * @param glb_function_id the global function id of chip
 * @param lun_id the lun id of this volume
 * @param hash_paras hash item paras
 * @return
 *   - 0: success
 *   - -1: fail, internal error
 *   - others: fail, refer to errno.h
 */
int hvio_volume_mount(uint16_t glb_function_id, uint32_t lun_id,
		      struct hvio_mount_para *hash_paras);

/**
 * @brief umount VIO volume, synchronous interface. Invoked by VIO.
 * @param glb_function_id the global function id of chip
 * @param lun_id the lun id of this volume
 * @return
 *   - 0: success
 *   - -1: fail, internal error
 *   - others: fail, refer to errno.h
 */
int hvio_volume_umount(uint16_t glb_function_id, uint32_t lun_id);

/**
 * @brief update virtio device used or not.
 * @param glb_function_id the global function index of the chip
 * @param device_used virtio device is used or not
 * @return
 *   - 0: success
 *   - -1: fail, internal error
 *   - others: fail, refer to errno.h
 */
int hvio_update_virtio_device_used(uint16_t glb_function_id, uint64_t device_used);

/**
 * @brief release virtio blk vq resource.
 * @param glb_function_id the global function index of the chip, the related function is virtio_blk
 * @return
 *   - 0: success
 *   - <0: fail, refer to errno.h
 */
int hvio_virtio_blk_release_resource(uint16_t glb_function_id);

/**
 * @brief query slave host cfg require and cfg global func to it
 * @param null
 * @return
 */
void hvio_hotplug_cfg(void);

/**
 * @brief port_id hot plug add
 * @param port_id the global function index of the chip
 * @return
 *   - 0: success
 *   - -1: invalid port_id
 *   - -2: repeat hot plug
 *   - others: fail, refer to errno.h
 */
int hvio_hotplug_add(uint16_t port_id);

/**
 * @brief port_id hot plug del
 * @param port_id the global function index of the chip
 * @return
 *   - 0: success
 *   - -1: invalid port_id
 *   - -2: repeat hot del
 *   - others: fail, refer to errno.h
 */
int hvio_hotplug_del(uint16_t port_id);

/**
 * @brief func_id hotplug del async api
 * @param func_id the global function index of the chip
 * @return
 *   - 0:  success
 *   - 1:  remove failed
 *   - 2:  invalid func_id
 *   - 3:  repeat hot del
 *   - others: fail, refer to errno.h
 */
int hvio_hotplug_del_async(uint16_t port_id);

/**
 * @brief check hotplug if enable
 * @param null
 * @return
 *   - true: enable
 *   - false: disable
 */
bool hvio_hotplug_enable_check(void);

/**
 * @brief get hot del state
 * @param func_id the global function index of the chip
 * @return
 *   - 0:  success
 *   - 1:  remove failed
 *   - 2:  try again
 */
int hvio_hotplug_del_async_check(uint16_t port_id);

/**
 * @brief alloc virtio blk vq resource.
 * @param glb_function_id the global function index of the chip, the related function is virtio_blk
 * @return
 *   - 0: success
 *   - <0: fail, refer to errno.h
 */
int hvio_virtio_blk_alloc_resource(uint16_t glb_function_id, uint16_t queue_num);

/**
 * @brief vq bind core.
 * @param glb_function_id the global function index of the chip, the related function is virtio_blk
 * @param queue_num the num of vqueue
 * @return
 *   - 0: success
 *   - <0: fail, refer to errno.h
 */
int hvio_virtio_vq_bind_core(uint16_t glb_function_id, uint16_t queue_num);

/**
 * @brief vq unbind core.
 * @param glb_function_id the global function index of the chip, the related function is virtio_blk
 * @return
 *   - 0: success
 *   - <0: fail, refer to errno.h
 */
int hvio_virtio_vq_unbind_core(uint16_t glb_function_id);

struct hivo_qos_cfg {
	/* 指定qos 配置:单独配置pps、bps 或者同时配置, 0-pps, 1-bps, 2-bps&pps, other-invalid */
	uint32_t qos_type;
	/* 单桶配置 */
	uint32_t pps_cir;
	uint32_t pps_cbs;
	uint32_t bps_cir;
	uint32_t bps_cbs;
	/* 双桶配置 */
	uint32_t pps_pir;
	uint32_t pps_pbs;
	uint32_t bps_pir;
	uint32_t bps_pbs;
};

/**
 * @brief set limit of storage QoS.
 * @param level  level of QoS limit. 0-host, 1-group, 2-function
 * @param id  index of target, host_id/group_id/func_id
 * @param qos_cfg  information of QoS limit
 * @return
 *   - 0: success
 *   - other: fail, refer to errno.h
 */
int hvio_qos_set_limit(uint32_t level, uint32_t id, struct hivo_qos_cfg *qos_cfg);

/**
 * @brief get limit of storage QoS.
 * @param level  level of QoS limit. 0-host, 1-group, 2-function
 * @param id  index of target, host_id/group_id/func_id
 * @param[out] qos_cfg  output for storage Qos limit.
 * @return
 *   - 0: success
 *   - other: fail, refer to errno.h
 */
int hvio_qos_get_limit(uint32_t level, uint32_t id, struct hivo_qos_cfg *qos_cfg);

/**
 * @brief map func to group.
 * @param group_id  index of group
 * @param func_id  index of func
 * @return
 *   - 0: success
 *   - other: fail, refer to errno.h
 */
int hvio_qos_group_func_mmap(uint32_t group_id, uint32_t func_id);

/**
 * @brief unmap func from group.
 * @param group_id  index of group
 * @param func_id  index of func
 * @return
 *   - 0: success
 *   - other: fail, refer to errno.h
 */
int hvio_qos_group_func_unmmap(uint32_t group_id, uint32_t func_id);

struct storage_qos_func_list {
	uint32_t func_num; /* function number */
	uint16_t func_list[64];  /* max function num per QoS group */
};

/**
 * @brief get the function table of group .
 * @param group_id  index of group.
 * @param[out] result the function table of group
 * @return
 *   - 0: success
 *   - other: fail, refer to errno.h
 */
int hvio_qos_get_func_of_group(uint32_t group_id, struct storage_qos_func_list *result);

/**
 * @brief clear all qos cfg
 * @param void
 * @return
 *   - 0: success
 */
int hvio_qos_clear(void);

/**
 * @brief get group id by function id
 * @param func_id index of func.
 * @param group_id index of group.
 * @return
 *   - 0: get group id success
 *   - others: fail, refer to errno.h
 */
int hvio_qos_get_group_id_by_func(uint32_t func_id, uint32_t *group_id);

#endif /* HIVIO_API_H */
