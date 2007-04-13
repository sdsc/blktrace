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

static inline void __update_q2c(struct io *q_iop, struct io *c_iop)
{
	__u64 q2c = tdelta(q_iop, c_iop);

	update_q2c(q_iop, q2c);
	latency_q2c(q_iop->dip, q_iop->t.time, q2c);
}

void run_queue(struct io *q_iop, __attribute__((__unused__))struct io *u_iop,
	       struct io *c_iop)
{
	struct bilink *blp;
	struct io *a_iop = bilink_first_down(q_iop, &blp);

	if (a_iop) {
		run_remap(a_iop, q_iop, c_iop);
		biunlink(blp);
	}

	__update_q2c(q_iop, c_iop);
	dump_iop(q_iop, 0);
	add_rmhd(q_iop);
}

int ready_queue(struct io *q_iop, struct io *c_iop)
{
	struct io *a_iop;

	if (!list_empty(&q_iop->down_list))
		return 1;

	a_iop = dip_find_sec(q_iop->dip, IOP_A, BIT_START(q_iop));
	if (!a_iop)
		return 1;

	if (!ready_remap(a_iop, c_iop))
		return 0;

	ASSERT(q_iop->t.bytes == a_iop->t.bytes);
	bilink(a_iop, q_iop);
	dip_rem(a_iop);
	return 1;
}

void trace_queue(struct io *q_iop)
{
	if (io_setup(q_iop, IOP_Q)) {
		update_lq(&last_q, &all_avgs.q2q, q_iop->t.time);
		update_qregion(&all_regions, q_iop->t.time);
		dip_update_q(q_iop->dip, q_iop);
		pip_update_q(q_iop);
		if (!remapper_dev(q_iop->t.device))
			update_q_histo(q_iop->t.bytes);
	}
	else
		io_release(q_iop);
}
