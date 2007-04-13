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
#include "globals.h"

LIST_HEAD(pending_cs);

static inline void __run_complete(struct io *c_iop)
{
	if (remapper_dev(c_iop->t.device)) {
		struct bilink *blp = blp;
		struct io *iop = bilink_first_down(c_iop, &blp);

		if (iop->type == IOP_Q) {
			run_queue(iop, c_iop, c_iop);
			biunlink(blp);
		}
		else
			bilink_for_each_down(run_remap, c_iop, c_iop, 1);
	}
	else
		bilink_for_each_down(run_issue, c_iop, c_iop, 1);

	dump_iop(c_iop, 1);

	LIST_DEL(&c_iop->c_pending);
	del_retry(c_iop);
	add_rmhd(c_iop);

	release_iops();
}

static int ready_complete_remapper(struct io *c_iop)
{
	LIST_HEAD(head);
	struct list_head *p, *q;

	dip_foreach_list(c_iop, IOP_L, &head);
	if (list_empty(&head)) {
		struct io *q_iop;

		dip_foreach_list(c_iop, IOP_Q, &head);
		list_for_each_safe(p, q, &head) {
			q_iop = list_entry(p, struct io, f_head);
			LIST_DEL(&q_iop->f_head);

			ASSERT(list_empty(&q_iop->up_list));
			ASSERT(list_empty(&q_iop->down_list));
			ASSERT(q_iop->t.bytes == c_iop->t.bytes);
			if (ready_queue(q_iop, c_iop)) {
				dip_rem(q_iop);
				bilink(q_iop, c_iop);
				c_iop->bytes_left -= q_iop->t.bytes;
			}
		}
	}
	else {
		struct io *l_iop, *a_iop;

		list_for_each_safe(p, q, &head) {
			l_iop = list_entry(p, struct io, f_head);
			LIST_DEL(&l_iop->f_head);

			ASSERT(!list_empty(&l_iop->up_list));
			a_iop = bilink_first_up(l_iop, NULL);
			if (ready_remap(a_iop, c_iop)) {
				dip_rem(l_iop);
				bilink(a_iop, c_iop);
				c_iop->bytes_left -= a_iop->t.bytes;
			}
		}
	}

	return c_iop->bytes_left == 0;
}

int ready_complete(struct io *c_iop)
{
	__u64 d2c;
	struct io *d_iop;

	if (c_iop->bytes_left == 0)
		return 1;

	if (remapper_dev(c_iop->t.device))
		return ready_complete_remapper(c_iop);

	if (!list_empty(&c_iop->down_list))
		return 1;

	d_iop = dip_find_sec(c_iop->dip, IOP_D, BIT_START(c_iop));
	if (!d_iop)
		return -1;

	if (c_iop->t.bytes != d_iop->t.bytes) {
		fprintf(stderr, 
			"\nFATAL: Probable time anomaly detected\n");
		fprintf(stderr, 
			"D @ %15.9lf missing C, later C @ %15.9lf\n", 
			BIT_TIME(d_iop->t.time), 
			BIT_TIME(c_iop->t.time));
		exit(1);
	}

	if (!ready_issue(d_iop, c_iop))
		return 0;

	c_iop->bytes_left = 0;

	d2c = tdelta(d_iop, c_iop);
	update_d2c(d_iop, d_iop->down_len, d2c);
	latency_d2c(d_iop->dip, c_iop->t.time, d2c);
	iostat_complete(d_iop, c_iop);

	bilink(d_iop, c_iop);
	dip_rem(d_iop);
	return 1;
}

void trace_complete(struct io *c_iop)
{
	if (io_setup(c_iop, IOP_C)) {
		update_blks(c_iop);
		update_cregion(&all_regions, c_iop->t.time);
		update_cregion(&c_iop->dip->regions, c_iop->t.time);
		if (c_iop->pip)
			update_cregion(&c_iop->pip->regions, c_iop->t.time);

		list_add_tail(&c_iop->c_pending, &pending_cs);
		switch (ready_complete(c_iop)) {
		case  1: 
			__run_complete(c_iop); 
			break;
		case  0: 
			add_retry(c_iop); 
			break;
		case -1: 
			LIST_DEL(&c_iop->c_pending);
			del_retry(c_iop);
			io_release(c_iop);
			break;
		}
	}
	else 
		io_release(c_iop);
}

void retry_complete(struct io *c_iop, __u64 now)
{
	double tc = BIT_TIME(c_iop->t.time);

	switch (ready_complete(c_iop)) {
	case  1: 
#		if defined(DEBUG)
			fprintf(stderr, "Retried %15.9lf success!\n", tc);
#		endif

		__run_complete(c_iop); 
		break;
	case  0:
		if (now == 0 || ((BIT_TIME(now) - tc) < 1.0))
			break;
		if (!list_empty(&c_iop->down_list))
			break;
		/*FALLTHROUGH*/
	case -1: 
		LIST_DEL(&c_iop->c_pending);
		del_retry(c_iop);
		io_release(c_iop);
		break;
	}
}
