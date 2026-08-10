/*
 *   BSD LICENSE
 *   Copyright (c) Huawei Technologies Co., Ltd. 2021-2021. All rights reserved.
 *
 */

#include "spdk/stdinc.h"

#include "spdk/log.h"
#include "spdk/env.h"
#include "dpak_ssam.h"

#define MP_CK_HEADER_LEN    sizeof(struct ssam_mp_chunk)
#define MP_CK_END_LEN       sizeof(struct ssam_mp_chunk*)
#define MP_CK_CB_LEN        (MP_CK_HEADER_LEN + MP_CK_END_LEN)

#define SHIFT_2MB           21 /* (1 << 21) == 2MB */
#define VALUE_2MB           (1ULL << SHIFT_2MB)
#define SHIFT_1GB           30 /* (1 << 30) == 1G */
#define VALUE_1GB           (1ULL << SHIFT_1GB)
#define SSAM_SPDK_VTOPHYS_ERROR (0xFFFFFFFFFFFFFFFFULL)
#define SSAM_DMA_MEM_MAGIC  (0xBABEFACEBABEFACE)

struct ssam_mp_dma_mem {
	uint64_t magic;
	uint64_t size;
	char mem[0];
};

struct ssam_mp_chunk {
	struct ssam_mp_chunk *prev;
	struct ssam_mp_chunk *next;

	/* Total size of the memory pool chunk, the chunk is in the memory block */
	uint64_t size;

	/* The chunk is free when true or in use when false */
	bool is_free;
};

struct ssam_mp_block {
	struct ssam_mp_chunk *free_list;
	struct ssam_mp_chunk *alloc_list;
	struct ssam_mp_block *next;

	/* The memory pool block's start virtual address */
	char *virt_start;

	/* The memory pool block's start physical address */
	char *phys_start;

	/* Total size of the memory pool block */
	uint64_t size;

	/* Total size of the memory pool block that be allocated */
	uint64_t alloc_size;

	/* Total size of the memory pool block be allocated that program can be use */
	uint64_t alloc_prog_size;
};

struct ssam_mempool {
	/* Total size of the memory pool */
	uint64_t size;
	uint64_t extra_size;
	uint64_t extra_size_limit;
	struct ssam_mp_block *blk_list;

	/* The memory pool's start virtual address */
	char *virt;
	pthread_mutex_t lock;
};


static uint64_t
ssam_mp_align_up(uint64_t size)
{
	/* Aligin to sizeof long */
	return (size + sizeof(long) - 1) & (~(sizeof(long) - 1));
}

static inline void
ssam_mp_lock(struct ssam_mempool *mp)
{
	pthread_mutex_lock(&mp->lock);
}

static inline void
ssam_mp_unlock(struct ssam_mempool *mp)
{
	pthread_mutex_unlock(&mp->lock);
}

static void
ssam_mp_init_block(struct ssam_mp_block *blk, uint64_t size)
{
	blk->size = size;
	blk->alloc_size = 0;
	blk->alloc_prog_size = 0;
	blk->free_list = (struct ssam_mp_chunk *)blk->virt_start;
	blk->free_list->is_free = true;
	blk->free_list->size = size;
	blk->free_list->prev = NULL;
	blk->free_list->next = NULL;
	blk->alloc_list = NULL;
}

static inline void
ssam_mp_list_insert(struct ssam_mp_chunk **head, struct ssam_mp_chunk *ck)
{
	struct ssam_mp_chunk *hd = *head;

	ck->prev = NULL;
	ck->next = hd;
	if (hd != NULL) {
		hd->prev = ck;
	}
	*head = ck;
}

static void
ssam_mp_list_delete(struct ssam_mp_chunk **head, struct ssam_mp_chunk *ck)
{
	if (ck->prev == NULL) {
		*head = ck->next;
		if (ck->next != NULL) {
			ck->next->prev = NULL;
		}
	} else {
		ck->prev->next = ck->next;
		if (ck->next != NULL) {
			ck->next->prev = ck->prev;
		}
	}
}

static struct
ssam_mp_block *ssam_mp_find_block(struct ssam_mempool *mp, void *p)
{
	struct ssam_mp_block *blk = mp->blk_list;

	while (blk != NULL) {
		if ((blk->virt_start <= (char *)p) &&
		    ((blk->virt_start + blk->size) > (char *)p)) {
			break;
		}
		blk = blk->next;
	}

	return blk;
}

