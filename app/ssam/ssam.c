/*
 *   BSD LICENSE
 *   Copyright (c) Huawei Technologies Co., Ltd. 2021-2021. All rights reserved.
 *
 */

#include "spdk/ssam.h"
#include "spdk/string.h"

#define IOVA_MODE_PA "pa"

static void
ssam_started(void *ctx)
{
	spdk_ssam_poller_start();
	SPDK_NOTICELOG("%s server started.\n", SSAM_SERVER_NAME);
}

int
main(int argc, char *argv[])
{
	struct spdk_app_opts opts = {};
	int rc;
	int shm_id;

	spdk_app_opts_init(&opts, sizeof(opts));
	opts.name = SSAM_SERVER_NAME;
	opts.iova_mode = IOVA_MODE_PA;
	opts.num_entries = 0;

	spdk_ssam_user_config_init();

	shm_id = shm_open(SSAM_SHM, O_RDWR, SSAM_SHM_PERMIT);
	if (shm_id < 0) {
		SPDK_NOTICELOG("ssam share memory hasn't been created.\n");
	} else {
		spdk_ssam_set_shm_created(true);
		SPDK_NOTICELOG("ssam share memory has been created.\n");
	}

	rc = spdk_ssam_rc_preinit();
	if (rc < 0) {
		exit(rc);
	}

	rc = spdk_app_parse_args(argc, argv, &opts, NULL, NULL, NULL, NULL);
	if (rc != SPDK_APP_PARSE_ARGS_SUCCESS) {
		SPDK_ERRLOG("spdk app parse args fail: %d\n", rc);
		exit(rc);
	}

	/* Blocks until the application is exiting */
	rc = spdk_app_start(&opts, ssam_started, NULL);
	spdk_ssam_exit();

	spdk_app_fini();
	SPDK_NOTICELOG("%s server exited.\n", SSAM_SERVER_NAME);

	return rc;
}
