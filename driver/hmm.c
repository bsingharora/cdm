/*
 * Copyright (C) 2017 IBM Corporation
 * Author: Reza Arbab <arbab@linux.vnet.ibm.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>
 */

#include <linux/hmm.h>
#include "cdm.h"

extern const struct migrate_dma_ops cdm_migrate_ops;

/*
 * The world's dumbest page allocator
 */
struct page *cdm_devmem_alloc(struct cdm_device *cdmdev)
{
	unsigned long pfn = cdmdev->free_pfn;
	struct page *page;
	void *addr;

	if (pfn > PFN_DOWN(cdmdev->res.end))
		return NULL;

	page = pfn_to_page(pfn);
	get_page(page);

	addr = cdmdev->mem + (PFN_PHYS(pfn) - cdmdev->res.start);
	hmm_devmem_page_set_drvdata(page, (unsigned long)addr);

	pr_info("%s: pfn 0x%lx, memremap=0x%p\n", __func__, pfn, addr);

	cdmdev->free_pfn++;
	return page;
}

static void cdm_devmem_free(struct hmm_devmem *devmem, struct page *page)
{
	pr_info("%s: 0x%lx\n", __func__, page_to_pfn(page));

	hmm_devmem_page_set_drvdata(page, 0);
}

/*
 * CPU fault, migrate to system memory
 */
static int cdm_fault(struct hmm_devmem *devmem, struct vm_area_struct *vma,
		     unsigned long addr, struct page *page,
		     unsigned int flags, pmd_t *pmdp)
{
	struct migrate_dma_ctx ctx = {};
	unsigned long src, dst;

	pr_info("%s: 0x%lx\n", __func__, addr);

	ctx.ops = &cdm_migrate_ops;
	ctx.src = &src;
	ctx.dst = &dst;

	return hmm_devmem_fault_range(devmem, &ctx, vma, addr, addr,
				      addr + PAGE_SIZE);
}

#ifdef WIP
static void cdm_devmem_fault_collect(struct migrate_dma_ctx *ctx, unsigned long start,
				     unsigned long end)
{
	for (; start < end; start += PAGE_SIZE) {
		unsigned long pfn = PHYS_PFN(start);

		get_page(pfn_to_page(pfn));

		ctx->cpages++;
		ctx->src[ctx->npages] = migrate_pfn(pfn);
		ctx->src[ctx->npages++] |= MIGRATE_PFN_MIGRATE;
	}
}

/*
 * GPU fault, migrate to device memory
 *
 * start and end are physical addresses
 */
static int cdm_devmem_fault(struct cdm_device *cdmdev, unsigned long start,
			    unsigned long end)
{
	struct migrate_dma_ctx ctx = {};
	unsigned long addr, next;

	for (addr = start; addr < end; addr = next) {
		unsigned long src[64], dst[64];
		int rc;

		next = min(end, addr + (64 << PAGE_SHIFT));

		ctx.ops = &cdm_migrate_ops;
		ctx.src = src;
		ctx.dst = dst;
		ctx.private = cdmdev;
		cdm_devmem_fault_collect(&ctx, addr, next);

		rc = migrate_dma(&ctx);
		if (rc)
			return rc;
	}
}
#endif

static const struct hmm_devmem_ops cdm_devmem_ops = {
	.free = cdm_devmem_free,
	.fault = cdm_fault
};

int cdm_devmem_init(struct cdm_device *cdmdev)
{
	struct resource *res = &cdmdev->res;

	cdmdev->devmem = hmm_devmem_add_resource(&cdm_devmem_ops,
						 cdmdev_dev(cdmdev), res);
	if (IS_ERR(cdmdev->devmem))
		return PTR_ERR(cdmdev->devmem);

	cdmdev->mem = devm_memremap(cdmdev_dev(cdmdev), res->start,
				    resource_size(res), MEMREMAP_WB);
	if (!cdmdev->mem)
		goto err;

	cdmdev->free_pfn = PFN_UP(res->start);
	return 0;

err:
	hmm_devmem_remove(cdmdev->devmem);
	return -ENOMEM;
}

void cdm_devmem_remove(struct cdm_device *cdmdev)
{
	devm_memunmap(cdmdev_dev(cdmdev), cdmdev->mem);
	hmm_devmem_remove(cdmdev->devmem);
}