static void
ssam_mp_merge_chunk(struct ssam_mp_block *blk, struct ssam_mp_chunk *ck)
{
	struct ssam_mp_chunk *free_mem = ck;
	struct ssam_mp_chunk *next = ck;

	/* Traversal free memory backward */
	while (next->is_free) {
		free_mem = next;
		if (((char *)next - MP_CK_CB_LEN) <= blk->virt_start) {
			break;
		}
		next = *(struct ssam_mp_chunk **)((char *)next - MP_CK_END_LEN);
	}

	/* Traverse free memory forward */
	next = (struct ssam_mp_chunk *)((char *)free_mem + free_mem->size);
	while (((char *)next <= blk->virt_start + blk->size - MP_CK_HEADER_LEN) && next->is_free) {
		ssam_mp_list_delete(&blk->free_list, next);
		free_mem->size += next->size;
		next = (struct ssam_mp_chunk *)((char *)next + next->size);
	}

	/* Merge free memory */
	*(struct ssam_mp_chunk **)((char *)free_mem + free_mem->size - MP_CK_END_LEN) = free_mem;

	return;
}

static int
ssam_mp_get_mem_block(uint64_t start_virt_addr, uint64_t len, uint64_t *phys_addr,
		      uint64_t *blk_size)
{
	uint64_t virt0, virt1, phys0, phys1;
	uint64_t phys_len;

	if ((len % VALUE_2MB) != 0) {
		SPDK_ERRLOG("Memory len %lu not align to %llu\n", len, VALUE_2MB);
		return -EINVAL;
	}

	virt0 = start_virt_addr;
	virt1 = start_virt_addr;
	phys0 = spdk_vtophys((void *)virt0, NULL);
	if (phys0 == SSAM_SPDK_VTOPHYS_ERROR) {
		SPDK_ERRLOG("Error translating virt0 address %lu\n", virt0);
		return -EINVAL;
	}

	/*
	 * Find a piece of memory with consecutive physical address,
	 * the memory got by spdk_dma_malloc is aligned by VALUE_2MB,
	 * this ensures that the physical addresses are consecutive
	 * within the VALUE_2MB length range.
	 */
	for (phys_len = VALUE_2MB; phys_len < len; phys_len += VALUE_2MB) {
		virt1 += VALUE_2MB;
		phys1 = spdk_vtophys((void *)virt1, NULL);
		if (phys1 == SSAM_SPDK_VTOPHYS_ERROR) {
			SPDK_ERRLOG("Error translating virt1 address %lu\n", virt1);
			break;
		}
		if ((long)(phys1 - phys0) != (long)(virt1 - virt0)) {
			SPDK_DEBUGLOG(ssam_mempool, "End of consecutive physical addresses\n");
			break;
		}
	}

	*phys_addr = spdk_vtophys((void *)virt0, NULL);
	*blk_size = phys_len;

	return 0;
}

static void
ssam_mp_free_blk_heads(struct ssam_mp_block *blk)
{
	struct ssam_mp_block *blk_head = blk;
	struct ssam_mp_block *l_mp = NULL;

	while (blk_head != NULL) {
		l_mp = blk_head;
		blk_head = blk_head->next;
		free(l_mp);
		l_mp = NULL;
	}
}

static int
ssam_mp_insert_blocks(struct ssam_mempool *mp, uint64_t size)
{
	struct ssam_mp_block *blk_head = NULL;
	uint64_t blk_size = 0;
	uint64_t remain_size = size;
	uint64_t phys = 0;
	char *virt_addr = mp->virt;
	int rc;

	/* Find memory blocks and insert them to memory pool list */
	while (remain_size > 0) {
		rc = ssam_mp_get_mem_block((uint64_t)virt_addr, remain_size, &phys, &blk_size);
		if (rc != 0) {
			ssam_mp_free_blk_heads(mp->blk_list);
			return -ENOMEM;
		}
		blk_head = (struct ssam_mp_block *)malloc(sizeof(struct ssam_mp_block));
		if (blk_head == NULL) {
			SPDK_ERRLOG("mempool block head malloc failed, mempool create failed\n");
			ssam_mp_free_blk_heads(mp->blk_list);
			return -ENOMEM;
		}
		blk_head->virt_start = virt_addr;
		blk_head->phys_start = (char *)phys;
		ssam_mp_init_block(blk_head, blk_size);
		blk_head->next = mp->blk_list;
		mp->blk_list = blk_head;
		mp->size += blk_size;
		virt_addr += blk_size;
		remain_size -= blk_size;
	}

	if (mp->size != size) {
		SPDK_ERRLOG("mempool size lost, mempool create failed\n");
		ssam_mp_free_blk_heads(mp->blk_list);
		return -ENOMEM;
	}

	return 0;
}

