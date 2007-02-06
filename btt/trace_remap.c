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

void run_remap(struct io *a_iop, struct io *c_iop, void *param)
{
	struct bilink *blp = blp, *blp2;
	struct list_head *rmhd = param;
	struct io *q_iop, *l_iop = bilink_first_down(a_iop, &blp);

	ASSERT(l_iop);
	q_iop = bilink_first_down(l_iop, &blp2);
	if (q_iop) {
		run_queue(q_iop, c_iop, rmhd);
		biunlink(blp2);
	}

	dump_iop2(a_iop, l_iop);

	biunlink(blp);
	list_add_tail(&l_iop->f_head, rmhd);
	list_add_tail(&a_iop->f_head, rmhd);
}

int ready_dev_remap(struct io *l_iop, struct io *c_iop)
{
	struct io *q_iop;

	if (!list_empty(&l_iop->down_list)) 
		return 1;

	q_iop = dip_find_sec(l_iop->dip, IOP_Q, l_iop->t.sector);
	if (!q_iop || !ready_queue(q_iop, c_iop))
		return 0;

	ASSERT(l_iop->t.bytes <= q_iop->t.bytes);
	update_q2a(q_iop, tdelta(q_iop, l_iop));
	bilink(q_iop, l_iop);

	q_iop->bytes_left -= l_iop->t.bytes;
	if (q_iop->bytes_left == 0)
		dip_rem(q_iop);
	return 1;
}

int ready_self_remap(struct io *l_iop)
{
	struct io *a_iop = dip_find_sec(l_iop->dip, IOP_A, l_iop->t.sector);

	if (a_iop) {
		update_q2a(a_iop, tdelta(a_iop, l_iop));
		dip_rem(a_iop);
	}

	return 1;
}

int ready_remap(struct io *a_iop, struct io *c_iop)
{
	struct io *l_iop = bilink_first_down(a_iop, NULL);

	ASSERT(l_iop);
	if (remapper_dev(l_iop->t.device))
		return ready_dev_remap(l_iop, c_iop);
	else
		return ready_self_remap(l_iop);
}

void trace_remap(struct io *a_iop)
{
	struct io *l_iop;
	struct blk_io_trace_remap *rp = a_iop->pdu;

	a_iop->t.device = be32_to_cpu(rp->device_from);
	if (!io_setup(a_iop, IOP_A)) {
		io_release(a_iop);
		return;
	}

	l_iop = io_alloc();
	memcpy(&l_iop->t, &a_iop->t, sizeof(a_iop->t));
	if (l_iop->t.pdu_len) {
		l_iop->pdu = malloc(l_iop->t.pdu_len);
		memcpy(l_iop->pdu, a_iop->pdu, l_iop->t.pdu_len);
	}

	l_iop->t.device = be32_to_cpu(rp->device);
	l_iop->t.sector = be64_to_cpu(rp->sector);
	if (!io_setup(l_iop, IOP_L)) {
		io_release(l_iop);
		io_release(a_iop);
		return;
	}

	bilink(l_iop, a_iop);
}
