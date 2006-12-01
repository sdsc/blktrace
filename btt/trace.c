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

int dump_level;
LIST_HEAD(retries);

static inline void dump_dev(FILE *ofp, __u32 dev)
{
	fprintf(ofp, "%3d,%-3d ", MAJOR(dev), MINOR(dev));
}

static inline void dump_desc(FILE *ofp, struct io *iop)
{
	fprintf(ofp, "%10llu+%-4u ", (unsigned long long)iop->t.sector, 
		t_sec(&iop->t));
}

void dump_iop(FILE *ofp, struct io *to_iop, struct io *from_iop, int indent)
{
	int i, c;

	if (!ofp) return;
	if (to_iop->displayed) return;

	fprintf(ofp, "%5d.%09lu ", (int)SECONDS(to_iop->t.time),
		(unsigned long)NANO_SECONDS(to_iop->t.time));

	for (i = 0; i < ((dump_level * 4) + indent); i++)
		fprintf(ofp, " ");

	dump_dev(ofp, to_iop->t.device);

	switch (to_iop->type) {
	case IOP_Q: c = 'Q'; break;
	case IOP_L: c = 'L'; break;
	case IOP_A: c = 'A'; break;
	case IOP_I: c = 'I'; break;
	case IOP_M: c = 'M'; break;
	case IOP_D: c = 'D'; break;
	case IOP_C: c = 'C'; break;
	default   : c = '?'; break;
	}

	fprintf(ofp, "%c ", c);
	dump_desc(ofp, to_iop);
	if (from_iop) {
		fprintf(ofp, "<- ");
		dump_dev(ofp, from_iop->t.device);
		dump_desc(ofp, from_iop);
	}
		
	fprintf(ofp, "\n");

	to_iop->displayed = 1;
}

void release_iops(struct list_head *del_head)
{
	struct io *x_iop;
	struct list_head *p, *q;

	list_for_each_safe(p, q, del_head) {
		x_iop = list_entry(p, struct io, f_head);
		LIST_DEL(&x_iop->f_head);
		io_release(x_iop);
	}
}

static void do_retries(void)
{
	struct io *iop;
	struct list_head *p, *q;

	list_for_each_safe(p, q, &retries) {
		iop = list_entry(p, struct io, retry);
		// iop could be gone after call...
		if (iop->type == IOP_C) 
			retry_complete(iop);
		else
			retry_requeue(iop);
	}
}

static void __add_trace(struct io *iop)
{
	time_t now = time(NULL);

	n_traces++;
	iostat_check_time(iop->t.time);

	if (verbose && ((now - last_vtrace) > 0)) {
		printf("%10lu t\r", n_traces);
		if ((n_traces % 1000000) == 0) printf("\n");
		fflush(stdout);
		last_vtrace = now;
	}

	switch (iop->t.action & 0xffff) {
	case __BLK_TA_QUEUE:		trace_queue(iop); break;
	case __BLK_TA_REMAP:		trace_remap(iop); break;
	case __BLK_TA_INSERT:		trace_insert(iop); break;
	case __BLK_TA_BACKMERGE:	trace_merge(iop); break;
	case __BLK_TA_FRONTMERGE:	trace_merge(iop); break;
	case __BLK_TA_REQUEUE:		trace_requeue(iop); break;
	case __BLK_TA_ISSUE:		trace_issue(iop); break;
	case __BLK_TA_COMPLETE:		trace_complete(iop); break;
	default:			
		io_release(iop); 
		return;
	}

	if (((iop->t.action & 0xffff) != __BLK_TA_REQUEUE) && 
						!list_empty(&retries))
		do_retries();
}

void add_trace(struct io *iop)
{
	if (iop->t.time == 15717167961) dbg_ping();
	if (iop->t.action & BLK_TC_ACT(BLK_TC_NOTIFY)) {
		char *slash = strchr(iop->pdu, '/');

		if (slash)
			*slash = '\0';

		add_process(iop->t.pid, iop->pdu);
		io_release(iop);
	}
	else if (iop->t.action & BLK_TC_ACT(BLK_TC_PC))
		io_release(iop);
	else
		__add_trace(iop);
}