static int
ssam_check_mempool_size(uint64_t size, uint64_t extra_size_limit)
{
	if (size == 0) {
		SPDK_ERRLOG("Memory pool size can not be %lu, mempool create failed\n", size);
		return -EINVAL;
	}

	if (size < VALUE_2MB) {
		SPDK_ERRLOG("Memory pool size can not less than %llu, actually %lu, mempool create failed\n",
			    VALUE_2MB, size);
		return -EINVAL;
	}

	if (extra_size_limit > VALUE_1GB) {
		SPDK_ERRLOG("Memory pool extra size can not greater than %llu, actually %lu, mempool create failed\n",
			    VALUE_1GB, extra_size_limit);
		return -EINVAL;
	}

	return 0;
}

ssam_mempool_t *
ssam_mempool_create(uint64_t size, uint64_t extra_size_limit)
{
	struct ssam_mempool *mp = NULL;
	uint64_t mp_size = size;
	uint64_t mp_extra_size_limit = extra_size_limit;
	void *virt = NULL;
	int rc;

	rc = ssam_check_mempool_size(mp_size, mp_extra_size_limit);
	if (rc != 0) {
		return NULL;
	}

	if ((mp_size % VALUE_2MB) != 0) {
		SPDK_NOTICELOG("Memory pool size %lu not align to %llu, Align down memory pool size to %llu\n",
			       mp_size,
			       VALUE_2MB, mp_size & ~(VALUE_2MB - 1));
		mp_size = mp_size & ~(VALUE_2MB - 1);
	}

	if ((mp_extra_size_limit % VALUE_2MB) != 0) {
		SPDK_NOTICELOG("Memory pool extra size %lu not align to %llu, Align down memory pool size to %llu\n",
			       mp_extra_size_limit, VALUE_2MB, mp_extra_size_limit & ~(VALUE_2MB - 1));
		mp_extra_size_limit = mp_extra_size_limit & ~(VALUE_2MB - 1);
	}

	mp = (struct ssam_mempool *)calloc(1, sizeof(struct ssam_mempool));
	if (mp == NULL) {
		SPDK_ERRLOG("mempool head malloc failed, mempool create failed\n");
		return NULL;
	}

	virt = spdk_dma_malloc(mp_size, VALUE_2MB, NULL);
	if (virt == NULL) {
		SPDK_ERRLOG("spdk_dma_malloc failed, mempool create failed\n");
		free(mp);
		mp = NULL;
		return NULL;
	}
	mp->virt = (char *)virt;

	rc = ssam_mp_insert_blocks(mp, mp_size);
	if (rc != 0) {
		free(mp);
		mp = NULL;
		spdk_dma_free(virt);
		return NULL;
	}

	mp->extra_size = 0;
	mp->extra_size_limit = mp_extra_size_limit;
	pthread_mutex_init(&mp->lock, NULL);

	return (ssam_mempool_t *)mp;
}

static void ssam_mp_split_block(struct ssam_mp_block *blk, struct ssam_mp_chunk *free_mem,
				struct ssam_mp_chunk *allocated, uint64_t size)
{
	*free_mem = *allocated;
	free_mem->size -= size;
	if (free_mem->size < MP_CK_END_LEN) {
		SPDK_ERRLOG("split size %lu < MP_CK_END_LEN %lu, cannot split chunk.\n",
				(unsigned long)free_mem->size, (unsigned long)MP_CK_END_LEN);
		return;
	}
	*(struct ssam_mp_chunk **)((char *)free_mem + free_mem->size - MP_CK_END_LEN) = free_mem;

	if (free_mem->prev == NULL) {
		blk->free_list = free_mem;
	} else {
		free_mem->prev->next = free_mem;
	}

