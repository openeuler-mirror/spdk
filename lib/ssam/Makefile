#
#   BSD LICENSE
#   Copyright (c) Huawei Technologies Co., Ltd. 2021-2021. All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 1
SO_MINOR := 0

CFLAGS += -I. -I../../dpdk/lib/eal/common
CFLAGS += $(ENV_CFLAGS)

C_SRCS = ssam.c ssam_blk.c ssam_rpc.c \
	ssam_config.c ssam_scsi.c ssam_malloc.c ssam_device_pcie.c \
	ssam_fs.c ssam_fuse_adapter.c
C_SRCS += ssam_driver/ssam_driver.c
C_SRCS += ssam_driver/ssam_dbdf.c
C_SRCS += ssam_driver/ssam_mempool.c
C_SRCS += ssam_driver/ssam_driver_adapter.c

LIBNAME = ssam

SPDK_MAP_FILE = $(abspath $(CURDIR)/spdk_ssam.map)

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
