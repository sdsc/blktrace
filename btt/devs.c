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
struct list_head	dev_heads[N_DEV_HASH];

void init_dev_heads(void)
{
	int i;
	for (i = 0; i < N_DEV_HASH; i++)
		INIT_LIST_HEAD(&dev_heads[i]);
}

struct d_info *__dip_find(__u32 device)
{
	struct list_head *p;
	struct d_info *dip;

	__list_for_each(p, &dev_heads[DEV_HASH(device)]) {
		dip = list_entry(p, struct d_info, hash_head);
		if (device == dip->device)
			return dip;
	}

	return NULL;
}

struct d_info *dip_add(__u32 device, struct io *iop, int link)
{
	struct d_info *dip = __dip_find(device);

	if (dip == NULL) {
		dip = malloc(sizeof(struct d_info));
		dip->heads = dip_rb_mkhds();
		init_region(&dip->regions);
		dip->device = device;
		dip->last_q = (__u64)-1;
		dip->map = dev_map_find(device);
		dip->seek_handle = seeki_init(device);
		latency_init(dip);
		memset(&dip->stats, 0, sizeof(dip->stats));
		memset(&dip->all_stats, 0, sizeof(dip->all_stats));
		list_add_tail(&dip->hash_head, &dev_heads[DEV_HASH(device)]);
		list_add_tail(&dip->all_head, &all_devs);
		n_devs++;
	}

	if (link)
		dip_rb_ins(dip, iop);

	return dip;
}

void dip_rem(struct io *iop)
{
	dip_rb_rem(iop);
}

void dip_foreach(struct io *iop, enum iop_type type, 
		 void (*fnc)(struct io *iop, struct io *this), int rm_after)
{
	if (rm_after) {
		LIST_HEAD(head);
		struct io *this;
		struct list_head *p, *q;

		dip_rb_fe(iop->dip, type, iop, fnc, &head);
		list_for_each_safe(p, q, &head) {
			this = list_entry(p, struct io, f_head);
			LIST_DEL(&this->f_head);
			io_release(this);
		}
	}
	else
		dip_rb_fe(iop->dip, type, iop, fnc, NULL);
}

struct io *dip_find_sec(struct d_info *dip, enum iop_type type, __u64 sec)
{
	return dip_rb_find_sec(dip, type, sec);
}

void dip_foreach_out(void (*func)(struct d_info *, void *), void *arg)
{
	if (devices == NULL) {
		struct list_head *p;
		__list_for_each(p, &all_devs)
			func(list_entry(p, struct d_info, all_head), arg);
	}
	else {
		int i;
		struct d_info *dip;
		unsigned int mjr, mnr;
		char *p = devices;

		while (p && ((i = sscanf(p, "%u,%u", &mjr, &mnr)) == 2)) {
			dip = __dip_find((__u32)((mjr << MINORBITS) | mnr));
			ASSERT(dip);

			func(dip, arg);

			p = strchr(p, ';');
			if (p) p++;
		}
	}
}
