/* Copyright (C) 2008-2012 Freescale Semiconductor, Inc.
 * Authors: Andy Fleming <afleming@freescale.com>
 *	    Timur Tabi <timur@freescale.com>
 *	    Geoff Thorpe <Geoff.Thorpe@freescale.com>
 *
 * This file is licensed under the terms of the GNU General Public License
 * version 2.  This program is licensed "as is" without any warranty of any
 * kind, whether express or implied.
 */

#include <linux/fsl_usdpaa.h>
#include <linux/fsl_qman.h>
#include <linux/fsl_bman.h>

#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/mm.h>
#include <linux/of.h>
#include <linux/memblock.h>
#include <linux/slab.h>

/* Physical address range of the memory reservation, exported for mm/mem.c */
static u64 phys_start;
static u64 phys_size;
/* PFN versions of the above */
static unsigned long pfn_start;
static unsigned long pfn_size;

/* Memory reservations are manipulated under this spinlock (which is why 'refs'
 * isn't atomic_t). */
static DEFINE_SPINLOCK(mem_lock);

/* The range of TLB1 indices */
static unsigned int first_tlb;
static unsigned int num_tlb;
static unsigned int current_tlb; /* loops around for fault handling */

/* Memory reservation is represented as a list of 'mem_fragment's, some of which
 * may be mapped. Unmapped fragments are always merged where possible. */
static LIST_HEAD(mem_list);

struct mem_mapping;

/* Memory fragments are in 'mem_list'. */
struct mem_fragment {
	u64 base;
	u64 len;
	unsigned long pfn_base; /* PFN version of 'base' */
	unsigned long pfn_len; /* PFN version of 'len' */
	unsigned int refs; /* zero if unmapped */
	struct list_head list;
	/* if mapped, flags+name captured at creation time */
	u32 flags;
	char name[USDPAA_DMA_NAME_MAX];
	/* support multi-process locks per-memory-fragment. */
	int has_locking;
	wait_queue_head_t wq;
	struct mem_mapping *owner;
};

/* Mappings of memory fragments in 'struct ctx'. These are created from
 * ioctl(USDPAA_IOCTL_DMA_MAP), though the actual mapping then happens via a
 * mmap(). */
struct mem_mapping {
	struct mem_fragment *frag;
	struct list_head list;
};

/* Per-FD state (which should also be per-process but we don't enforce that) */
struct ctx {
	/* Allocated resources get put here for accounting */
	struct dpa_alloc ids[usdpaa_id_max];
	struct list_head maps;
};

/* Different resource classes */
static const struct alloc_backend {
	enum usdpaa_id_type id_type;
	int (*alloc)(u32 *, u32, u32, int);
	void (*release)(u32 base, unsigned int count);
	const char *acronym;
} alloc_backends[] = {
	{
		.id_type = usdpaa_id_fqid,
		.alloc = qman_alloc_fqid_range,
		.release = qman_release_fqid_range,
		.acronym = "FQID"
	},
	{
		.id_type = usdpaa_id_bpid,
		.alloc = bman_alloc_bpid_range,
		.release = bman_release_bpid_range,
		.acronym = "BPID"
	},
	{
		.id_type = usdpaa_id_qpool,
		.alloc = qman_alloc_pool_range,
		.release = qman_release_pool_range,
		.acronym = "QPOOL"
	},
	{
		.id_type = usdpaa_id_cgrid,
		.alloc = qman_alloc_cgrid_range,
		.release = qman_release_cgrid_range,
		.acronym = "CGRID"
	},
	{
		/* This terminates the array */
		.id_type = usdpaa_id_max
	}
};

/* Helper for ioctl_dma_map() when we have a larger fragment than we need. This
 * splits the fragment into 4 and returns the upper-most. (The caller can loop
 * until it has a suitable fragment size.) */
static struct mem_fragment *split_frag(struct mem_fragment *frag)
{
	struct mem_fragment *x[3];
	x[0] = kmalloc(sizeof(struct mem_fragment), GFP_KERNEL);
	x[1] = kmalloc(sizeof(struct mem_fragment), GFP_KERNEL);
	x[2] = kmalloc(sizeof(struct mem_fragment), GFP_KERNEL);
	if (!x[0] || !x[1] || !x[2]) {
		kfree(x[0]);
		kfree(x[1]);
		kfree(x[2]);
		return NULL;
	}
	BUG_ON(frag->refs);
	frag->len >>= 2;
	frag->pfn_len >>= 2;
	x[0]->base = frag->base + frag->len;
	x[1]->base = x[0]->base + frag->len;
	x[2]->base = x[1]->base + frag->len;
	x[0]->len = x[1]->len = x[2]->len = frag->len;
	x[0]->pfn_base = frag->pfn_base + frag->pfn_len;
	x[1]->pfn_base = x[0]->pfn_base + frag->pfn_len;
	x[2]->pfn_base = x[1]->pfn_base + frag->pfn_len;
	x[0]->pfn_len = x[1]->pfn_len = x[2]->pfn_len = frag->pfn_len;
	x[0]->refs = x[1]->refs = x[2]->refs = 0;
	list_add(&x[0]->list, &frag->list);
	list_add(&x[1]->list, &x[0]->list);
	list_add(&x[2]->list, &x[1]->list);
	return x[2];
}

