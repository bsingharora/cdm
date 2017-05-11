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

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/migrate.h>
#include "cdm.h"
#include "uapi.h"

extern nodemask_t cdm_nmask;

static struct page *alloc_page_cdm(struct cdm_device *cdmdev,
				   struct page *old)
{
	nodemask_t nmask = NODE_MASK_NONE;
	gfp_t gfp = GFP_HIGHUSER_MOVABLE;
	struct zonelist *zl;

	if (cdmdev)
		node_set(cdmdev_to_node(cdmdev), nmask);
	else
		nodes_andnot(nmask, node_states[N_MEMORY], cdm_nmask);

	zl = node_zonelist(page_to_nid(old), gfp);
	return __alloc_pages_nodemask(gfp, 0, zl, &nmask);
}

static void alloc_and_copy(struct migrate_dma_ctx *ctx)
{
	struct cdm_device *cdmdev = (struct cdm_device *)ctx->private;
	unsigned long i;

	for (i = 0; i < ctx->npages; i++) {
		struct page *spage, *dpage;

		if (!(ctx->src[i] & MIGRATE_PFN_MIGRATE))
			continue;

		spage = migrate_pfn_to_page(ctx->src[i]);
		dpage = alloc_page_cdm(cdmdev, spage);

		if (!dpage) {
			pr_err("%s: failed to alloc dst[%ld]\n", __func__, i);
			ctx->dst[i] = 0;
			continue;
		}

		/* use dma here */
		copy_highpage(dpage, spage);

		lock_page(dpage);
		ctx->dst[i] = migrate_pfn(page_to_pfn(dpage));
		ctx->dst[i] |= MIGRATE_PFN_LOCKED;
	}
}

static void finalize_and_map(struct migrate_dma_ctx *ctx)
{
	unsigned long i;

	for (i = 0; i < ctx->npages; i++) {
		if (ctx->src[i] & MIGRATE_PFN_MIGRATE)
			continue;

		pr_err("%s: src[%ld] not migrated\n", __func__, i);
	}
}

static const struct migrate_dma_ops cdm_migrate_ops = {
	.alloc_and_copy = alloc_and_copy,
	.finalize_and_map = finalize_and_map
};

int cdm_migrate(struct cdm_device *cdmdev, struct cdm_migrate *mig)
{
	struct vm_area_struct *vma;
	unsigned long addr, next;

	vma = find_vma_intersection(current->mm, mig->start, mig->end);

	for (addr = mig->start; addr < mig->end; addr = next) {
		struct migrate_dma_ctx ctx = {};
		unsigned long src[64], dst[64];
		int rc;

		next = min_t(unsigned long, mig->end,
			     addr + (64 << PAGE_SHIFT));

		ctx.ops = &cdm_migrate_ops;
		ctx.src = src;
		ctx.dst = dst;
		ctx.private = cdmdev;

		rc = migrate_vma(&ctx, vma, addr, next);
		if (rc)
			return rc;
	}

	return 0;
}
