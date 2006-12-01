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

void trace_queue(struct io *q_iop)
{
	if (!io_setup(q_iop, IOP_Q)) {
		io_release(q_iop);
		return;
	}

	update_lq(&last_q, &all_avgs.q2q, q_iop->t.time);
	update_qregion(&all_regions, q_iop->t.time);
	dip_update_q(q_iop->dip, q_iop);
	pip_update_q(q_iop);
}

int ready_queue(struct io *q_iop, struct io *top)
{
	struct io *a_iop = dip_find_sec(q_iop->dip, IOP_A, BIT_START(q_iop));

	if (a_iop) {
		ASSERT(a_iop->bytes_left == q_iop->bytes_left);
		return ready_remap(a_iop, top);
	}

	return q_iop->t.device == top->t.device &&
	       BIT_START(top) <= BIT_START(q_iop) &&
	                         BIT_END(q_iop) <= BIT_END(top);
}

void run_queue(struct io *q_iop, struct io *top, struct list_head *del_head)
{
	struct io *iop;
	struct io *a_iop = dip_find_sec(q_iop->dip, IOP_A, BIT_START(q_iop));

	if (a_iop) {
		__link(a_iop, q_iop);
		run_remap(a_iop, top, del_head);
		__unlink(a_iop, q_iop);
	}

	for (iop = q_iop; iop != NULL; iop = list_first_up(iop)) {
		if (iop->type == IOP_C && iop->t.device == q_iop->t.device) {
			__u64 q2c = tdelta(q_iop, iop);

			update_q2c(q_iop, q2c);
			latency_q2c(q_iop->dip, q_iop->t.time, q2c);

			dump_iop(per_io_ofp, q_iop, NULL, 
			         (q_iop->t.device == top->t.device) ? -4 : 0);

			break;
		}
	}

	iop = list_first_up(q_iop);
	q_iop->bytes_left -= iop->bytes_left;
	if (q_iop->bytes_left == 0)
		list_add_tail(&q_iop->f_head, del_head);
}

void run_unqueue(struct io *q_iop, struct list_head *del_head)
{
	struct io *a_iop = dip_find_sec(q_iop->dip, IOP_A, BIT_START(q_iop));

	if (a_iop) {
		__link(a_iop, q_iop);
		run_unremap(a_iop, del_head);
		__unlink(a_iop, q_iop);
	}

	list_add_tail(&q_iop->f_head, del_head);
}