/* Conversely, when a fragment is released we look to see whether its
 * similarly-split siblings are free to be reassembled. */
static struct mem_fragment *merge_frag(struct mem_fragment *frag)
{
	/* If this fragment can be merged with its siblings, it will have
	 * newbase and newlen as its geometry. */
	uint64_t newlen = frag->len << 2;
	uint64_t newbase = frag->base & ~(newlen - 1);
	struct mem_fragment *tmp, *leftmost = frag, *rightmost = frag;
	/* Scan left until we find the start */
	tmp = list_entry(frag->list.prev, struct mem_fragment, list);
	while ((&tmp->list != &mem_list) && (tmp->base >= newbase)) {
		if (tmp->refs)
			return NULL;
		if (tmp->len != tmp->len)
			return NULL;
		leftmost = tmp;
		tmp = list_entry(tmp->list.prev, struct mem_fragment, list);
	}
	/* Scan right until we find the end */
	tmp = list_entry(frag->list.next, struct mem_fragment, list);
	while ((&tmp->list != &mem_list) && (tmp->base < (newbase + newlen))) {
		if (tmp->refs)
			return NULL;
		if (tmp->len != tmp->len)
			return NULL;
		rightmost = tmp;
		tmp = list_entry(tmp->list.next, struct mem_fragment, list);
	}
	if (leftmost == rightmost)
		return NULL;
	/* OK, we can merge */
	frag = leftmost;
	frag->len = newlen;
	frag->pfn_len = newlen >> PAGE_SHIFT;
	while (1) {
		int lastone;
		tmp = list_entry(frag->list.next, struct mem_fragment, list);
		lastone = (tmp == rightmost);
		if (&tmp->list == &mem_list)
			break;
		list_del(&tmp->list);
		kfree(tmp);
		if (lastone)
			break;
	}
	return frag;
}

/* Helper to verify that 'sz' is (4096 * 4^x) for some x. */
static int is_good_size(u64 sz)
{
	int log = ilog2(phys_size);
	if ((phys_size & (phys_size - 1)) || (log < 12) || (log & 1))
		return 0;
	return 1;
}

/* Hook from arch/powerpc/mm/mem.c */
int usdpaa_test_fault(unsigned long pfn, u64 *phys_addr, u64 *size)
{
	struct mem_fragment *frag;
	int idx = -1;
	if ((pfn < pfn_start) || (pfn >= (pfn_start + pfn_size)))
		return -1;
	/* It's in-range, we need to find the fragment */
	spin_lock(&mem_lock);
	list_for_each_entry(frag, &mem_list, list) {
		if ((pfn >= frag->pfn_base) && (pfn < (frag->pfn_base +
						       frag->pfn_len))) {
			*phys_addr = frag->base;
			*size = frag->len;
			idx = current_tlb++;
			if (current_tlb >= (first_tlb + num_tlb))
				current_tlb = first_tlb;
			break;
		}
	}
	spin_unlock(&mem_lock);
	return idx;
}

