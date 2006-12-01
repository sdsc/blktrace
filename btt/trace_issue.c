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

void trace_issue(struct io *d_iop)
{
	if (!io_setup(d_iop, IOP_D)) {
		io_release(d_iop);
		return;
	}

	if (seek_name)
		seeki_add(d_iop->dip->seek_handle, d_iop);
	iostat_issue(d_iop);
	d_iop->dip->n_ds++;
}

int ready_issue(struct io *d_iop, struct io *top)
{
	LIST_HEAD(head);
	struct io *im_iop;
	struct list_head *p, *q;
	__u64 bl = d_iop->bytes_left;

	dip_foreach_list(d_iop, IOP_I, &head);
	dip_foreach_list(d_iop, IOP_M, &head);
	list_for_each_safe(p, q, &head) {
		im_iop = list_entry(p, struct io, f_head);
		LIST_DEL(&im_iop->f_head);

		if (!ready_im(im_iop, top))
			return 0;

		bl -= im_iop->bytes_left;
	}

	return bl == 0;
}

void run_issue(struct io *d_iop, struct io *top, struct list_head *del_head)
{
	LIST_HEAD(head);
	struct list_head *p, *q;
	struct io *im_iop;

	dip_foreach_list(d_iop, IOP_I, &head);
	dip_foreach_list(d_iop, IOP_M, &head);
	list_for_each_safe(p, q, &head) {
		im_iop = list_entry(p, struct io, f_head);
		LIST_DEL(&im_iop->f_head);

		update_i2d(im_iop, tdelta(im_iop, d_iop));

		__link(im_iop, d_iop);
		run_im(im_iop, top, del_head);
		__unlink(im_iop, d_iop);
	}

	dump_iop(per_io_ofp, d_iop, NULL, 0);
	list_add_tail(&d_iop->f_head, del_head);
}

void run_unissue(struct io *d_iop, struct list_head *del_head)
{
	LIST_HEAD(head);
	struct io *im_iop;
	struct list_head *p, *q;

	iostat_unissue(d_iop);

	dip_foreach_list(d_iop, IOP_I, &head);
	dip_foreach_list(d_iop, IOP_M, &head);
	list_for_each_safe(p, q, &head) {
		im_iop = list_entry(p, struct io, f_head);
		LIST_DEL(&im_iop->f_head);

		__link(im_iop, d_iop);
		unupdate_i2d(im_iop, tdelta(im_iop, d_iop));
		run_unim(im_iop, del_head);
		__unlink(im_iop, d_iop);
	}

	list_add_tail(&d_iop->f_head, del_head);
}
