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

void trace_requeue(struct io *r_iop)
{
	if (!io_setup(r_iop, IOP_R)) {
		io_release(r_iop);
		return;
	}

	if (ready_requeue(r_iop, r_iop))
		run_requeue(r_iop);
	else 
		add_retry(r_iop);
}

int retry_requeue(struct io *r_iop)
{
	if (!ready_requeue(r_iop, r_iop))
		return 0;

	run_requeue(r_iop);
	return 1;
}

int ready_requeue(struct io *r_iop, struct io *top)
{
	struct io *d_iop = dip_find_sec(r_iop->dip, IOP_D, BIT_START(r_iop));
	if (d_iop)
		return ready_issue(d_iop, top);
	return 0;
}

void run_requeue(struct io *r_iop)
{
	LIST_HEAD(del_head);
	struct io *d_iop = dip_find_sec(r_iop->dip, IOP_D, BIT_START(r_iop));

	__link(d_iop, r_iop);
	run_unissue(d_iop, &del_head);
	__unlink(d_iop, r_iop);

	del_retry(r_iop);
	list_add_tail(&r_iop->f_head, &del_head);
	release_iops(&del_head);
}
