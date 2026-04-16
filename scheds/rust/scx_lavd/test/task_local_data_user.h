/* SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause) */
/*
 * TL;DR: Small userspace helper header for mapping and updating the scheduler's
 * task-local hint page from the branch test binaries.
 */
#ifndef __SCX_LAVD_TASK_LOCAL_DATA_USER_H
#define __SCX_LAVD_TASK_LOCAL_DATA_USER_H

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/types.h>

#include <bpf/bpf.h>

#define TLD_PAGE_SIZE getpagesize()
#define TLD_PAGE_MASK (~(TLD_PAGE_SIZE - 1))

#define TLD_ROUND_MASK(x, y) ((__typeof__(x))((y) - 1))
#define TLD_ROUND_UP(x, y) ((((x) - 1) | TLD_ROUND_MASK(x, y)) + 1)

#define TLD_ROUND_UP_POWER_OF_TWO(x) \
	(1UL << (sizeof(x) * 8 - __builtin_clzl((x) - 1)))

#ifndef TLD_DYN_DATA_SIZE
#define TLD_DYN_DATA_SIZE 64
#endif

#ifndef TLD_NAME_LEN
#define TLD_NAME_LEN 62
#endif

#define TLD_MAX_DATA_CNT (TLD_PAGE_SIZE / sizeof(struct tld_metadata) - 1)

#ifndef sys_gettid
#define sys_gettid() syscall(SYS_gettid)
#endif

typedef struct {
	int16_t off;
} tld_key_t;

struct tld_metadata {
	char name[TLD_NAME_LEN];
	_Atomic uint16_t size;
};

struct tld_meta_u {
	_Atomic uint16_t cnt;
	uint16_t size;
	struct tld_metadata metadata[];
};

struct tld_data_u {
	uint64_t unused;
	char data[] __attribute__((aligned(8)));
};

struct tld_map_value {
	void *data;
	struct tld_meta_u *meta;
	uint16_t start;
};

static struct tld_meta_u * _Atomic tld_meta_p;
static __thread struct tld_data_u *tld_data_p;

static inline int __tld_init_meta_p(void)
{
	struct tld_meta_u *meta;
	struct tld_meta_u *uninit = NULL;

	meta = aligned_alloc(TLD_PAGE_SIZE, TLD_PAGE_SIZE);
	if (!meta)
		return -ENOMEM;

	memset(meta, 0, TLD_PAGE_SIZE);
	meta->size = TLD_DYN_DATA_SIZE;

	if (!atomic_compare_exchange_strong(&tld_meta_p, &uninit, meta))
		free(meta);

	return 0;
}

static inline int __tld_init_data_p(int map_fd)
{
	struct tld_map_value map_val;
	struct tld_data_u *data;
	size_t size, size_pot;
	int err;
	int tid_fd = -1;

	tid_fd = syscall(SYS_pidfd_open, sys_gettid(), O_EXCL);
	if (tid_fd < 0)
		return -errno;

	size = tld_meta_p->size + sizeof(struct tld_data_u);
	size_pot = TLD_ROUND_UP_POWER_OF_TWO(size);
	data = aligned_alloc(size_pot, size_pot);
	if (!data) {
		close(tid_fd);
		return -ENOMEM;
	}

	memset(data, 0, size_pot);
	map_val.data = (void *)(TLD_PAGE_MASK & (intptr_t)data);
	map_val.start = (~TLD_PAGE_MASK & (intptr_t)data) +
		sizeof(struct tld_data_u);
	map_val.meta = tld_meta_p;

	err = bpf_map_update_elem(map_fd, &tid_fd, &map_val, 0);
	close(tid_fd);
	if (err) {
		err = -errno;
		free(data);
		return err;
	}

	tld_data_p = data;
	return 0;
}

static inline tld_key_t __tld_create_key(const char *name, size_t size, int dyn_data)
{
	int i, sz;
	int off = 0;
	uint16_t cnt;

	if (!tld_meta_p && __tld_init_meta_p())
		return (tld_key_t){-ENOMEM};

	for (i = 0; i < (int)TLD_MAX_DATA_CNT; i++) {
retry:
		cnt = atomic_load(&tld_meta_p->cnt);
		if (i < cnt) {
			while (!(sz = atomic_load(&tld_meta_p->metadata[i].size)))
				sched_yield();

			if (!strncmp(tld_meta_p->metadata[i].name, name, TLD_NAME_LEN))
				return (tld_key_t){-EEXIST};

			off += TLD_ROUND_UP(sz, 8);
			continue;
		}

		if (dyn_data) {
			if (off + TLD_ROUND_UP(size, 8) > tld_meta_p->size)
				return (tld_key_t){-E2BIG};
		} else {
			if (off + TLD_ROUND_UP(size, 8) >
			    TLD_PAGE_SIZE - sizeof(struct tld_data_u))
				return (tld_key_t){-E2BIG};
			tld_meta_p->size += TLD_ROUND_UP(size, 8);
		}

		if (!atomic_compare_exchange_strong(&tld_meta_p->cnt, &cnt, cnt + 1))
			goto retry;

		strscpy(tld_meta_p->metadata[i].name, name);
		atomic_store(&tld_meta_p->metadata[i].size, size);
		return (tld_key_t){(int16_t)off};
	}

	return (tld_key_t){-ENOSPC};
}

#define TLD_DEFINE_KEY(key, name, size)				\
	tld_key_t key;						\
	__attribute__((constructor(101)))			\
	static void __tld_define_key_##key(void)		\
	{							\
		key = __tld_create_key(name, size, 0);		\
	}

static inline int tld_key_is_err(tld_key_t key)
{
	return key.off < 0;
}

static inline void *tld_get_data(int map_fd, tld_key_t key)
{
	if (!tld_meta_p)
		return NULL;

	if (!tld_data_p && __tld_init_data_p(map_fd))
		return NULL;

	if (tld_key_is_err(key))
		return NULL;

	return tld_data_p->data + key.off;
}

static inline void tld_free(void)
{
	if (tld_data_p) {
		free(tld_data_p);
		tld_data_p = NULL;
	}
}

#endif
