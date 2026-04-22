/*
 * BSD LICENSE
 * Copyright (c) Huawei Technologies Co., Ltd. 2021-2021. All rights reserved.
 * This file contains code segment derived form pkgeval.jl
 * Original copyright notice:
 *   Copyright (C) 2001-2008 Miklos Szeredi <miklos@szeredi.hu> 
 *   This program can be distributed under the terms of the GNU GPL.
 *   This -- and only this --header file may also be distributed under the term of 
 *   BSD License as flows:
 *   Copyright (C) 2001-2007 Miklos Szeredi. ALL rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     1.Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     2.Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *
 *   THIS SOFTWARE IS PROVIDED BY AUTHOR AND CONTRIBUTORS "AS IS" AND
 *   ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE 
 *   IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *   ARE DISCLAIMED. IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 *   FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL 
 *   DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 *   OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) 
 *   HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *   LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 *   OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 *   SUCH DAMAGE.
 */
 
#ifndef SSAM_FS_INTERNAL_H
#define SSAM_FS_INTERNAL_H

#define FUSE_USE_VERSION 34

#include "spdk/stdinc.h"
#include <sys/file.h>
#include <sys/xattr.h>
#include <rte_eal.h>
#include <rte_per_lcore.h>
#include <rte_lcore.h>
#include <rte_memory.h>

#include "fuse3/fuse.h"
#include "fuse3/fuse_lowlevel.h"

#include "ssam_driver/dpak_ssam.h"
#include "ssam_internal.h"

/* We are re-using pointers to our `struct lo_inode` and `struct
   lo_dirp` elements as inodes. This means that we must be able to
   store uintptr_t values in a fuse_ino_t variable. The following
   incantation checks this condition at compile time. */
#if defined(__GNUC__) && (__GNUC__ > 4 || __GNUC__ == 4 && __GNUC_MINOR__ >= 6) && !defined __cplusplus
_Static_assert(sizeof(fuse_ino_t) >= sizeof(uintptr_t),
	       "fuse_ino_t too small to hold uintptr_t values!");
#else
struct _uintptr_to_must_hold_fuse_ino_t_dummy_struct {
unsigned _uintptr_to_must_hold_fuse_ino_t :
	((sizeof(fuse_ino_t) >= sizeof(uintptr_t)) ? 1 : -1);
};
#endif

#define SSAM_FS_DEFAULT_THREADS 4
#define SSAM_FS_LCORE_ID_MAX    24
#define SSAM_FUSE_ARGS_NUM      3
#define SSAM_FS_FLR_POLLER_PERIOD   (10 * 1000 * 1000) /* 10s */
#define SSAM_FS_FLR_SEQ_PATH    "/proc/vbs/flr_seq"
#define SSAM_FS_BUF_LEN         256
#define SSAM_FS_MAX_PAGES       104
#define SSAM_FS_STATIC_BUF_SIZE (PATH_MAX + 1)
#define SSAM_FS_DEFAULT_ALIGN   0x1000
#define SSAM_FS_REF_COUNT 2

#define ST_ATIM_NSEC(stbuf) 0
#define ST_CTIM_NSEC(stbuf) 0
#define ST_MTIM_NSEC(stbuf) 0

#define FOPEN_DIRECT_IO (1 << 0)
#define FOPEN_KEEP_CACHE (1 << 1)
#define FOPEN_NONSEEKABLE (1 << 2)
#define FOPEN_CACHE_DIR (1 << 3)
#define FOPEN_STREAM (1 << 4)
#define FOPEN_NOFLUSH (1 << 5)
#define FOPEN_PARALLEL_DIRECT_WRITES (1 << 6)

#define SHM_SIZE 4096
#define SHM_NAME 32
#define MEM_MAJOR 7
#define MEM_MINOR 40

/* UDAA Error provides information regarding different errors caused while using the UDAA libraries. */

