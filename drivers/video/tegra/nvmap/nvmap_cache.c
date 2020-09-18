/*
 * drivers/video/tegra/nvmap/nvmap_cache.c
 *
 * Copyright (c) 2011-2020, NVIDIA CORPORATION. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#define pr_fmt(fmt)	"nvmap: %s() " fmt, __func__

#include <linux/highmem.h>
#include <linux/io.h>
#include <linux/debugfs.h>
#include <linux/of.h>
#include <linux/version.h>
#include <linux/tegra-cache.h>
#if KERNEL_VERSION(4, 15, 0) > LINUX_VERSION_CODE
#include <soc/tegra/chip-id.h>
#else
#include <soc/tegra/fuse.h>
#endif

#include <trace/events/nvmap.h>

#include "nvmap_priv.h"

#ifndef CONFIG_NVMAP_CACHE_MAINT_BY_SET_WAYS
/* This is basically the L2 cache size but may be tuned as per requirement */
size_t cache_maint_inner_threshold = SIZE_MAX;
int nvmap_cache_maint_by_set_ways;
#else
int nvmap_cache_maint_by_set_ways = 1;
size_t cache_maint_inner_threshold = 8 * SZ_2M;
#endif

static struct static_key nvmap_disable_vaddr_for_cache_maint;


/*
 * FIXME:
 *
 *   __clean_dcache_page() is only available on ARM64 (well, we haven't
 *   implemented it on ARMv7).
 */
void nvmap_clean_cache_page(struct page *page)
{
	__clean_dcache_page(page);
}

void nvmap_clean_cache(struct page **pages, int numpages)
{
	int i;

	/* Not technically a flush but that's what nvmap knows about. */
	nvmap_stats_inc(NS_CFLUSH_DONE, numpages << PAGE_SHIFT);
	trace_nvmap_cache_flush(numpages << PAGE_SHIFT,
		nvmap_stats_read(NS_ALLOC),
		nvmap_stats_read(NS_CFLUSH_RQ),
		nvmap_stats_read(NS_CFLUSH_DONE));

	for (i = 0; i < numpages; i++)
		nvmap_clean_cache_page(pages[i]);
}

void inner_cache_maint(unsigned int op, void *vaddr, size_t size)
{
	if (op == NVMAP_CACHE_OP_WB_INV)
		__dma_flush_area(vaddr, size);
	else if (op == NVMAP_CACHE_OP_INV)
		__dma_map_area(vaddr, size, DMA_FROM_DEVICE);
	else
		__dma_map_area(vaddr, size, DMA_TO_DEVICE);
}

static void heap_page_cache_maint(
	struct nvmap_handle *h, unsigned long start, unsigned long end,
	unsigned int op, bool inner, bool outer, bool clean_only_dirty)
{
	if (h->userflags & NVMAP_HANDLE_CACHE_SYNC) {
		/*
		 * zap user VA->PA mappings so that any access to the pages
		 * will result in a fault and can be marked dirty
		 */
		nvmap_handle_mkclean(h, start, end-start);
		nvmap_zap_handle(h, start, end - start);
	}

	if (static_key_false(&nvmap_disable_vaddr_for_cache_maint))
		goto per_page_cache_maint;

	if (inner) {
		if (!h->vaddr) {
			if (__nvmap_mmap(h))
				__nvmap_munmap(h, h->vaddr);
			else
				goto per_page_cache_maint;
		}
		/* Fast inner cache maintenance using single mapping */
		inner_cache_maint(op, h->vaddr + start, end - start);
		if (!outer)
			return;
		/* Skip per-page inner maintenance in loop below */
		inner = false;

	}
per_page_cache_maint:

	while (start < end) {
		struct page *page;
		phys_addr_t paddr;
		unsigned long next;
		unsigned long off;
		size_t size;
		int ret;

		page = nvmap_to_page(h->pgalloc.pages[start >> PAGE_SHIFT]);
		next = min(((start + PAGE_SIZE) & PAGE_MASK), end);
		off = start & ~PAGE_MASK;
		size = next - start;
		paddr = page_to_phys(page) + off;

		ret = nvmap_cache_maint_phys_range(op, paddr, paddr + size,
				inner, outer);
		BUG_ON(ret != 0);
		start = next;
	}
}

