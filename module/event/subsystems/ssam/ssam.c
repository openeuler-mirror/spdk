/*
 *   BSD LICENSE
 *   Copyright (c) Huawei Technologies Co., Ltd. 2021-2021. All rights reserved.
 *
 */

#include "spdk/stdinc.h"

#include "spdk/ssam.h"

#include "spdk_internal/event.h"

static void
ssam_subsystem_init_done(int rc)
{
	spdk_subsystem_init_next(rc);
}

static void
ssam_subsystem_init(void)
{
	spdk_ssam_subsystem_init(ssam_subsystem_init_done);
}

static void
ssam_subsystem_fini_done(void)
{
	spdk_subsystem_fini_next();
}

static void
ssam_subsystem_fini(void)
{
	spdk_ssam_subsystem_fini(ssam_subsystem_fini_done);
}

static struct spdk_subsystem g_spdk_subsystem_ssam = {
	.name = SSAM_SERVER_NAME,
	.init = ssam_subsystem_init,
	.fini = ssam_subsystem_fini,
	.write_config_json = spdk_ssam_config_json,
};

SPDK_SUBSYSTEM_REGISTER(g_spdk_subsystem_ssam);
SPDK_SUBSYSTEM_DEPEND(ssam, scsi)