typedef enum udaa_error {
	UDAA_SUCCESS,
	UDAA_ERROR_UNKNOWN,
	UDAA_ERROR_NOT_PERMITTED,	  /**< Operation not permitted */
	UDAA_ERROR_IN_USE,		      /**< Resource already in use */
	UDAA_ERROR_NOT_SUPPORTED,	  /**< Operation not supported */
	UDAA_ERROR_WAIT,		      /**< Resource temporarily unavailable, try again */
	UDAA_ERROR_INVALID_VALUE,	  /**< Invalid input */
	UDAA_ERROR_NO_MEMORY,		  /**< Memory allocation failure */
	UDAA_ERROR_INITIALIZATION,	  /**< Resource initialization failure */
	UDAA_ERROR_TIME_OUT,		  /**< Timer expired waiting for resource */
	UDAA_ERROR_NOT_FOUND,         /**< Resource Not Found */
	UDAA_ERROR_IO_FAILED,		  /**< Input/Output Operation Failed */
	UDAA_ERROR_BAD_STATE,		  /**< Bad State */
	UDAA_ERROR_AGAIN,		      /**< No element is available, try again later */
} udaa_error_t;

/**
 * @brief Specifies the device type for udaa representor device
 *
 */
enum udaa_pci_func_type {
	UDAA_PCI_FUNC_PF = 0,       /* physical function */
	UDAA_PCI_FUNC_VF,           /* virtual function */
	UDAA_PCI_FUNC_SF,           /* sub function */
};

/**
 * @brief Specifies the PCI function type for udaa representor device
 *
 */
typedef enum udaa_func_emulation_type {
	UDAA_PCI_FUNC_NVME,         /* nvme emulation function */
	UDAA_PCI_FUNC_VIRTIO_NET,   /* virtio-net emulation function */
	UDAA_PCI_FUNC_VIRTIO_BLK,   /* virtio-blk emulation function */
	UDAA_PCI_FUNC_VIRTIO_SCSI,  /* virtio-blk emulation function */
	UDAA_PCI_FUNC_VIRTIO_VSOCK, /* virtio-blk emulation function */
	UDAA_PCI_FUNC_VIRTIO_FS,    /* virtio-fs emulation function */
} udaa_eml_type_t;

struct udaa_emlq {
	uint16_t queue_id;
	udaa_eml_type_t eml_type;
	struct ssam_request **vmio_req;
};

#define VIRTIO_FS_CFG_TAG_SIZE 36

struct udaa_dev_fs_cfg {
	/* Name associated with FS */
	uint8_t tag[VIRTIO_FS_CFG_TAG_SIZE];

	/* Total number of VQs exposed by the device */
	uint32_t num_request_queues;

	/* Minimum byte size of each buffer in the notification queue, if such is supported */
	uint32_t notify_buf_min_size_bytes;
};

struct udaa_dev_info {
	char sn[16];

	uint16_t dev_id;
	uint16_t dpdk_tid;
	uint16_t modern; /* not used */

	uint16_t vq_size;
	uint16_t num_vqs;

	uint16_t func_id; /* global function id */
	uint16_t pf_id;   /* pf id */
	uint16_t vf_num;  /* if is pf */

	uint16_t pf_idx;
	uint16_t pf_configured;

	enum udaa_pci_func_type pci_func_type;
	uint16_t func_eml_type;

	struct udaa_dev_fs_cfg fs_cfg;
};

struct udaa_eml_dev {
	uint16_t pf_idx;
	struct udaa_dev_info dev_info;
};

/**
 * @brief Emulation request structure describes request created by the emulation device consumer and delivered by correspondendt context.
 *
 * UDAA Job layout
 *
 *  SDK job --> +--------------------------+
 *              |     UDAA Job (base)      |
 *              | type                     |
 *              | ctx                      |
 *              |                          |
 *              +------------+-------------+ <--  request arguments
 *              |                          |      variable size
 *              | arguments                |      library specific
 *              |     .                    |      structure
 *              |     .                    |
 *              |     .                    |
 *              |     .                    |
 *              |     .                    |
 *              |     .                    |
 *              |                          |
 *              +------------+-------------+
 */
