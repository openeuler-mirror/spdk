/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021-2025 Huawei Technologies Co.
 *   All rights reserved.
 */

#include "spdk/ssam.h"
#include "spdk/string.h"

#define IOVA_MODE_PA "pa"

static bool g_start_flag = false;

bool
spdk_ssam_is_starting(void)
{
	return g_start_flag;
}

static void
ssam_started(void *ctx)
{
	int rc;
	int hot_upgrade_status = spdk_ssam_get_hot_upgrade_status();
	int hot_restart = spdk_ssam_get_hot_restart();

	SPDK_NOTICELOG("ssam started, hot restart :%d, hot upgrade :%d\n", hot_restart, hot_upgrade_status);
	if (hot_upgrade_status == SSAM_HOT_UPGRADE_BEGIN) {
		spdk_ssam_set_hot_upgrade_status(SSAM_HOT_UPGRADE_INIT_DONE);
	} else {
		rc = spdk_ssam_io_poller_start();
		if (rc != 0) {
			spdk_app_stop(rc);
			return;
		}
	}
	spdk_ssam_set_hot_restart(false);
	g_start_flag = false;
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
	g_start_flag = true;

	rc = spdk_ssam_user_config_init();
	if (rc != 0) {
		SPDK_ERRLOG("ssam user config init failed: %s\n", spdk_strerror(-rc));
		exit(rc);
	}

	shm_id = shm_open(SSAM_SHM, O_RDWR, SSAM_SHM_PERMIT);
	if (shm_id < 0) {
		SPDK_NOTICELOG("ssam share memory hasn't been created.\n");
		g_start_flag = false;
	} else {
		spdk_ssam_set_shm_created(true);
		SPDK_NOTICELOG("ssam share memory has been created.\n");
		close(shm_id);
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
	spdk_ssam_set_hot_restart(opts.hot_restart);
	if (opts.hot_upgrade) {
		spdk_ssam_set_hot_upgrade_status(SSAM_HOT_UPGRADE_BEGIN);
		SPDK_NOTICELOG("ssam start hot upgrade.\n");
	}

	/* Blocks until the application is exiting */
	rc = spdk_app_start(&opts, ssam_started, NULL);
	spdk_ssam_exit();

	spdk_app_fini();
	SPDK_NOTICELOG("%s server exited.\n", SSAM_SERVER_NAME);

	return rc;
}