	if (free_mem->next != NULL) {
		free_mem->next->prev = free_mem;
	}

	allocated->is_free = false;
	allocated->size = size;

	*(struct ssam_mp_chunk **)((char *)allocated + size - MP_CK_END_LEN) = allocated;
}

static void *
ssam_mp_alloc_mem_from_block(struct ssam_mp_block *blk, uint64_t size,
			     uint64_t *phys_addr)
{
	struct ssam_mp_chunk *free_mem = NULL;
	struct ssam_mp_chunk *allocated = NULL;
	char *alloc = NULL;

	free_mem = blk->free_list;
	while (free_mem != NULL) {
		if (free_mem->size < size) {
			free_mem = free_mem->next;
			continue;
		}

		allocated = free_mem;
		if ((free_mem->size - size) > MP_CK_CB_LEN) {
			/* If enough mem in free chunk, split it */
			free_mem = (struct ssam_mp_chunk *)((char *)allocated + size);
			ssam_mp_split_block(blk, free_mem, allocated, size);
		} else {
			/* If no enough mem in free chunk, all will be allocated */
			ssam_mp_list_delete(&blk->free_list, allocated);
			allocated->is_free = false;
		}
		ssam_mp_list_insert(&blk->alloc_list, allocated);

		blk->alloc_size += allocated->size;
		blk->alloc_prog_size += allocated->size - (uint64_t)MP_CK_CB_LEN;
		alloc = (char *)allocated + MP_CK_HEADER_LEN;
		if (phys_addr != NULL) {
			*phys_addr = (uint64_t)blk->phys_start + (uint64_t)(alloc - blk->virt_start);
		}

		return (void *)alloc;
	}

	return NULL;
}

static bool
ssam_mp_check_consecutive_mem(void *start_addr, uint64_t len)
{
	uint64_t phys_start;
	uint64_t phys_end;

	phys_start = spdk_vtophys(start_addr, NULL);
	phys_end = spdk_vtophys((void *)((uint64_t)start_addr + len - 1), NULL);
	if ((phys_end - phys_start) == (len - 1)) {
		return true;
	}

	return false;
}

/* alloc dma memory from hugepage directly */
static void *
ssam_mp_dma_alloc(struct ssam_mempool *mp, uint64_t size, uint64_t *phys)
{
	struct ssam_mp_dma_mem *alloc;
	size_t len = size + sizeof(struct ssam_mp_dma_mem);
	uint64_t phys_addr = 0;

	if (mp->extra_size + len > mp->extra_size_limit) {
		SPDK_INFOLOG(ssam_mempool, "spdk_dma_malloc alloc failed, extra_size(%lu) size(%zu) limit(%lu).\n",
			     mp->extra_size, len, mp->extra_size_limit);
		return NULL;
	}

	alloc = (struct ssam_mp_dma_mem *)spdk_dma_malloc(len, 0, NULL);
	if (alloc == NULL) {
		SPDK_INFOLOG(ssam_mempool, "spdk_dma_malloc alloc failed, len %zu.\n", len);
		return NULL;
	}
	if (!ssam_mp_check_consecutive_mem((void *)alloc->mem, size)) {
		SPDK_ERRLOG("spdk_dma_malloc alloc failed, no consecutive mem, len %lu.\n", size);
		spdk_dma_free(alloc);
		return NULL;
	}
	phys_addr = spdk_vtophys((const void *)alloc->mem, NULL);
	if (phys_addr == SSAM_SPDK_VTOPHYS_ERROR) {
		SPDK_ERRLOG("Error translating spdk_dma_malloc address %lu\n", phys_addr);
		spdk_dma_free(alloc);
		return NULL;
	}
	*phys = phys_addr;
	alloc->magic = SSAM_DMA_MEM_MAGIC;
	alloc->size = len;
	mp->extra_size += len;

	return (void *)alloc->mem;
}

static void
ssam_mp_dma_free(struct ssam_mempool *mp, const void *ptr)
{
	struct ssam_mp_dma_mem *free_mem;
	uint64_t addr = (uint64_t)ptr;

	if (addr <= sizeof(struct ssam_mp_dma_mem)) {
		SPDK_ERRLOG("ssam_mp_dma_free mem address err\n");
		return;
	}

	free_mem = (struct ssam_mp_dma_mem *)(addr - sizeof(struct ssam_mp_dma_mem));
	if (free_mem->magic == SSAM_DMA_MEM_MAGIC) {
		mp->extra_size -= free_mem->size;
		spdk_dma_free(free_mem);
	} else {
		SPDK_ERRLOG("ssam_mp_dma_free magic err, magic is %lx\n", free_mem->magic);
	}
	return;
}