struct mount_info {
	size_t size;
	enum fuse_buf_flags flags;
	int fd;
	off_t pos;
	uint32_t	len;
	uint32_t	opcode;
	uint64_t	unique;
	uint64_t	nodeid;
	uint32_t	uid;
	uint32_t	gid;
	uint32_t	pid;
	uint16_t	total_extlen; /* length of extensions in 8byte units */
	uint16_t	padding;
};

struct fuse_init_in {
	uint32_t	major;
	uint32_t	minor;
	uint32_t	max_readahead;
	uint32_t	flags;
	uint32_t	flags2;
	uint32_t	unused[11];
};

struct udaa_eml_req {
	int type;                     /* < Defines the type of the request. */
	struct udaa_eml_dev *eml_dev; /* < UDAA Emulation device that executes the request. */
	struct udaa_buf **in_buf;     /* < Data_in buffers > */
	uint32_t in_buf_num;          /* < Number of in_buf > */
	struct udaa_buf **out_buf;    /* < Data_out buffers(the first one is for the fuse_out_header) > */
	uint32_t out_buf_num;         /* < Number of out_buf > */
	void *buf;                    /* < Consecutive buffer for application usage > */
	size_t buf_len;               /* < Consecutive buffer size > */
};

enum host_dma_mode {
	READ_HOST_MODE = 0,  /* *< read host data and write to SPU */
	WRITE_HOST_MODE = 1, /* *< write data to host */
	HOST_DMA_MODE_MAX
};

struct lo_inode {
	struct lo_inode *next; /* protected by lo->mutex */
	struct lo_inode *prev; /* protected by lo->mutex */
	int fd;
	ino_t ino;
	dev_t dev;
	uint64_t refcount; /* protected by lo->mutex */
};

enum {
	CACHE_NEVER,
	CACHE_NORMAL,
	CACHE_ALWAYS,
};

struct lo_data {
	pthread_mutex_t mutex;
	pthread_mutex_t exit_mutex;
	int writeback;
	int flock;
	int xattr;
	char *source;
	double timeout;
	int cache;
	int timeout_set;
	struct lo_inode root; /* protected by lo->mutex */

	struct udaa_emlq **udaa_fs_queues;
	uint32_t num_queues;
	struct udaa_eml_req *fs_reqs;
	struct fuse_session *se;
	struct spdk_ssam_session *smsession[SSAM_FS_LCORE_ID_MAX];
	uint16_t gfunc_id;
	uint16_t exit_num;
	bool mounted;
	bool used;
	char *name;
	char *dbdf;
	uint32_t flr_seq;
	bool have_shm;
	bool load_shm_flag;
	bool delete_flag;

	spdk_ssam_session_rsp_fn rsp_fn;
	void *rsp_ctx;
};

struct fuse_out_header {
	uint32_t len;
	int32_t error;
	uint64_t unique;
};

struct fuse_attr {
	uint64_t ino;
	uint64_t size;
	uint64_t blocks;
	uint64_t atime;
	uint64_t mtime;
	uint64_t ctime;
	uint32_t atimensec;
	uint32_t mtimensec;
	uint32_t ctimensec;
	uint32_t mode;
	uint32_t nlink;
	uint32_t uid;
	uint32_t gid;
	uint32_t rdev;
	uint32_t blksize;
	uint32_t flags;
};

#define SSAM_FUSE_COMPAT_ATTR_OUT_SIZE 96

struct fuse_attr_out {
	uint64_t attr_valid; /* Cache timeout for the attributes */
	uint32_t attr_valid_nsec;
	uint32_t dummy;
	struct fuse_attr attr;
};

#define SSAM_FUSE_COMPAT_ENTRY_OUT_SIZE 120

