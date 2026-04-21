/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021-2025 Huawei Technologies Co.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/log.h"
#include "dpak_ssam.h"

#define SSAM_DBDF_DOMAIN_OFFSET          16
#define SSAM_DBDF_BUS_OFFSET             8
#define SSAM_DBDF_DEVICE_OFFSET          3
#define SSAM_DBDF_FUNC_OFFSET            0x7
#define SSAM_DBDF_DOMAIN_MAX             0xffff
#define SSAM_DBDF_BUS_MAX                0xff
#define SSAM_DBDF_DEVICE_MAX             0x1f
#define SSAM_DBDF_FUNCTION_MAX           0x7
#define SSAM_DBDF_DOMAIN_MAX_LEN         4
#define SSAM_DBDF_BD_MAX_LEN             2
#define SSAM_DBDF_FUNCTION_MAX_LEN       1
#define SSAM_DBDF_MAX_STR_LEN            20
#define SSAM_STR_CONVERT_HEX             16


struct ssam_dbdf {
	uint32_t domain;
	uint32_t bus;
	uint32_t device;
	uint32_t function;
};

static int
ssam_dbdf_cvt_str2num(char *input, uint16_t val_limit, uint32_t len_limit,
		      uint32_t *num_resolved)
{
	char *end_ptr = NULL;
	long int val = strtol(input, &end_ptr, SSAM_STR_CONVERT_HEX);

	if (strlen(input) > len_limit) {
		return -EINVAL;
	}

	if (end_ptr == NULL || end_ptr == input || *end_ptr != '\0') {
		return -EINVAL;
	}
	if (val < 0 || val > val_limit) {
		return -EINVAL;
	}

	*num_resolved = (uint32_t)val;
	return 0;
}

/* resolve dbdf's domain */
static int
ssam_dbdf_cvt_dom(char *str, struct ssam_dbdf *dbdf,
		  char **bus)
{
	char *colon2 = NULL;
	int rc;

	/* find second ":" from dbdf string */
	colon2 = strchr(str, ':');
	if (colon2 != NULL) {
		*colon2++ = 0;
		*bus = colon2;
		if (str[0] != 0) {
			/* convert domain number */
			rc = ssam_dbdf_cvt_str2num(str, SSAM_DBDF_DOMAIN_MAX,
						   SSAM_DBDF_DOMAIN_MAX_LEN, &dbdf->domain);
			if (rc != 0) {
				SPDK_ERRLOG("Invalid characters of domain number!\n");
				return rc;
			}
		} else {
			SPDK_ERRLOG("domain number is blank!\n");
			return -EINVAL;
		}
	} else {
		/* dbdf string does not contain domain number */
		*bus = str;
	}

	return 0;
}

/* resolve dbdf's bus */
static int
ssam_dbdf_cvt_b(struct ssam_dbdf *dbdf, char *bus)
{
	int rc;

	if (bus[0] != 0) {
		/* convert bus number */
		rc = ssam_dbdf_cvt_str2num(bus, SSAM_DBDF_BUS_MAX,
					   SSAM_DBDF_BD_MAX_LEN, &dbdf->bus);
		if (rc != 0) {
			SPDK_ERRLOG("Invalid characters of bus number!\n");
			return rc;
		}
	} else {
		SPDK_ERRLOG("bus number is blank!\n");
		return -EINVAL;
	}

	return 0;
}

/* resolve dbdf's domain and bus part */
static int
ssam_dbdf_cvt_domb(char *str, struct ssam_dbdf *dbdf,
		   char **colon_input, char **mid_input)
{
	char *bus = NULL;
	char *colon = *colon_input;
	int rc;

	*colon++ = 0;
	*mid_input = colon;
	rc = ssam_dbdf_cvt_dom(str, dbdf, &bus);
	if (rc != 0) {
		return rc;
	}

	return ssam_dbdf_cvt_b(dbdf, bus);
}

/* resolve dbdf's device */
static int
ssam_dbdf_cvt_dev(struct ssam_dbdf *dbdf, char *mid)
{
	int rc;

	if (mid[0] != 0) {
		/* convert device number */
		rc = ssam_dbdf_cvt_str2num(mid, SSAM_DBDF_DEVICE_MAX,
					   SSAM_DBDF_BD_MAX_LEN, &dbdf->device);
		if (rc != 0) {
			SPDK_ERRLOG("Invalid characters of device number!\n");
			return rc;
		}
	} else {
		SPDK_ERRLOG("device number is blank!\n");
		return -EINVAL;
	}

	return 0;
}

static int
ssam_dbdf_cvt_f(struct ssam_dbdf *dbdf, char *dot)
{
	int rc;

	if (dot != NULL && dot[0] != 0) {
		/* convert function number */
		rc = ssam_dbdf_cvt_str2num(dot, SSAM_DBDF_FUNCTION_MAX,
					   SSAM_DBDF_FUNCTION_MAX_LEN, &dbdf->function);
		if (rc != 0) {
			SPDK_ERRLOG("Invalid characters of function number!\n");
			return rc;
		}
	} else {
		SPDK_ERRLOG("function number is blank!\n");
		return -EINVAL;
	}

	return 0;
}