static void *
ssam_mp_alloc_mem_from_blocks(struct ssam_mempool *mp, uint64_t size,
			      uint64_t *phys_addr)
{
	struct ssam_mp_block *blk = mp->blk_list;
	void *alloc = NULL;

	while (blk != NULL) {
		if (size > (blk->size - blk->alloc_size)) {
			blk = blk->next;
			continue;
		}

		alloc = ssam_mp_alloc_mem_from_block(blk, size, phys_addr);
		if (alloc != NULL) {
			return alloc;
		}

		blk = blk->next;
	}
	SPDK_INFOLOG(ssam_mempool, "ssam mempool no enough memory, alloc size %lu\n", size);
	alloc = ssam_mp_dma_alloc(mp, size, phys_addr);

	return alloc;
}

void *
ssam_mempool_alloc(ssam_mempool_t *mp, uint64_t size, uint64_t *phys_addr)
{
	struct ssam_mempool *l_mp = (struct ssam_mempool *)mp;
	void *alloc = NULL;
	uint64_t need_size;

	if (phys_addr == NULL) {
		SPDK_ERRLOG("alloc phys_addr pointer is NULL\n");
		return NULL;
	}

	if (l_mp == NULL) {
		SPDK_ERRLOG("alloc mp pointer is NULL\n");
		return NULL;
	}

	if (size == 0) {
		SPDK_ERRLOG("Memory pool size can not be %lu, mempool alloc failed\n", size);
		return NULL;
	}

	need_size = ssam_mp_align_up(size + MP_CK_CB_LEN);

	ssam_mp_lock(l_mp);
	if (need_size > l_mp->size) {
		SPDK_INFOLOG(ssam_mempool, "No enough memory in mempool, need %lu, actually %lu\n",
			     need_size, l_mp->size);
		alloc = ssam_mp_dma_alloc(l_mp, size, phys_addr);
		ssam_mp_unlock(l_mp);
		return alloc;
	}

	alloc = ssam_mp_alloc_mem_from_blocks(l_mp, need_size, phys_addr);

	ssam_mp_unlock(l_mp);

	return alloc;
}

void
ssam_mempool_free(ssam_mempool_t *mp, void *ptr)
{
	struct ssam_mempool *l_mp = (struct ssam_mempool *)mp;
	struct ssam_mp_block *blk = NULL;
	struct ssam_mp_chunk *ck = NULL;

	if (l_mp == NULL) {
		SPDK_ERRLOG("free mp pointer is NULL\n");
		return;
	}

	if (ptr == NULL) {
		SPDK_ERRLOG("free ptr pointer is NULL\n");
		return;
	}

	ssam_mp_lock(l_mp);

	blk = ssam_mp_find_block(l_mp, ptr);
	if (blk == NULL) {
		ssam_mp_dma_free(l_mp, ptr);
		ssam_mp_unlock(l_mp);
		return;
	}

	ck = (struct ssam_mp_chunk *)((char *)ptr - MP_CK_HEADER_LEN);

	ssam_mp_list_delete(&blk->alloc_list, ck);
	ssam_mp_list_insert(&blk->free_list, ck);
	ck->is_free = true;

	blk->alloc_size -= ck->size;
	blk->alloc_prog_size -= ck->size - (uint64_t)MP_CK_CB_LEN;

	ssam_mp_merge_chunk(blk, ck);

	ssam_mp_unlock(l_mp);

	return;
}

void
ssam_mempool_destroy(ssam_mempool_t *mp)
{
	struct ssam_mempool *l_mp = (struct ssam_mempool *)mp;

	if (l_mp == NULL) {
		SPDK_ERRLOG("destroy mp pointer is NULL\n");
		return;
	}

	if (l_mp->virt == NULL) {
		SPDK_ERRLOG("destroy mp->virt pointer is NULL\n");
		return;
	}

	ssam_mp_lock(l_mp);
	ssam_mp_free_blk_heads(l_mp->blk_list);
	spdk_dma_free(l_mp->virt);
	ssam_mp_unlock(l_mp);
	pthread_mutex_destroy(&l_mp->lock);
	free(l_mp);
	l_mp = NULL;

	return;
}

