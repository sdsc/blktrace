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

void im2d_func(struct io *d_iop, struct io *im_iop)
{
	update_i2d(im_iop, d_iop->t.time - im_iop->t.time);
}

void q2c_func(struct io *c_iop, struct io *q_iop)
{
	__u64 q2c = c_iop->t.time - q_iop->t.time;

	update_q2c(q_iop, q2c);
	latency_q2c(q_iop->dip, q_iop->t.time, q2c);
}

static inline void handle_im(struct io *im_iop)
{
	struct io *q_iop;

	q_iop = dip_find_sec(im_iop->dip, IOP_Q, BIT_START(im_iop));
	if (q_iop)
		update_q2i(q_iop, im_iop->t.time - q_iop->t.time);
}

void handle_queue(struct io *q_iop)
{
	io_setup(q_iop, IOP_Q, 1);
	update_lq(&last_q, &all_avgs.q2q, q_iop->t.time);
	update_qregion(&all_regions, q_iop->t.time);
	dip_update_q(q_iop->dip, q_iop);
	pip_update_q(q_iop);
}

void handle_remap(struct io *a_iop)
{
	struct io *q_iop;
	struct blk_io_trace_remap *rp = a_iop->pdu;
	struct d_info *dip = __dip_find(be32_to_cpu(rp->device));

	io_setup(a_iop, IOP_A, 0);
	q_iop = dip_find_sec(dip, IOP_Q, be64_to_cpu(rp->sector));
	if (q_iop)
		update_q2a(q_iop, a_iop->t.time - q_iop->t.time);
	io_release(a_iop);
}

void handle_insert(struct io *i_iop)
{
	io_setup(i_iop, IOP_I, 1);
	iostat_insert(i_iop);
	handle_im(i_iop);
}

void handle_merge(struct io *m_iop)
{
	io_setup(m_iop, IOP_M, 1);
	iostat_merge(m_iop);
	handle_im(m_iop);
}

void handle_issue(struct io *d_iop)
{
	io_setup(d_iop, IOP_D, 1);
	d_iop->dip->n_ds++;

	dip_foreach(d_iop, IOP_I, im2d_func, 0);
	dip_foreach(d_iop, IOP_M, im2d_func, 0);

	if (seek_name)
		seeki_add(d_iop->dip->seek_handle, d_iop);
	iostat_issue(d_iop);
}

void handle_complete(struct io *c_iop)
{
	struct io *d_iop;

	io_setup(c_iop, IOP_C, 0);
	update_blks(c_iop);
	update_cregion(&all_regions, c_iop->t.time);
	update_cregion(&c_iop->dip->regions, c_iop->t.time);
	if (c_iop->pip)
		update_cregion(&c_iop->pip->regions, c_iop->t.time);

	d_iop = dip_find_sec(c_iop->dip, IOP_D, BIT_START(c_iop));
	if (d_iop) {
		__u64 d2c = c_iop->t.time - d_iop->t.time;
		update_d2c(d_iop, d2c);
		latency_d2c(d_iop->dip, c_iop->t.time, d2c);
		iostat_complete(d_iop, c_iop);
		dip_foreach(d_iop, IOP_I, NULL, 1);
		dip_foreach(d_iop, IOP_M, NULL, 1);
		io_release(d_iop);
	}

	dip_foreach(c_iop, IOP_Q, q2c_func, 1);
	io_release(c_iop);
}

void rq_im2d_func(struct io *d_iop, struct io *im_iop)
{
	unupdate_i2d(im_iop, d_iop->t.time - im_iop->t.time);
}

/*
 * Careful surgery
 * (1) Need to remove D & its I & M's
 * (2) Need to leave I's Q and M's Q's
 * (3) XXX: Need to downward adjust stats, but we don't carry PREVIOUS
 *     XXX: min/maxes?! We'll just adjust what we can, and hope that 
 *     XXX: the min/maxes are "pretty close". (REQUEUEs are rare, right?)
 */
void handle_requeue(struct io *r_iop)
{
	struct io *d_iop;

	io_setup(r_iop, IOP_R, 0);
	d_iop = dip_find_sec(r_iop->dip, IOP_D, BIT_START(r_iop));
	if (d_iop) {
		dip_foreach(d_iop, IOP_I, rq_im2d_func, 1);
		dip_foreach(d_iop, IOP_M, rq_im2d_func, 1);
		iostat_unissue(d_iop);
		io_release(d_iop);
	}
	io_release(r_iop);
}

void __add_trace(struct io *iop)
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
	case __BLK_TA_QUEUE:		handle_queue(iop); break;
	case __BLK_TA_BACKMERGE:	handle_merge(iop); break;
	case __BLK_TA_FRONTMERGE:	handle_merge(iop); break;
	case __BLK_TA_ISSUE:		handle_issue(iop); break;
	case __BLK_TA_COMPLETE:		handle_complete(iop); break;
	case __BLK_TA_INSERT:		handle_insert(iop); break;
	case __BLK_TA_REMAP:		handle_remap(iop); break;
	case __BLK_TA_REQUEUE:		handle_requeue(iop); break;
	default:			io_release(iop); break;
	}
}

void add_trace(struct io *iop)
{
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