/* resolve dbdf's device and function part */
static int
ssam_dbdf_cvt_devf(struct ssam_dbdf *dbdf, char **dot_input, char **mid_input)
{
	char *dot = *dot_input;
	int rc;

	if (dot != NULL) {
		*dot++ = 0;
	} else {
		/* Input dbdf string does not contain "." */
		SPDK_ERRLOG("Invalid DBDF format\n");
		return -1;
	}

	rc = ssam_dbdf_cvt_dev(dbdf, *mid_input);
	if (rc != 0) {
		return rc;
	}

	return ssam_dbdf_cvt_f(dbdf, dot);
}

static uint32_t
ssam_dbdf_assemble(const struct ssam_dbdf *dbdf)
{
	return ((dbdf->domain << SSAM_DBDF_DOMAIN_OFFSET) |
		(dbdf->bus << SSAM_DBDF_BUS_OFFSET) |
		(dbdf->device << SSAM_DBDF_DEVICE_OFFSET) |
		(dbdf->function & SSAM_DBDF_FUNC_OFFSET));
}

static int
ssam_dbdf_cvt_dbdf(char *str, size_t len, uint32_t *dbdf)
{
	if (dbdf == NULL) {
		SPDK_ERRLOG("dbdf is null\n");
		return -1;
	}
	/* find ":" from dbdf string */
	char *colon = strrchr(str, ':');
	/* find "." from dbdf string */
	char *dot = NULL;
	char *mid = str;
	int rc;
	struct ssam_dbdf st_dbdf = {0};

	if (colon != NULL) {
		rc = ssam_dbdf_cvt_domb(str, &st_dbdf, &colon, &mid);
		if (rc != 0) {
			return rc;
		}
	} else {
		/* Input dbdf string does not contain ":" */
		SPDK_ERRLOG("Invalid DBDF format\n");
		return -EINVAL;
	}

	dot = strchr((colon ? (colon + 1) : str), '.');
	rc = ssam_dbdf_cvt_devf(&st_dbdf, &dot, &mid);
	if (rc != 0) {
		return rc;
	}

	*dbdf = ssam_dbdf_assemble(&st_dbdf);

	return 0;
}

/* convert dbdf from string to number */
int
ssam_dbdf_str2num(char *str, uint32_t *dbdf)
{
	int len;
	char *dbdf_str = NULL;
	int ret;

	if (str == NULL) {
		SPDK_ERRLOG("dbdf str2num input str null!\n");
		return -EINVAL;
	}

	if (dbdf == NULL) {
		SPDK_ERRLOG("dbdf str2num output dbdf null!\n");
		return -EINVAL;
	}

	len = strlen(str);
	if (len == 0 || len > SSAM_DBDF_MAX_STR_LEN) {
		SPDK_ERRLOG("dbdf str2num len %u error!\n", len);
		return -ERANGE;
	}

	dbdf_str = (char *)malloc(len + 1);
	if (dbdf_str == NULL) {
		return -ENOMEM;
	}

	ret = snprintf(dbdf_str, len + 1, "%s", str);
	if ((ret > len) || (ret <= 0)) {
		SPDK_ERRLOG("dbdf str2num snprintf_s error\n");
		free(dbdf_str);
		return -EINVAL;
	}

	ret = ssam_dbdf_cvt_dbdf(dbdf_str, len, dbdf);
	free(dbdf_str);
	dbdf_str = NULL;

	return ret;
}

static void
ssam_dbdf_num2struct(uint32_t dbdf, struct ssam_dbdf *st_dbdf)
{
	st_dbdf->domain = (dbdf >> SSAM_DBDF_DOMAIN_OFFSET) & SSAM_DBDF_DOMAIN_MAX;
	st_dbdf->bus = (dbdf >> SSAM_DBDF_BUS_OFFSET) & SSAM_DBDF_BUS_MAX;
	st_dbdf->device = (dbdf >> SSAM_DBDF_DEVICE_OFFSET) & SSAM_DBDF_DEVICE_MAX;
	st_dbdf->function = dbdf & SSAM_DBDF_FUNCTION_MAX;
	return;
}

int
ssam_dbdf_num2str(uint32_t dbdf, char *str, size_t len)
{
	int ret;
	struct ssam_dbdf st_dbdf = {0};

	if (str == NULL) {
		SPDK_ERRLOG("dbdf num2str output str null!\n");
		return -EINVAL;
	}

	ssam_dbdf_num2struct(dbdf, &st_dbdf);

	ret = snprintf(str, len - 1, "%04x:%02x:%02x.%x",
		       st_dbdf.domain, st_dbdf.bus, st_dbdf.device, st_dbdf.function);
	if ((ret >= (int)(len - 1)) || (ret <= 0)) {
		SPDK_ERRLOG("dbdf num2str error\n");
		return -EINVAL;
	}

	return 0;
}