static uint64_t
ssam_mp_total_memory(ssam_mempool_t *mp)
{
	struct ssam_mempool *l_mp = (struct ssam_mempool *)mp;
	uint64_t size;

	ssam_mp_lock(l_mp);
	size = l_mp->size;
	ssam_mp_unlock(l_mp);

	return size;
}

static uint64_t
ssam_mp_total_used_memory(ssam_mempool_t *mp)
{
	struct ssam_mempool *l_mp = (struct ssam_mempool *)mp;
	struct ssam_mp_block *blk = NULL;
	uint64_t total = 0;

	ssam_mp_lock(l_mp);

	blk = l_mp->blk_list;
	while (blk != NULL) {
		total += blk->alloc_size;
		blk = blk->next;
	}

	ssam_mp_unlock(l_mp);

	return total;
}

static uint32_t
ssam_mp_alloc_num(ssam_mempool_t *mp)
{
	struct ssam_mempool *l_mp = (struct ssam_mempool *)mp;
	struct ssam_mp_block *blk = NULL;
	struct ssam_mp_chunk *alloc = NULL;
	uint32_t total = 0;

	ssam_mp_lock(l_mp);

	blk = l_mp->blk_list;
	while (blk != NULL) {
		alloc = blk->alloc_list;
		while (alloc != NULL) {
			if (total == UINT32_MAX) {
				SPDK_ERRLOG("mp alloc num out of bound\n");
				ssam_mp_unlock(l_mp);
				return total;
			}
			total++;
			alloc = alloc->next;
		}
		blk = blk->next;
	}

	ssam_mp_unlock(l_mp);

	return total;
}

static uint32_t
ssam_mp_free_num(ssam_mempool_t *mp)
{
	struct ssam_mempool *l_mp = (struct ssam_mempool *)mp;
	struct ssam_mp_block *blk = NULL;
	struct ssam_mp_chunk *free_mem = NULL;
	uint32_t total = 0;

	ssam_mp_lock(l_mp);

	blk = l_mp->blk_list;
	while (blk != NULL) {
		free_mem = blk->free_list;
		while (free_mem != NULL) {
			if (total == UINT32_MAX) {
				SPDK_ERRLOG("mp free num out of bound\n");
				ssam_mp_unlock(l_mp);
				return total;
			}
			total++;
			free_mem = free_mem->next;
		}
		blk = blk->next;
	}

	ssam_mp_unlock(l_mp);

	return total;
}

static uint64_t
ssam_mp_get_greatest_free_size(ssam_mempool_t *mp)
{
	struct ssam_mempool *l_mp = (struct ssam_mempool *)mp;
	struct ssam_mp_block *blk = NULL;
	struct ssam_mp_chunk *free_mem = NULL;
	uint64_t max_size = 0;

	ssam_mp_lock(l_mp);

	blk = l_mp->blk_list;
	while (blk != NULL) {
		free_mem = blk->free_list;
		while (free_mem != NULL) {
			if (max_size < free_mem->size) {
				max_size = free_mem->size;
			}
			free_mem = free_mem->next;
		}
		blk = blk->next;
	}

	ssam_mp_unlock(l_mp);

	return max_size;
}

int
ssam_get_mempool_info(ssam_mempool_t *mp, struct memory_info_stats *info)
{
	struct ssam_mempool *l_mp = (struct ssam_mempool *)mp;

	if (l_mp == NULL || info == NULL) {
		SPDK_ERRLOG("ssam get mempool info mp or info pointer is NULL\n");
		return -EINVAL;
	}

	info->total_size = ssam_mp_total_memory(l_mp);
	info->used_size = ssam_mp_total_used_memory(l_mp);
	info->free_size = info->total_size - info->used_size;
	info->greatest_free_size = ssam_mp_get_greatest_free_size(l_mp);
	info->alloc_count = ssam_mp_alloc_num(l_mp);
	info->free_count = ssam_mp_free_num(l_mp);

	return 0;
}
SPDK_LOG_REGISTER_COMPONENT(ssam_mempool)