struct fuse_entry_out {
	uint64_t nodeid;      /* Inode ID */
	uint64_t generation;  /* Inode generation: nodeid:gen must
                     be unique for the fs's lifetime */
	uint64_t entry_valid; /* Cache timeout for the name */
	uint64_t attr_valid;  /* Cache timeout for the attributes */
	uint32_t entry_valid_nsec;
	uint32_t attr_valid_nsec;
	struct fuse_attr attr;
};

struct fuse_open_out {
	uint64_t fh;
	uint32_t open_flags;
	uint32_t padding;
};

struct fuse_write_out {
	uint32_t size;
	uint32_t padding;
};

struct fuse_kstatfs {
	uint64_t blocks;
	uint64_t bfree;
	uint64_t bavail;
	uint64_t files;
	uint64_t ffree;
	uint32_t bsize;
	uint32_t namelen;
	uint32_t frsize;
	uint32_t padding;
	uint32_t spare[6];
};

#define SSAM_FUSE_COMPAT_STATFS_SIZE 48

struct fuse_statfs_out {
	struct fuse_kstatfs st;
};

struct fuse_getxattr_out {
	uint32_t size;
	uint32_t padding;
};

struct fuse_lseek_out {
	uint64_t offset;
};

struct fuse_req {
	struct fuse_session *se;
	uint64_t unique;
	int ctr;
	pthread_mutex_t lock;
	struct fuse_ctx ctx;
	struct fuse_chan *ch;
	int interrupted;
	unsigned int ioctl_64bit : 1;
	union {
		struct {
			uint64_t unique;
		} i;
		struct {
			fuse_interrupt_func_t func;
			void *data;
		} ni;
	} u;
	struct fuse_req *next;
	struct fuse_req *prev;
};

struct fuse_notify_req {
	uint64_t unique;
	void (*reply)(struct fuse_notify_req *, fuse_req_t, fuse_ino_t, const void *,
		      const struct fuse_buf *);
	struct fuse_notify_req *next;
	struct fuse_notify_req *prev;
};

struct fuse_session {
	char *mountpoint;
	volatile int exited;
	int fd;
	struct fuse_custom_io *io;
	struct mount_opts *mo;
	int debug;
	int deny_others;
	struct fuse_lowlevel_ops op;
	int got_init;
	struct cuse_data *cuse_data;
	void *userdata;
	uid_t owner;
	struct fuse_conn_info conn;
	struct fuse_req list;
	struct fuse_req interrupts;
	pthread_mutex_t lock;
	int got_destroy;
	pthread_key_t pipe_key;
	int broken_splice_nonblock;
	uint64_t notify_ctr;
	struct fuse_notify_req notify_list;
	size_t bufsize;
	int error;
};

struct fuse_chan {
	pthread_mutex_t lock;
	int ctr;
	int fd;
};

struct fuse_release_in {
	uint64_t fh;
	uint32_t flags;
	uint32_t release_flags;
	uint64_t lock_owner;
};

#define SSAM_FUSE_OPCODE_READ       15
#define SSAM_FUSE_OPCODE_WRITE      16
#define SSAM_FUSE_OPCODE_RELEASE    18
#define SSAM_FUSE_OPCODE_FLUSH      25
#define SSAM_FUSE_OPCODE_INIT       26
#define SSAM_FUSE_OPCODE_RELEASEDIR 29
#define SSAM_FUSE_OPCODE_DESTROY    38

struct fuse_in_header {
	uint32_t	len;
	uint32_t	opcode;
	uint64_t	unique;
	uint64_t	nodeid;
	uint32_t	uid;
	uint32_t	gid;
	uint32_t	pid;
	uint16_t	total_extlen; /* length of extensions in 8byte units */
	uint16_t	padding;
};

struct fuse_init_out {
	uint32_t	major;
	uint32_t	minor;
	uint32_t	max_readahead;
	uint32_t	flags;
	uint16_t	max_background;
	uint16_t	congestion_threshold;
	uint32_t	max_write;
	uint32_t	time_gran;
	uint16_t	max_pages;
	uint16_t	map_alignment;
	uint32_t	flags2;
	uint32_t	unused[7];
};

