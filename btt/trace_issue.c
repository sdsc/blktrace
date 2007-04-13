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

static void __run_issue(struct io *im_iop, struct io *d_iop, struct io *c_iop)
{
	update_i2d(im_iop, tdelta(im_iop, d_iop));
	run_im(im_iop, d_iop, c_iop);
	dump_iop(d_iop, 0);
}

static void __run_unissue(struct io *im_iop, struct io *d_iop, 
			  struct io *c_iop)
{
	unupdate_i2d(im_iop, tdelta(im_iop, d_iop));
	run_unim(im_iop, d_iop, c_iop);
}

void run_issue(struct io *d_iop, __attribute__((__unused__))struct io *u_iop, 
							struct io *c_iop)
{
	bilink_for_each_down(__run_issue, d_iop, c_iop, 1);
	add_rmhd(d_iop);
}

void run_unissue(struct io *d_iop, 
		 __attribute__((__unused__))struct io *u_iop, 
		 struct io *c_iop)
{
	bilink_for_each_down(__run_unissue, d_iop, c_iop, 1);
	add_rmhd(d_iop);
}

int ready_issue(struct io *d_iop, struct io *c_iop)
{
	if (d_iop->bytes_left > 0) {
		LIST_HEAD(head);
		struct io *im_iop;
		struct list_head *p, *q;

		dip_foreach_list(d_iop, IOP_I, &head);
		dip_foreach_list(d_iop, IOP_M, &head);
		list_for_each_safe(p, q, &head) {
			im_iop = list_entry(p, struct io, f_head);
			LIST_DEL(&im_iop->f_head);

			ASSERT(d_iop->bytes_left >= im_iop->t.bytes);
			if (ready_im(im_iop, c_iop)) {
				bilink(im_iop, d_iop);
				dip_rem(im_iop);
				d_iop->bytes_left -= im_iop->t.bytes;
			}
		}
	}

	return d_iop->bytes_left == 0;
}

void trace_issue(struct io *d_iop)
{
	if (io_setup(d_iop, IOP_D)) {
		seeki_add(d_iop->dip->seek_handle, d_iop);
		iostat_issue(d_iop);
		d_iop->dip->n_ds++;
		if (!remapper_dev(d_iop->t.device))
			update_d_histo(d_iop->t.bytes);
	}
	else
		io_release(d_iop);

}
