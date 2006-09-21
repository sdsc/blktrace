/*
 * blktrace output analysis: generate a timeline & gather statistics
 *
 * Copyright (C) 2006 Alan D. Brunelle <Alan.Brunelle@hp.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */
#include <stdio.h>

#include "globals.h"

#define N_DEV_HASH	128
#define DEV_HASH(dev)	((MAJOR(dev) ^ MINOR(dev)) & (N_DEV_HASH - 1))
struct list_head	*dev_heads = NULL;

static void init_dev_heads(void)
{
	int i;

	dev_heads = zmalloc(N_DEV_HASH * sizeof(struct list_head));
	for (i = 0; i < N_DEV_HASH; i++)
		INIT_LIST_HEAD(&dev_heads[i]);
}

struct d_info *__dip_find(__u32 device)
{
	struct list_head *p;
	struct d_info *dip;

	if (dev_heads == NULL) {
		init_dev_heads();
		return NULL;
	}

	__list_for_each(p, &dev_heads[DEV_HASH(device)]) {
		dip = list_entry(p, struct d_info, hash_head);
		if (device == dip->device)
			return dip;
	}

	return NULL;
}

struct d_info *dip_add(__u32 device, struct io *iop)
{
	struct d_info *dip = __dip_find(device);

	if (dip == NULL) {
		int i;

		dip = zmalloc(sizeof(*dip));
		dip->device = device;
		dip->last_q = (__u64)-1;
		for (i = 0; i < N_IOP_TYPES; i++)
			INIT_LIST_HEAD(&dip->iop_heads[i]);
		init_region(&dip->regions);
		dip->map = dev_map_find(device);
		dip->seek_handle = seeki_init(device);
		memset(&dip->stats, 0, sizeof(dip->stats));
		memset(&dip->all_stats, 0, sizeof(dip->all_stats));

		if (dev_heads == NULL) init_dev_heads();
		list_add_tail(&dip->hash_head, &dev_heads[DEV_HASH(device)]);

		list_add_tail(&dip->head, &all_devs);
		n_devs++;
	}

	list_add(&iop->dev_head, dip_get_head(dip, iop->type));

	return dip;
}
