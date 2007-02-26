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

#if defined(DEBUG)
void __dump_rb_node(struct rb_node *n)
{
	struct io *iop = rb_entry(n, struct io, rb_node);

	dbg_ping();
	if (iop->type == IOP_A)
		__dump_iop2(stdout, iop, bilink_first_down(iop, NULL));
	else
		__dump_iop(stdout, iop, 0);
	if (n->rb_left)
		__dump_rb_node(n->rb_left);
	if (n->rb_right)
		__dump_rb_node(n->rb_right);
}

void __dump_rb_tree(struct d_info *dip, enum iop_type type)
{
	struct rb_root *roots = dip->heads;
	struct rb_root *root = &roots[type];
	struct rb_node *n = root->rb_node;

	if (n) {
		printf("\tIOP_%c\n", type2c(type));
		__dump_rb_node(n);
	}
}

void dump_rb_trees(void)
{
	int i;
	enum iop_type type;
	struct d_info *dip;
	struct list_head *p;

	for (i = 0; i < N_DEV_HASH; i++) {
		__list_for_each(p, &dev_heads[i]) {
			dip = list_entry(p, struct d_info, hash_head);
			printf("Trees for %3d,%-3d\n", MAJOR(dip->device),
			       MINOR(dip->device));
			for (type = IOP_Q; type < N_IOP_TYPES; type++) {
				if (type != IOP_L)
					__dump_rb_tree(dip, type);
			}
		}
	}
}
#endif

void init_dev_heads(void)
{
	int i;
	for (i = 0; i < N_DEV_HASH; i++)
		INIT_LIST_HEAD(&dev_heads[i]);
}

struct d_info *__dip_find(__u32 device)
{
	struct d_info *dip;
	struct list_head *p;

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
		dip = malloc(sizeof(struct d_info));
		memset(dip, 0, sizeof(*dip));
		dip->heads = dip_rb_mkhds();
		init_region(&dip->regions);
		dip->device = device;
		dip->last_q = (__u64)-1;
		dip->map = dev_map_find(device);
		dip->seek_handle = seeki_init(device);
		latency_init(dip);
		list_add_tail(&dip->hash_head, &dev_heads[DEV_HASH(device)]);
		list_add_tail(&dip->all_head, &all_devs);
		dip->start_time = BIT_TIME(iop->t.time);
		dip->pre_culling = 1;
		n_devs++;
	}

	if (dip->pre_culling) {
		if (iop->type == IOP_Q || iop->type == IOP_A)
			dip->pre_culling = 0;
		else
			return NULL;
	}

	iop->linked = dip_rb_ins(dip, iop);
#if defined(DEBUG)
	if (iop->linked) 
		rb_tree_size++;
#endif

	dip->end_time = BIT_TIME(iop->t.time);
	return dip;
}

void dip_rem(struct io *iop)
{
	if (iop->linked) {
		dip_rb_rem(iop);
		iop->linked = 0;
	}
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

void dip_foreach_list(struct io *iop, enum iop_type type, struct list_head *hd)
{
	dip_rb_fe(iop->dip, type, iop, NULL, hd);
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

void dip_plug(__u32 dev, double cur_time)
{
	struct d_info *dip = __dip_find(dev);

	if (!dip || dip->is_plugged) return;

	dip->is_plugged = 1;
	dip->last_plug = cur_time;
}

void dip_unplug(__u32 dev, double cur_time, int is_timer)
{
	struct d_info *dip = __dip_find(dev);

	if (!dip || !dip->is_plugged) return;

	dip->nplugs++;
	if (is_timer) dip->n_timer_unplugs++;

	dip->plugged_time += (cur_time - dip->last_plug);
	dip->is_plugged = 0;
}