struct cache_maint_op {
	phys_addr_t start;
	phys_addr_t end;
	unsigned int op;
	struct nvmap_handle *h;
	bool inner;
	bool outer;
	bool clean_only_dirty;
};

int nvmap_cache_maint_phys_range(unsigned int op, phys_addr_t pstart,
		phys_addr_t pend, int inner, int outer)
{
	unsigned long kaddr;
	struct vm_struct *area = NULL;
	phys_addr_t loop;

	if (!inner)
		goto do_outer;

	area = alloc_vm_area(PAGE_SIZE, NULL);
	if (!area)
		return -ENOMEM;
	kaddr = (ulong)area->addr;

	loop = pstart;
	while (loop < pend) {
		phys_addr_t next = (loop + PAGE_SIZE) & PAGE_MASK;
		void *base = (void *)kaddr + (loop & ~PAGE_MASK);

		next = min(next, pend);
		ioremap_page_range(kaddr, kaddr + PAGE_SIZE,
			loop, PG_PROT_KERNEL);
		inner_cache_maint(op, base, next - loop);
		loop = next;
		unmap_kernel_range(kaddr, PAGE_SIZE);
	}

	free_vm_area(area);
do_outer:
	return 0;
}

static int do_cache_maint(struct cache_maint_op *cache_work)
{
	phys_addr_t pstart = cache_work->start;
	phys_addr_t pend = cache_work->end;
	int err = 0;
	struct nvmap_handle *h = cache_work->h;
	unsigned int op = cache_work->op;

	if (!h || !h->alloc)
		return -EFAULT;

	wmb();
	if (h->flags == NVMAP_HANDLE_UNCACHEABLE ||
	    h->flags == NVMAP_HANDLE_WRITE_COMBINE || pstart == pend)
		goto out;

	trace_nvmap_cache_maint(h->owner, h, pstart, pend, op, pend - pstart);
	if (pstart > h->size || pend > h->size) {
		pr_warn("cache maintenance outside handle\n");
		err = -EINVAL;
		goto out;
	}

	if (h->heap_pgalloc) {
		heap_page_cache_maint(h, pstart, pend, op, true,
			(h->flags == NVMAP_HANDLE_INNER_CACHEABLE) ?
			false : true, cache_work->clean_only_dirty);
		goto out;
	}

	pstart += h->carveout->base;
	pend += h->carveout->base;

	err = nvmap_cache_maint_phys_range(op, pstart, pend, true,
			h->flags != NVMAP_HANDLE_INNER_CACHEABLE);

out:
	if (!err) {
		nvmap_stats_inc(NS_CFLUSH_DONE, pend - pstart);
	}

	trace_nvmap_cache_flush(pend - pstart,
		nvmap_stats_read(NS_ALLOC),
		nvmap_stats_read(NS_CFLUSH_RQ),
		nvmap_stats_read(NS_CFLUSH_DONE));

	return 0;
}

__weak void nvmap_handle_get_cacheability(struct nvmap_handle *h,
		bool *inner, bool *outer)
{
	*inner = h->flags == NVMAP_HANDLE_CACHEABLE ||
		 h->flags == NVMAP_HANDLE_INNER_CACHEABLE;
	*outer = h->flags == NVMAP_HANDLE_CACHEABLE;
}

int __nvmap_do_cache_maint(struct nvmap_client *client,
			struct nvmap_handle *h,
			unsigned long start, unsigned long end,
			unsigned int op, bool clean_only_dirty)
{
	int err;
	struct cache_maint_op cache_op;

	h = nvmap_handle_get(h);
	if (!h)
		return -EFAULT;

	if ((start >= h->size) || (end > h->size)) {
		pr_debug("%s start: %ld end: %ld h->size: %zu\n", __func__,
				start, end, h->size);
		nvmap_handle_put(h);
		return -EFAULT;
	}

	if (!(h->heap_type & nvmap_dev->cpu_access_mask)) {
		pr_debug("%s heap_type %u access_mask 0x%x\n", __func__,
				h->heap_type, nvmap_dev->cpu_access_mask);
		nvmap_handle_put(h);
		return -EPERM;
	}

	nvmap_kmaps_inc(h);
	if (op == NVMAP_CACHE_OP_INV)
		op = NVMAP_CACHE_OP_WB_INV;

	/* clean only dirty is applicable only for Write Back operation */
	if (op != NVMAP_CACHE_OP_WB)
		clean_only_dirty = false;

	cache_op.h = h;
	cache_op.start = start ? start : 0;
	cache_op.end = end ? end : h->size;
	cache_op.op = op;
	nvmap_handle_get_cacheability(h, &cache_op.inner, &cache_op.outer);
	cache_op.clean_only_dirty = clean_only_dirty;

	nvmap_stats_inc(NS_CFLUSH_RQ, end - start);
	err = do_cache_maint(&cache_op);
	nvmap_kmaps_dec(h);
	nvmap_handle_put(h);
	return err;
}

