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

struct params {
	struct io *c_iop;
	struct list_head *rmhd;
};

void __run_im(struct io *q_iop, struct io *im_iop, void *param)
{
	struct params *p = param;
	run_queue(q_iop, p->c_iop, p->rmhd);
	dump_iop(im_iop, 0);
	list_add_tail(&im_iop->f_head, p->rmhd);
}

void __run_unim(struct io *q_iop, struct io *im_iop, void *param)
{
	struct params *p = param;
	if (q_iop->bytes_left == 0) {
		q_iop->linked = dip_rb_ins(q_iop->dip, q_iop);
		ASSERT(q_iop->linked);
#if defined(DEBUG)
		rb_tree_size++;
#endif
	}

	q_iop->bytes_left += im_iop->t.bytes;
	unupdate_q2i(q_iop, tdelta(q_iop, im_iop));
	list_add_tail(&im_iop->f_head, p->rmhd);
}

void run_im(struct io *im_iop, struct io *c_iop, void *param)
{
	struct params p = {
		.c_iop = c_iop,
		.rmhd = (struct list_head *)param
	};
	bilink_for_each_down(__run_im, im_iop, &p, 1);
}

void run_unim(struct io *im_iop, struct list_head *rmhd)
{
	struct params p = {
		.c_iop = NULL,
		.rmhd = rmhd
	};
	bilink_for_each_down(__run_unim, im_iop, &p, 1);
}

int ready_im(struct io *im_iop, struct io *c_iop)
{
	if (im_iop->bytes_left > 0) {
		__u64 xfer;
		LIST_HEAD(head);
		struct io *q_iop;
		struct list_head *p, *q;

		dip_foreach_list(im_iop, IOP_Q, &head);
		list_for_each_safe(p, q, &head) {
			q_iop = list_entry(p, struct io, f_head);
			LIST_DEL(&q_iop->f_head);

			if (ready_queue(q_iop, c_iop)) {
				update_q2i(q_iop, tdelta(q_iop, im_iop));

				bilink(q_iop, im_iop);
				dip_rem(q_iop);

				xfer = min(im_iop->bytes_left, 
							    q_iop->bytes_left);
				im_iop->bytes_left -= xfer;
				q_iop->bytes_left -= xfer;

				if (q_iop->bytes_left == 0)
					dip_rem(q_iop);
			}
		}
	}

	return im_iop->bytes_left == 0;
}

void trace_insert(struct io *i_iop)
{
	if (io_setup(i_iop, IOP_I))
		iostat_insert(i_iop);
	else
		io_release(i_iop);

}

void trace_merge(struct io *m_iop)
{
	if (io_setup(m_iop, IOP_M))
		iostat_merge(m_iop);
	else
		io_release(m_iop);
}
