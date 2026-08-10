#
#  BSD LICENSE
#  Copyright (c) Huawei Technologies Co., Ltd. 2021-2021. All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk

APP = ssam

C_SRCS := ssam.c

SYS_LIBS += -lcap
SPDK_LIB_LIST = $(ALL_MODULES_LIST) event_ssam event ssam

ifeq ($(OS),Linux)
SPDK_LIB_LIST += event_nbd
endif

ifeq ($(SPDK_ROOT_DIR)/lib/env_dpdk,$(CONFIG_ENV))
SPDK_LIB_LIST += env_dpdk_rpc
endif

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk

install: $(APP)
	$(INSTALL_APP)

uninstall:
	$(UNINSTALL_APP)