int __nvmap_cache_maint(struct nvmap_client *client,
			       struct nvmap_cache_op_64 *op)
{
	struct vm_area_struct *vma;
	struct nvmap_vma_priv *priv;
	struct nvmap_handle *handle;
	unsigned long start;
	unsigned long end;
	int err = 0;

	if (!op->addr || op->op < NVMAP_CACHE_OP_WB ||
	    op->op > NVMAP_CACHE_OP_WB_INV)
		return -EINVAL;

	handle = nvmap_handle_get_from_fd(op->handle);
	if (!handle)
		return -EINVAL;

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 4, 0)
	down_read(&current->mm->mmap_sem);
#else
	down_read(&current->mm->mmap_lock);
#endif

	vma = find_vma(current->active_mm, (unsigned long)op->addr);
	if (!vma || !is_nvmap_vma(vma) ||
	    (ulong)op->addr < vma->vm_start ||
	    (ulong)op->addr >= vma->vm_end ||
	    op->len > vma->vm_end - (ulong)op->addr) {
		err = -EADDRNOTAVAIL;
		goto out;
	}

	priv = (struct nvmap_vma_priv *)vma->vm_private_data;

	if (priv->handle != handle) {
		err = -EFAULT;
		goto out;
	}

	start = (unsigned long)op->addr - vma->vm_start +
		(vma->vm_pgoff << PAGE_SHIFT);
	end = start + op->len;

	err = __nvmap_do_cache_maint(client, priv->handle, start, end, op->op,
				     false);
out:
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 4, 0)
	up_read(&current->mm->mmap_sem);
#else
	up_read(&current->mm->mmap_lock);
#endif
	nvmap_handle_put(handle);
	return err;
}

/*
 * Perform cache op on the list of memory regions within passed handles.
 * A memory region within handle[i] is identified by offsets[i], sizes[i]
 *
 * sizes[i] == 0  is a special case which causes handle wide operation,
 * this is done by replacing offsets[i] = 0, sizes[i] = handles[i]->size.
 * So, the input arrays sizes, offsets  are not guaranteed to be read-only
 *
 * This will optimze the op if it can.
 * In the case that all the handles together are larger than the inner cache
 * maint threshold it is possible to just do an entire inner cache flush.
 *
 * NOTE: this omits outer cache operations which is fine for ARM64
 */