struct ssam_fs_stat {
	uint64_t bytes_read;            /* Read Bytes */
	uint64_t num_read_ops;          /* Read IO */
	uint64_t bytes_written;         /* Write Bytes */
	uint64_t num_write_ops;         /* Write IO */
	uint64_t read_latency_ticks;
	uint64_t write_latency_ticks;

	uint64_t complete_read_ios;     /* Number of successfully completed read requests, */
	uint64_t err_read_ios;          /* Number of failed completed read requests, */
	uint64_t complete_write_ios;    /* Number of successfully completed write requests, */
	uint64_t err_write_ios;         /* Number of failed completed write requests, */
	uint64_t flush_ios;             /* Total number of flush requests, */
	uint64_t complete_flush_ios;    /* Number of successfully completed flush requests, */
	uint64_t err_flush_ios;         /* Number of failed completed flush requests, */
	uint64_t fatal_ios;
	uint64_t other_ios;

	uint64_t start_tsc;
	uint64_t complete_start_tsc;
	uint64_t complete_end_tsc;

	uint32_t op_type;
	uint32_t payload_size;
	__virtio32 type;
};

struct spdk_ssam_fs_session {
	struct spdk_ssam_session smsession;
	struct spdk_poller *fs_poller;
	struct fuse_buf fbuf;
	struct fuse_out_header iov_header;

	char *dynamic_buf;          /* for read/write IO, which size is variable. */
	char *static_buf;           /* for most control IO, which size is 4096. */
	ssize_t in_len;

	struct iovec
		*src_iov;      /* for asynchronous dma in fuse_udaa_writev, which iovcnt is variable. */
	struct iovec dst_iov;       /* for asynchronous dma in fuse_udaa_read, which iovcnt is 1. */
	bool fbuf_used;
	bool need_write_config;
	struct lo_data *p_lo;
	struct ssam_fs_stat fs_stat;
};

struct spdk_ssam_dev_io_scan_poller_ctx {
	uint8_t func_id;
	bool restart_flag;
};

struct spdk_ssam_fs_poller_ctx {
	struct spdk_poller *flr_seq_poller;
	int flr_fd;
	void *flr_map;
	struct spdk_poller *pf_poller[SSAM_HOSTEP_NUM_MAX];
	pthread_mutex_t poll_mutex[SSAM_HOSTEP_NUM_MAX];
	struct spdk_ssam_dev_io_scan_poller_ctx pf_poller_ctx[SSAM_HOSTEP_NUM_MAX];
};

int ssam_fuse_reply_entry(fuse_req_t req, const struct fuse_entry_param *e,
			  struct spdk_ssam_fs_session *fsmsession);
int ssam_fuse_reply_open(fuse_req_t req, const struct fuse_file_info *f,
			 struct spdk_ssam_fs_session *fsmsession);
int ssam_fuse_reply_create(fuse_req_t req, const struct fuse_entry_param *e,
			   const struct fuse_file_info *f,
			   struct spdk_ssam_fs_session *fsmsession);
int ssam_fuse_reply_attr(fuse_req_t req, const struct stat *attr, double attr_timeout,
			 struct spdk_ssam_fs_session *fsmsession);
int ssam_fuse_reply_write(fuse_req_t req, size_t count, struct spdk_ssam_fs_session *fsmsession);
int ssam_fuse_reply_statfs(fuse_req_t req, const struct statvfs *stbuf,
			   struct spdk_ssam_fs_session *fsmsession);
int ssam_fuse_reply_xattr(fuse_req_t req, size_t count, struct spdk_ssam_fs_session *fsmsession);
int ssam_fuse_reply_lseek(fuse_req_t req, off_t off, struct spdk_ssam_fs_session *fsmsession);
int ssam_fuse_reply_data(fuse_req_t req, struct fuse_bufvec *bufv, enum fuse_buf_copy_flags flags,
			 struct spdk_ssam_fs_session *fsmsession);

#endif
/* SSAM_FS_INTERNAL_H */
