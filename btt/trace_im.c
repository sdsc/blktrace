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

void trace_insert(struct io *i_iop)
{
	if (!io_setup(i_iop, IOP_I)) {
		io_release(i_iop);
		return;
	}
	iostat_insert(i_iop);
}

void trace_merge(struct io *m_iop)
{
	if (!io_setup(m_iop, IOP_M)) {
		io_release(m_iop);
		return;
	}
	iostat_merge(m_iop);
}

int ready_im(struct io *im_iop, struct io *top)
{
	struct io *q_iop = dip_find_sec(im_iop->dip, IOP_Q, BIT_START(im_iop));

	if (q_iop) {
		ASSERT(q_iop->bytes_left >= im_iop->bytes_left);
		return ready_queue(q_iop, top);
	}

	return 0;
}

void run_im(struct io *im_iop, struct io *top, struct list_head *del_head)
{
	struct io *q_iop = dip_find_sec(im_iop->dip, IOP_Q, BIT_START(im_iop));

	ASSERT(q_iop);
	update_q2i(q_iop, tdelta(q_iop, im_iop));

	__link(q_iop, im_iop);
	run_queue(q_iop, top, del_head);
	__unlink(q_iop, im_iop);

	dump_iop(per_io_ofp, im_iop, NULL, 0);
	list_add_tail(&im_iop->f_head, del_head);
}

void run_unim(struct io *im_iop, struct list_head *del_head)
{
	struct io *q_iop = dip_find_sec(im_iop->dip, IOP_Q, BIT_START(im_iop));

	__link(q_iop, im_iop);
	run_unqueue(q_iop, del_head);
	__unlink(q_iop, im_iop);

	list_add_tail(&im_iop->f_head, del_head);
}
