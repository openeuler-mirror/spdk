/*
 *   BSD LICENSE
 *   Copyright (c) Huawei Technologies Co., Ltd. 2021-2021. All rights reserved.
 *
 */

#include <rte_malloc.h>
#include "spdk/env.h"

#include "ssam_internal.h"

int ssam_free_ex(void *addr)
{
	spdk_free(addr);
	return 0;
}

int ssam_malloc_elem_from_addr(const void *data, unsigned long long *pg_size, int *socket_id)
{
	struct rte_memseg_list *msl = NULL;

	msl = rte_mem_virt2memseg_list(data);
	if (msl == NULL) {
		return -1;
	}

	*socket_id = msl->socket_id;
	*pg_size = msl->page_sz;
	return 0;
}