static int usdpaa_open(struct inode *inode, struct file *filp)
{
	const struct alloc_backend *backend = &alloc_backends[0];
	struct ctx *ctx = kmalloc(sizeof(struct ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	filp->private_data = ctx;

	while (backend->id_type != usdpaa_id_max) {
		dpa_alloc_init(&ctx->ids[backend->id_type]);
		backend++;
	}

	INIT_LIST_HEAD(&ctx->maps);

	filp->f_mapping->backing_dev_info = &directly_mappable_cdev_bdi;

	return 0;
}

static int usdpaa_release(struct inode *inode, struct file *filp)
{
	struct ctx *ctx = filp->private_data;
	struct mem_mapping *map, *tmp;
	const struct alloc_backend *backend = &alloc_backends[0];
	while (backend->id_type != usdpaa_id_max) {
		int ret, leaks = 0;
		do {
			u32 id, num;
			ret = dpa_alloc_pop(&ctx->ids[backend->id_type],
					    &id, &num);
			if (!ret) {
				leaks += num;
				backend->release(id, num);
			}
		} while (ret == 1);
		if (leaks)
			pr_crit("USDPAA process leaking %d %s%s\n", leaks,
				backend->acronym, (leaks > 1) ? "s" : "");
		backend++;
	}
	spin_lock(&mem_lock);
	list_for_each_entry_safe(map, tmp, &ctx->maps, list) {
		if (map->frag->has_locking && (map->frag->owner == map)) {
			map->frag->owner = NULL;
			wake_up(&map->frag->wq);
		}
		if (!--map->frag->refs) {
			struct mem_fragment *frag = map->frag;
			do {
				frag = merge_frag(frag);
			} while (frag);
		}
		list_del(&map->list);
		kfree(map);
	}
	spin_unlock(&mem_lock);
	kfree(ctx);
	return 0;
}

static int usdpaa_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct ctx *ctx = filp->private_data;
	struct mem_mapping *map;
	int ret = 0;

	spin_lock(&mem_lock);
	list_for_each_entry(map, &ctx->maps, list) {
		if (map->frag->pfn_base == vma->vm_pgoff)
			goto map_match;
	}
	spin_unlock(&mem_lock);
	return -ENOMEM;

map_match:
	if (map->frag->len != (vma->vm_end - vma->vm_start))
		ret = -EINVAL;
	spin_unlock(&mem_lock);
	if (!ret)
		ret = remap_pfn_range(vma, vma->vm_start, map->frag->pfn_base,
				      vma->vm_end - vma->vm_start,
				      vma->vm_page_prot);
	return ret;
}

/* Return the nearest rounded-up address >= 'addr' that is 'sz'-aligned. 'sz'
 * must be a power of 2, but both 'addr' and 'sz' can be expressions. */
#define USDPAA_MEM_ROUNDUP(addr, sz) \
	({ \
		unsigned long foo_align = (sz) - 1; \
		((addr) + foo_align) & ~foo_align; \
	})
/* Searching for a size-aligned virtual address range starting from 'addr' */
static unsigned long usdpaa_get_unmapped_area(struct file *file,
					      unsigned long addr,
					      unsigned long len,
					      unsigned long pgoff,
					      unsigned long flags)
{
	struct vm_area_struct *vma;

	if (!is_good_size(len))
		return -EINVAL;

	addr = USDPAA_MEM_ROUNDUP(addr, len);
	vma = find_vma(current->mm, addr);
	/* Keep searching until we reach the end of currently-used virtual
	 * address-space or we find a big enough gap. */
	while (vma) {
		if ((addr + len) < vma->vm_start)
			return addr;
		addr = USDPAA_MEM_ROUNDUP(vma->vm_end, len);
		vma = vma->vm_next;
	}
	if ((TASK_SIZE - len) < addr)
		return -ENOMEM;
	return addr;
}

static long ioctl_id_alloc(struct ctx *ctx, void __user *arg)
{
	struct usdpaa_ioctl_id_alloc i;
	const struct alloc_backend *backend;
	int ret = copy_from_user(&i, arg, sizeof(i));
	if (ret)
		return ret;
	if ((i.id_type >= usdpaa_id_max) || !i.num)
		return -EINVAL;
	backend = &alloc_backends[i.id_type];
	/* Allocate the required resource type */
	ret = backend->alloc(&i.base, i.num, i.align, i.partial);
	if (ret < 0)
		return ret;
	i.num = ret;
	/* Copy the result to user-space */
	ret = copy_to_user(arg, &i, sizeof(i));
	if (ret) {
		backend->release(i.base, i.num);
		return ret;
	}
	/* Assign the allocated range to the FD accounting */
	dpa_alloc_free(&ctx->ids[i.id_type], i.base, i.num);
	return 0;
}

static long ioctl_id_release(struct ctx *ctx, void __user *arg)
{
	struct usdpaa_ioctl_id_release i;
	const struct alloc_backend *backend;
	int ret = copy_from_user(&i, arg, sizeof(i));
	if (ret)
		return ret;
	if ((i.id_type >= usdpaa_id_max) || !i.num)
		return -EINVAL;
	backend = &alloc_backends[i.id_type];
	/* Pull the range out of the FD accounting - the range is valid iff this
	 * succeeds. */
	ret = dpa_alloc_reserve(&ctx->ids[i.id_type], i.base, i.num);
	if (ret)
		return ret;
	/* Release the resource to the backend */
	backend->release(i.base, i.num);
	return 0;
}

static long ioctl_dma_map(struct ctx *ctx, void __user *arg)
{
	struct usdpaa_ioctl_dma_map i;
	struct mem_fragment *frag;
	struct mem_mapping *map, *tmp;
	u64 search_size;
	int ret = copy_from_user(&i, arg, sizeof(i));
	if (ret)
		return ret;
	if (i.len && !is_good_size(i.len))
		return -EINVAL;
	map = kmalloc(sizeof(*map), GFP_KERNEL);
	if (!map)
		return -ENOMEM;
	spin_lock(&mem_lock);
	if (i.flags & USDPAA_DMA_FLAG_SHARE) {
		list_for_each_entry(frag, &mem_list, list) {
			if (frag->refs && (frag->flags &
					   USDPAA_DMA_FLAG_SHARE) &&
					!strncmp(i.name, frag->name,
						 USDPAA_DMA_NAME_MAX)) {
				/* Matching entry */
				if ((i.flags & USDPAA_DMA_FLAG_CREATE) &&
				    !(i.flags & USDPAA_DMA_FLAG_LAZY)) {
					ret = -EBUSY;
					goto out;
				}
				list_for_each_entry(tmp, &ctx->maps, list)
					if (tmp->frag == frag) {
						ret = -EBUSY;
						goto out;
					}
				i.has_locking = frag->has_locking;
				i.did_create = 0;
				i.len = frag->len;
				goto do_map;
			}
		}
		/* No matching entry */
		if (!(i.flags & USDPAA_DMA_FLAG_CREATE)) {
			ret = -ENOMEM;
			goto out;
		}
	}
	/* New fragment required, size must be provided. */
	if (!i.len) {
		ret = -EINVAL;
		goto out;
	}
	/* We search for the required size and if that fails, for the next
	 * biggest size, etc. */
	for (search_size = i.len; search_size <= phys_size; search_size <<= 2) {
		list_for_each_entry(frag, &mem_list, list) {
			if (!frag->refs && (frag->len == search_size)) {
				while (frag->len > i.len) {
					frag = split_frag(frag);
					if (!frag) {
						ret = -ENOMEM;
						goto out;
					}
				}
				frag->flags = i.flags;
				strncpy(frag->name, i.name,
					USDPAA_DMA_NAME_MAX);
				frag->has_locking = i.has_locking;
				init_waitqueue_head(&frag->wq);
				frag->owner = NULL;
				i.did_create = 1;
				goto do_map;
			}
		}
	}
	ret = -ENOMEM;
	goto out;

do_map:
	map->frag = frag;
	frag->refs++;
	list_add(&map->list, &ctx->maps);
	i.pa_offset = frag->base;

out:
	spin_unlock(&mem_lock);
	if (!ret)
		ret = copy_to_user(arg, &i, sizeof(i));
	else
		kfree(map);
	return ret;
}

static int test_lock(struct mem_mapping *map)
{
	int ret = 0;
	spin_lock(&mem_lock);
	if (!map->frag->owner) {
		map->frag->owner = map;
		ret = 1;
	}
	spin_unlock(&mem_lock);
	return ret;
}

static long ioctl_dma_lock(struct ctx *ctx, void __user *arg)
{
	struct mem_mapping *map;
	struct vm_area_struct *vma;

	down_read(&current->mm->mmap_sem);
	vma = find_vma(current->mm, (unsigned long)arg);
	if (!vma || (vma->vm_start > (unsigned long)arg)) {
		up_read(&current->mm->mmap_sem);
		return -EFAULT;
	}
	spin_lock(&mem_lock);
	list_for_each_entry(map, &ctx->maps, list) {
		if (map->frag->pfn_base == vma->vm_pgoff)
			goto map_match;
	}
	map = NULL;
map_match:
	spin_unlock(&mem_lock);
	up_read(&current->mm->mmap_sem);

	if (!map->frag->has_locking)
		return -ENODEV;
	return wait_event_interruptible(map->frag->wq, test_lock(map));
}

static long ioctl_dma_unlock(struct ctx *ctx, void __user *arg)
{
	struct mem_mapping *map;
	struct vm_area_struct *vma;
	int ret;

	down_read(&current->mm->mmap_sem);
	vma = find_vma(current->mm, (unsigned long)arg);
	if (!vma || (vma->vm_start > (unsigned long)arg))
		ret = -EFAULT;
	else {
		spin_lock(&mem_lock);
		list_for_each_entry(map, &ctx->maps, list) {
			if (map->frag->pfn_base == vma->vm_pgoff) {
				if (!map->frag->has_locking)
					ret = -ENODEV;
				else if (map->frag->owner == map) {
					map->frag->owner = NULL;
					wake_up(&map->frag->wq);
					ret = 0;
				} else
					ret = -EBUSY;
				goto map_match;
			}
		}
		ret = -EINVAL;
map_match:
		spin_unlock(&mem_lock);
	}
	up_read(&current->mm->mmap_sem);
	return ret;
}

static long usdpaa_ioctl(struct file *fp, unsigned int cmd, unsigned long arg)
{
	struct ctx *ctx = fp->private_data;
	void __user *a = (void __user *)arg;
	switch (cmd) {
	case USDPAA_IOCTL_ID_ALLOC:
		return ioctl_id_alloc(ctx, a);
	case USDPAA_IOCTL_ID_RELEASE:
		return ioctl_id_release(ctx, a);
	case USDPAA_IOCTL_DMA_MAP:
		return ioctl_dma_map(ctx, a);
	case USDPAA_IOCTL_DMA_LOCK:
		return ioctl_dma_lock(ctx, a);
	case USDPAA_IOCTL_DMA_UNLOCK:
		return ioctl_dma_unlock(ctx, a);
	}
	return -EINVAL;
}

static const struct file_operations usdpaa_fops = {
	.open		   = usdpaa_open,
	.release	   = usdpaa_release,
	.mmap		   = usdpaa_mmap,
	.get_unmapped_area = usdpaa_get_unmapped_area,
	.unlocked_ioctl	   = usdpaa_ioctl,
	.compat_ioctl	   = usdpaa_ioctl
};

static struct miscdevice usdpaa_miscdev = {
	.name = "fsl-usdpaa",
	.fops = &usdpaa_fops,
	.minor = MISC_DYNAMIC_MINOR,
};

/* Early-boot memory allocation. The boot-arg "usdpaa_mem=<x>" is used to
 * indicate how much memory (if any) to allocate during early boot. If the
 * format "usdpaa_mem=<x>,<y>" is used, then <y> will be interpreted as the
 * number of TLB1 entries to reserve (default is 1). If there are more mappings
 * than there are TLB1 entries, fault-handling will occur. */
static __init int usdpaa_mem(char *arg)
{
	phys_size = memparse(arg, &arg);
	num_tlb = 1;
	if (*arg == ',') {
		unsigned long ul;
		int err = kstrtoul(arg + 1, 0, &ul);
		if (err < 0) {
			num_tlb = 1;
			pr_warning("ERROR, usdpaa_mem arg is invalid\n");
		} else
			num_tlb = (unsigned int)ul;
	}
	return 0;
}
early_param("usdpaa_mem", usdpaa_mem);

__init void fsl_usdpaa_init_early(void)
{
	if (!phys_size) {
		pr_info("No USDPAA memory, no 'usdpaa_mem' bootarg\n");
		return;
	}
	if (!is_good_size(phys_size)) {
		pr_err("'usdpaa_mem' bootarg must be 4096*4^x\n");
		phys_size = 0;
		return;
	}
	phys_start = memblock_alloc(phys_size, phys_size);
	if (!phys_start) {
		pr_err("Failed to reserve USDPAA region (sz:%llx)\n",
		       phys_size);
		return;
	}
	pfn_start = phys_start >> PAGE_SHIFT;
	pfn_size = phys_size >> PAGE_SHIFT;
	first_tlb = current_tlb = tlbcam_index;
	tlbcam_index += num_tlb;
	pr_info("USDPAA region at %llx:%llx(%lx:%lx), %d TLB1 entries)\n",
		phys_start, phys_size, pfn_start, pfn_size, num_tlb);
}

static int __init usdpaa_init(void)
{
	struct mem_fragment *frag;
	int ret;

	pr_info("Freescale USDPAA process driver\n");
	if (!phys_start) {
		pr_warning("fsl-usdpaa: no region found\n");
		return 0;
	}
	frag = kmalloc(sizeof(*frag), GFP_KERNEL);
	if (!frag) {
		pr_err("Failed to setup USDPAA memory accounting\n");
		return -ENOMEM;
	}
	frag->base = phys_start;
	frag->len = phys_size;
	frag->pfn_base = pfn_start;
	frag->pfn_len = pfn_size;
	frag->refs = 0;
	init_waitqueue_head(&frag->wq);
	frag->owner = NULL;
	list_add(&frag->list, &mem_list);
	ret = misc_register(&usdpaa_miscdev);
	if (ret)
		pr_err("fsl-usdpaa: failed to register misc device\n");
	return ret;
}

static void __exit usdpaa_exit(void)
{
	misc_deregister(&usdpaa_miscdev);
}

module_init(usdpaa_init);
module_exit(usdpaa_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Freescale Semiconductor");
MODULE_DESCRIPTION("Freescale USDPAA process driver");
