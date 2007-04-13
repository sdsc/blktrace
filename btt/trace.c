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

void __dump_iop(FILE *ofp, struct io *iop, int extra_nl)
{
	fprintf(ofp, "%5d.%09lu %3d,%-3d %c %10llu+%-4u\n",
		(int)SECONDS(iop->t.time),
		(unsigned long)NANO_SECONDS(iop->t.time),
		MAJOR(iop->t.device), MINOR(iop->t.device), type2c(iop->type),
		(unsigned long long)iop->t.sector, t_sec(&iop->t));
	if (extra_nl) fprintf(ofp, "\n");
}

void __dump_iop2(FILE *ofp, struct io *a_iop, struct io *l_iop)
{
	fprintf(ofp, "%5d.%09lu %3d,%-3d %c %10llu+%-4u <- (%3d,%-3d) %10llu\n",
		(int)SECONDS(a_iop->t.time),
		(unsigned long)NANO_SECONDS(a_iop->t.time),
		MAJOR(a_iop->t.device), MINOR(a_iop->t.device), 
		type2c(a_iop->type), (unsigned long long)a_iop->t.sector, 
		t_sec(&a_iop->t), MAJOR(l_iop->t.device), 
		MINOR(l_iop->t.device), (unsigned long long)l_iop->t.sector);
}

void release_iops(void)
{
	struct io *x_iop;
	struct list_head *p, *q;

	list_for_each_safe(p, q, &rmhd) {
		x_iop = list_entry(p, struct io, rm_head);
		LIST_DEL(&x_iop->rm_head);
		io_release(x_iop);
	}
}

void do_retries(__u64 now)
{
	struct io *iop;
	struct list_head *p, *q;

	list_for_each_safe(p, q, &retries) {
		iop = list_entry(p, struct io, retry);
		ASSERT(iop->type == IOP_C);

		// iop could be gone after call...
		retry_complete(iop, now);
	}
}

static inline int retry_check_time(__u64 t)
{
	return next_retry_check && (t > next_retry_check);
}

static void __add_trace(struct io *iop)
{
	time_t now = time(NULL);
	__u64 tstamp = iop->t.time;
	int run_retry = retry_check_time(iop->t.time);

	n_traces++;
	iostat_check_time(iop->t.time);

	if (verbose && ((now - last_vtrace) > 0)) {

#	if defined(DEBUG)
		printf("%10lu t\tretries=|%10d|\ttree size=|%10d|\r", 
			n_traces, list_len(&retries), rb_tree_size);
#	else
		printf("%10lu t\r", n_traces);
#	endif

		if ((n_traces % 1000000) == 0) printf("\n");
		fflush(stdout);
		last_vtrace = now;
	}

	switch (iop->t.action & 0xffff) {
	case __BLK_TA_QUEUE:		trace_queue(iop); break;
	case __BLK_TA_REMAP:		trace_remap(iop); break;
	case __BLK_TA_GETRQ:		trace_insert(iop); break;
	case __BLK_TA_BACKMERGE:	trace_merge(iop); break;
	case __BLK_TA_FRONTMERGE:	trace_merge(iop); break;
	case __BLK_TA_REQUEUE:		trace_requeue(iop); break;
	case __BLK_TA_ISSUE:		trace_issue(iop); break;
	case __BLK_TA_COMPLETE:		trace_complete(iop); break;
	case __BLK_TA_PLUG:		trace_plug(iop); break;
	case __BLK_TA_UNPLUG_IO:	trace_unplug_io(iop); break;
	case __BLK_TA_UNPLUG_TIMER:	trace_unplug_timer(iop); break;
	default:			
		io_release(iop); 
		return;
	}

	if (run_retry && !list_empty(&retries)) {
		do_retries(tstamp);
		bump_retry(tstamp);
	}
}

void add_trace(struct io *iop)
{
	if (iop->t.action & BLK_TC_ACT(BLK_TC_NOTIFY)) {
		if (iop->t.pid == 0) 
			add_process(0, "kernel");
		else {
			char *slash = strchr(iop->pdu, '/');
			if (slash)
				*slash = '\0';

			add_process(iop->t.pid, iop->pdu);
		}
		io_release(iop);
	}
	else if (iop->t.action & BLK_TC_ACT(BLK_TC_PC))
		io_release(iop);
	else {
		if (time_bounded) {
			if (BIT_TIME(iop->t.time) < t_astart) {
				io_release(iop);
				return;
			}
			else if (BIT_TIME(iop->t.time) > t_aend) {
				io_release(iop);
				done = 1;
				return;
			}
		}
		__add_trace(iop);
	}
}
