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

static void gen_c_list(struct io *c_iop, struct list_head *c_head)
{
	struct io *iop;
	struct list_head *p;

	__list_for_each(p, &pending_cs) {
		iop = list_entry(p, struct io, c_pending);
		if (iop->t.device == c_iop->t.device)
			continue;
		if (dip_find_sec(iop->dip, IOP_D, BIT_START(iop)) == NULL)
			continue;

		__link(iop, c_iop);
		if (ready_complete(iop, c_iop))
			list_add_tail(&iop->f_head, c_head);
		__unlink(iop, c_iop);
	}
}

static void run_comp(struct io *c_iop, struct io *top, struct list_head *rmhd)
{
	struct io *d_iop = dip_find_sec(c_iop->dip, IOP_D, BIT_START(c_iop));

	update_blks(c_iop);
	if (d_iop) {
		__u64 d2c = tdelta(d_iop, c_iop);

		update_d2c(d_iop, d2c);
		latency_d2c(d_iop->dip, c_iop->t.time, d2c);
		iostat_complete(d_iop, c_iop);

		__link(d_iop, c_iop);
		run_issue(d_iop, top, rmhd);
		__unlink(d_iop, c_iop);
	}
	else {
		LIST_HEAD(head);
		struct io *iop;
		struct list_head *p, *q;

		gen_c_list(c_iop, &head);
		list_for_each_safe(p, q, &head) {
			iop = list_entry(p, struct io, f_head);
			LIST_DEL(&iop->f_head);

			dump_level++;
			__link(iop, c_iop);
			run_comp(iop, top, rmhd);
			__unlink(iop, c_iop);
			dump_level--;
		}
	}

	dump_iop(per_io_ofp, c_iop, NULL, 0);

	LIST_DEL(&c_iop->c_pending);
	del_retry(c_iop);
	list_add_tail(&c_iop->f_head, rmhd);
}

static int ready_comp(struct io *c_iop, 
				__attribute__((__unused__)) struct io *top)
{
	LIST_HEAD(head);
	struct io *iop;
	struct list_head *p, *q;
	__u64 bl = c_iop->bytes_left;

	gen_c_list(c_iop, &head);
	list_for_each_safe(p, q, &head) {
		iop = list_entry(p, struct io, f_head);
		LIST_DEL(&iop->f_head);

		__link(iop, c_iop);
		if (ready_complete(iop, c_iop))
			bl -= iop->bytes_left;
		__unlink(iop, c_iop);
	}

	return bl == 0;
}

void trace_complete(struct io *c_iop)
{
	if (!io_setup(c_iop, IOP_C)) {
		io_release(c_iop);
		return;
	}

	list_add_tail(&c_iop->c_pending, &pending_cs);
	if (ready_complete(c_iop, c_iop)) {
		dump_level = 0;
		run_complete(c_iop);
	}
	else 
		add_retry(c_iop);
}

int retry_complete(struct io *c_iop)
{
	if (!ready_complete(c_iop, c_iop))
		return 0;

	run_complete(c_iop);
	return 1;
}

int ready_complete(struct io *c_iop, struct io *top)
{
	struct io *d_iop = dip_find_sec(c_iop->dip, IOP_D, BIT_START(c_iop));

	if (d_iop) {
		ASSERT(d_iop->t.bytes == c_iop->bytes_left);
		return ready_issue(d_iop, top);
	}
	else 
		return ready_comp(c_iop, top);
}

void run_complete(struct io *c_iop)
{
	LIST_HEAD(rmhd);

	update_cregion(&all_regions, c_iop->t.time);
	update_cregion(&c_iop->dip->regions, c_iop->t.time);
	if (c_iop->pip)
		update_cregion(&c_iop->pip->regions, c_iop->t.time);

	run_comp(c_iop, c_iop, &rmhd);
	if (per_io_ofp) fprintf(per_io_ofp, "\n");
	release_iops(&rmhd);
}
