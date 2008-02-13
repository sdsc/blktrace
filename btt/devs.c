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

static inline void *dip_rb_mkhds(void)
{
	size_t len = N_IOP_TYPES * sizeof(struct rb_root);
	return memset(malloc(len), 0, len);
}

static void __destroy(struct rb_node *n)
{
	if (n) {
		struct io *iop = rb_entry(n, struct io, rb_node);

		__destroy(n->rb_left);
		__destroy(n->rb_right);
		io_release(iop);
	}
}

static void __destroy_heads(struct rb_root *roots)
{
	int i;

	for (i = 0; i < N_IOP_TYPES; i++)
		__destroy(roots[i].rb_node);

	free(roots);
}

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

void dip_exit(void)
{
	struct d_info *dip;
	struct list_head *p, *q;

	list_for_each_safe(p, q, &all_devs) {
		dip = list_entry(p, struct d_info, all_head);

		__destroy_heads(dip->heads);
		region_exit(&dip->regions);
		seeki_exit(dip->seek_handle);
		seeki_exit(dip->q2q_handle);
		bno_dump_exit(dip->bno_dump_handle);
		unplug_hist_exit(dip->unplug_hist_handle);
		if (output_all_data)
			q2d_release(dip->q2d_priv);
		free(dip);
	}
}

static inline char *mkhandle(char *str, __u32 device, char *post)
{
	int mjr = device >> MINORBITS;
	int mnr = device & ((1 << MINORBITS) - 1);

	sprintf(str, "%03d,%03d%s", mjr, mnr, post);
	return str;
}

struct d_info *dip_add(__u32 device, struct io *iop)
{
	struct d_info *dip = __dip_find(device);

	if (dip == NULL) {
		char str[256];

		dip = malloc(sizeof(struct d_info));
		memset(dip, 0, sizeof(*dip));
		dip->heads = dip_rb_mkhds();
		region_init(&dip->regions);
		dip->device = device;
		dip->last_q = (__u64)-1;
		dip->map = dev_map_find(device);
		dip->bno_dump_handle = bno_dump_init(device);
		dip->unplug_hist_handle = unplug_hist_init(device);
		dip->seek_handle = seeki_init(mkhandle(str, device, "_d2d"));
		dip->q2q_handle = seeki_init(mkhandle(str, device, "_q2q"));
		latency_init(dip);
		list_add_tail(&dip->hash_head, &dev_heads[DEV_HASH(device)]);
		list_add_tail(&dip->all_head, &all_devs);
		dip->start_time = BIT_TIME(iop->t.time);
		dip->pre_culling = 1;
		if (output_all_data)
			dip->q2d_priv = q2d_init();
		n_devs++;
	}

	if (dip->pre_culling) {
		if (iop->type == IOP_Q || iop->type == IOP_A)
			dip->pre_culling = 0;
		else
			return NULL;
	}

	iop->linked = dip_rb_ins(dip, iop);
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
			list_del(&this->f_head);
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

void dip_unplug(__u32 dev, double cur_time, __u64 nios_up)
{
	struct d_info *dip = __dip_find(dev);

	if (dip && dip->is_plugged) {
		dip->nplugs++;
		dip->plugged_time += (cur_time - dip->last_plug);
		dip->is_plugged = 0;
		dip->nios_up += nios_up;
	}
}

void dip_unplug_tm(__u32 dev, __u64 nios_up)
{
	struct d_info *dip = __dip_find(dev);

	if (dip && dip->is_plugged) {
		dip->n_timer_unplugs++;
		dip->nios_upt += nios_up;
		dip->nplugs_t++;
	}
}
