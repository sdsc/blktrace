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

void trace_remap(struct io *a_iop)
{
	struct io *l_iop;
	struct blk_io_trace_remap *rp = a_iop->pdu;
	__u32 remap_dev = be32_to_cpu(rp->device);
	__u64 remap_sec = be64_to_cpu(rp->sector);

	if (!io_setup(a_iop, IOP_A)) {
		io_release(a_iop);
		return;
	}

	/* 
	 * Create a fake LINK trace
	 */
	l_iop = io_alloc();
	memcpy(&l_iop->t, &a_iop->t, sizeof(a_iop->t));
	l_iop->t.device = remap_dev;
	l_iop->t.sector = remap_sec;

	if (!io_setup(l_iop, IOP_L)) {
		io_release(l_iop);
		io_release(a_iop);
		return;
	}

	__link(l_iop, a_iop);
	l_iop->self_remap = (MAJOR(a_iop->t.device) == MAJOR(remap_dev));
}


int ready_remap(struct io *a_iop, struct io *top)
{
	struct io *l_iop = list_first_down(a_iop);
	struct blk_io_trace_remap *rp = a_iop->pdu;
	__u64 remap_sec = be64_to_cpu(rp->sector);

	if (l_iop->self_remap) {
		struct io *a_iop = dip_find_sec(l_iop->dip, IOP_A, remap_sec);
		if (a_iop)
			return ready_remap(a_iop, top);
	}
	else {
		struct io *q_iop = dip_find_sec(l_iop->dip, IOP_Q, remap_sec);
		if (q_iop)
			return ready_queue(q_iop, top);
	}

	return 0;
}

void run_remap(struct io *a_iop, struct io *top, struct list_head *del_head)
{
	struct io *l_iop = list_first_down(a_iop);
	struct blk_io_trace_remap *rp = a_iop->pdu;
	__u64 remap_sec = be64_to_cpu(rp->sector);

	if (l_iop->self_remap) {
		struct io *a2_iop = dip_find_sec(l_iop->dip, IOP_A, remap_sec);
		ASSERT(a2_iop);
		__link(a2_iop, l_iop);
		run_remap(a2_iop, top, del_head);
		__unlink(a2_iop, l_iop);
	}
	else {
		struct io *q_iop = dip_find_sec(l_iop->dip, IOP_Q, remap_sec);
		ASSERT(q_iop);
		__link(q_iop, l_iop);
		update_q2a(q_iop, tdelta(q_iop, a_iop));
		run_queue(q_iop, top, del_head);
		__unlink(q_iop, l_iop);
	}

	dump_iop(per_io_ofp, a_iop, l_iop, 0);

	__unlink(l_iop, a_iop);
	list_add_tail(&l_iop->f_head, del_head);
	list_add_tail(&a_iop->f_head, del_head);
}

void run_unremap(struct io *a_iop, struct list_head *del_head)
{
	struct io *l_iop = list_first_down(a_iop);
	struct blk_io_trace_remap *rp = a_iop->pdu;
	__u64 remap_sec = be64_to_cpu(rp->sector);

	if (l_iop->self_remap) {
		struct io *a_iop = dip_find_sec(l_iop->dip, IOP_A, remap_sec);
		__link(a_iop, l_iop);
		run_unremap(a_iop, del_head);
		__unlink(a_iop, l_iop);
	}
	else {
		struct io *q_iop = dip_find_sec(l_iop->dip, IOP_Q, remap_sec);
		__link(q_iop, l_iop);
		run_unqueue(q_iop, del_head);
		__unlink(q_iop, l_iop);
	}

	__unlink(l_iop, a_iop);
	list_add_tail(&l_iop->f_head, del_head);
	list_add_tail(&a_iop->f_head, del_head);
}