static int __nvmap_do_cache_maint_list(struct nvmap_handle **handles,
				u64 *offsets, u64 *sizes, int op, int nr,
				bool is_32)
{
	int i;
	u64 total = 0;
	u64 thresh = ~0;

	WARN(!IS_ENABLED(CONFIG_ARM64),
		"cache list operation may not function properly");

	if (nvmap_cache_maint_by_set_ways)
		thresh = cache_maint_inner_threshold;

	for (i = 0; i < nr; i++) {
		bool inner, outer;
		u32 *sizes_32 = (u32 *)sizes;
		u64 size = is_32 ? sizes_32[i] : sizes[i];

		nvmap_handle_get_cacheability(handles[i], &inner, &outer);

		if (!inner && !outer)
			continue;

		if ((op == NVMAP_CACHE_OP_WB) && nvmap_handle_track_dirty(handles[i]))
			total += atomic_read(&handles[i]->pgalloc.ndirty);
		else
			total += size ? size : handles[i]->size;
	}

	if (!total)
		return 0;

	/* Full flush in the case the passed list is bigger than our
	 * threshold. */
	if (total >= thresh) {
		for (i = 0; i < nr; i++) {
			if (handles[i]->userflags &
			    NVMAP_HANDLE_CACHE_SYNC) {
				nvmap_handle_mkclean(handles[i], 0,
						     handles[i]->size);
				nvmap_zap_handle(handles[i], 0,
						 handles[i]->size);
			}
		}

		nvmap_stats_inc(NS_CFLUSH_RQ, total);
		nvmap_stats_inc(NS_CFLUSH_DONE, thresh);
		trace_nvmap_cache_flush(total,
					nvmap_stats_read(NS_ALLOC),
					nvmap_stats_read(NS_CFLUSH_RQ),
					nvmap_stats_read(NS_CFLUSH_DONE));
	} else {
		for (i = 0; i < nr; i++) {
			u32 *offs_32 = (u32 *)offsets, *sizes_32 = (u32 *)sizes;
			u64 size = is_32 ? sizes_32[i] : sizes[i];
			u64 offset = is_32 ? offs_32[i] : offsets[i];
			int err;

			size = size ?: handles[i]->size;
			offset = offset ?: 0;
			err = __nvmap_do_cache_maint(handles[i]->owner,
						     handles[i], offset,
						     offset + size,
						     op, false);
			if (err) {
				pr_err("cache maint per handle failed [%d]\n",
						err);
				return err;
			}
		}
	}

	return 0;
}

inline int nvmap_do_cache_maint_list(struct nvmap_handle **handles,
				u64 *offsets, u64 *sizes, int op, int nr,
				bool is_32)
{
	int ret = 0;

	switch (tegra_get_chip_id()) {
	case TEGRA194:
		/*
		 * As io-coherency is enabled by default from T194 onwards,
		 * Don't do cache maint from CPU side. The HW, SCF will do.
		 */
		break;
	default:
		ret = __nvmap_do_cache_maint_list(handles,
					offsets, sizes, op, nr, is_32);
		break;
	}

	return ret;
}

static int cache_inner_threshold_show(struct seq_file *m, void *v)
{
	if (nvmap_cache_maint_by_set_ways)
		seq_printf(m, "%zuB\n", cache_maint_inner_threshold);
	else
		seq_printf(m, "%zuB\n", SIZE_MAX);
	return 0;
}

static int cache_inner_threshold_open(struct inode *inode, struct file *file)
{
	return single_open(file, cache_inner_threshold_show, inode->i_private);
}

static ssize_t cache_inner_threshold_write(struct file *file,
					const char __user *buffer,
					size_t count, loff_t *pos)
{
	int ret;
	struct seq_file *p = file->private_data;
	char str[] = "0123456789abcdef";

	count = min_t(size_t, strlen(str), count);
	if (copy_from_user(str, buffer, count))
		return -EINVAL;

	if (!nvmap_cache_maint_by_set_ways)
		return -EINVAL;

	mutex_lock(&p->lock);
	ret = sscanf(str, "%16zu", &cache_maint_inner_threshold);
	mutex_unlock(&p->lock);
	if (ret != 1)
		return -EINVAL;

	pr_debug("nvmap:cache_maint_inner_threshold is now :%zuB\n",
			cache_maint_inner_threshold);
	return count;
}

static const struct file_operations cache_inner_threshold_fops = {
	.open		= cache_inner_threshold_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
	.write		= cache_inner_threshold_write,
};

int nvmap_cache_debugfs_init(struct dentry *nvmap_root)
{
	struct dentry *cache_root;

	if (!nvmap_root)
		return -ENODEV;

	cache_root = debugfs_create_dir("cache", nvmap_root);
	if (!cache_root)
		return -ENODEV;

	if (nvmap_cache_maint_by_set_ways) {
		debugfs_create_x32("nvmap_cache_maint_by_set_ways",
				   S_IRUSR | S_IWUSR,
				   cache_root,
				   &nvmap_cache_maint_by_set_ways);

	debugfs_create_file("cache_maint_inner_threshold",
			    S_IRUSR | S_IWUSR,
			    cache_root,
			    NULL,
			    &cache_inner_threshold_fops);
	}

	debugfs_create_atomic_t("nvmap_disable_vaddr_for_cache_maint",
				S_IRUSR | S_IWUSR,
				cache_root,
				&nvmap_disable_vaddr_for_cache_maint.enabled);

	return 0;
}
