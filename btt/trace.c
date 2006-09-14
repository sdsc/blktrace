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

static inline void release_iop(struct io *iop)
{
	if (iop->pdu) free(iop->pdu);
	IO_FREE(iop);
}

struct io *dip_find_exact(struct list_head *head, struct io *iop_in)
{
	struct io *iop;
	struct list_head *p;

	if (head != NULL) __list_for_each(p, head) {
		iop = list_entry(p, struct io, dev_head);
		if (is_bit(iop_in, iop))
			return iop;
	}
	return NULL;
}

struct io *dip_find_in(struct list_head *head, struct io *iop_in)
{
	struct io *iop;
	struct list_head *p;

	if (head != NULL) __list_for_each(p, head) {
		iop = list_entry(p, struct io, dev_head);
		if (in_bit(iop, iop_in))
			return iop;
	}
	return NULL;
}

struct io *dip_find_start(struct list_head *head, __u64 sector)
{
	struct io *iop;
	struct list_head *p;

	if (head != NULL) __list_for_each(p, head) {
		iop = list_entry(p, struct io, dev_head);
		if (BIT_START(iop) == sector)
			return iop;
	}
	return NULL;
}

struct io *dip_find_end(struct list_head *head, __u64 sector)
{
	struct io *iop;
	struct list_head *p;

	if (head != NULL) __list_for_each(p, head) {
		iop = list_entry(p, struct io, dev_head);
		if (BIT_END(iop) == sector)
			return iop;
	}
	return NULL;
}

struct io *dip_find_in_sec(struct list_head *head, __u64 sector)
{
	struct io *iop;
	struct list_head *p;

	if (head != NULL) __list_for_each(p, head) {
		iop = list_entry(p, struct io, dev_head);
		if (BIT_START(iop) <= sector && sector <= BIT_END(iop))
			return iop;
	}
	return NULL;
}

struct io *dip_find_qa(struct list_head *head, struct blk_io_trace *t)
{
	struct io *iop;
	struct list_head *p;

	if (head != NULL) __list_for_each(p, head) {
		iop = list_entry(p, struct io, dev_head);
		if (iop->t.cpu == t->cpu && iop->t.sequence == (t->sequence-1))
			return iop;
	}
	return NULL;
}

void dip_add_ms(struct list_head *head, struct io *d_iop)
{
	struct io *m_iop;
	struct list_head *p;
	struct io_list *iolp;

	if (head != NULL) __list_for_each(p, head) {
		m_iop = list_entry(p, struct io, dev_head);
		if (in_bit(m_iop, d_iop)) {
			iolp = malloc(sizeof(*iolp));
			io_link(&iolp->iop, m_iop);
			list_add_tail(&iolp->head, &d_iop->u.d.d_im_head);
		}
	}
}

void dip_add_qs(struct list_head *head, struct io *i_iop)
{
	struct io *q_iop;
	struct list_head *p;
	struct io_list *iolp;

	if (head != NULL) __list_for_each(p, head) {
		q_iop = list_entry(p, struct io, dev_head);
		if (in_bit(q_iop, i_iop)) {
			iolp = malloc(sizeof(*iolp));
			io_link(&iolp->iop, q_iop);
			list_add_tail(&iolp->head, &i_iop->u.i.i_qs_head);
		}
	}
}

void handle_queue(struct io *iop)
{
	struct io *tmp;

	io_setup(iop, IOP_Q);

	update_lq(&last_q, &all_avgs.q2q, iop->t.time);
	update_qregion(&all_regions, iop->t.time);
	dip_update_q(iop->dip, iop);
	pip_update_q(iop);

	tmp = dip_find_exact(dip_get_head(iop->dip, IOP_A), iop);
	if (tmp) {
		iop->u.q.qp_type = Q_A;
		io_link(&iop->u.q.qp.q_a, tmp);
	}
	else
		iop->u.q.qp_type = Q_NONE;
}

void handle_merge(struct io *iop)
{
	struct io *q_iop;

	io_setup(iop, IOP_M);

	q_iop = dip_find_exact(dip_get_head(iop->dip, IOP_Q), iop);
	if (q_iop)
		io_link(&iop->u.m.m_q, q_iop);
}

void handle_insert(struct io *iop)
{
	struct io_list *iolp = malloc(sizeof(*iolp));

	io_setup(iop, IOP_I);
	INIT_LIST_HEAD(&iop->u.i.i_qs_head);
	dip_add_qs(dip_get_head(iop->dip, IOP_Q), iop);
}

void handle_complete(struct io *iop)
{
	struct io *d_iop;

	io_setup(iop, IOP_C);
	update_blks(iop);
	update_cregion(&all_regions, iop->t.time);
	update_cregion(&iop->dip->regions, iop->t.time);
	if (iop->pip)
		update_cregion(&iop->pip->regions, iop->t.time);

	d_iop = dip_find_exact(dip_get_head(iop->dip, IOP_D), iop);
	if (d_iop) {
		io_link(&iop->u.c.c_d, d_iop);

		add_cy(iop);
	}
	else
		io_free(iop);
}

void handle_issue(struct io *iop)
{
	struct io *i_iop;
	struct io_list *iolp = malloc(sizeof(*iolp));

	io_setup(iop, IOP_D);
	iop->dip->n_ds++;

	INIT_LIST_HEAD(&iop->u.d.d_im_head);
	i_iop = dip_find_in(dip_get_head(iop->dip, IOP_I), iop);
	if (i_iop) {
		io_link(&iolp->iop, i_iop);
		list_add_tail(&iolp->head, &iop->u.d.d_im_head);
	}

	dip_add_ms(dip_get_head(iop->dip, IOP_M), iop);
	seeki_add(iop->dip->seek_handle, iop);
}

void handle_split(struct io *iop)
{
	struct io *q_iop;

	pending_xs++;
	io_setup(iop, IOP_X);

	q_iop = dip_find_exact(dip_get_head(iop->dip, IOP_Q), iop);
	if (q_iop)
		io_link(&iop->u.x.x_q, q_iop);
}

void handle_remap(struct io *iop)
{
	struct io *q_iop, *a_iop;
	struct blk_io_trace_remap *rp = iop->pdu;
	__u32 dev = be32_to_cpu(rp->device);
	__u64 sector = be64_to_cpu(rp->sector);

	io_setup(iop, IOP_A);
	q_iop = dip_find_in_sec(dip_get_head_dev(dev, IOP_Q), sector);
	if (q_iop) {
		iop->u.a.ap_type = A_Q;
		io_link(&iop->u.a.ap.a_q, q_iop);
		return;
	}

	a_iop = dip_find_in_sec(dip_get_head_dev(dev, IOP_A), sector);
	if (a_iop) {
		iop->u.a.ap_type = A_A;
		io_link(&iop->u.a.ap.a_a, a_iop);
		return;
	}

	iop->u.a.ap_type = A_NONE;
}

void extract_i(struct io *i_iop)
{
	struct io_list *iolp;
	struct list_head *p, *q;

	ASSERT(i_iop != NULL && i_iop->type == IOP_I);
	list_for_each_safe(p, q, &i_iop->u.i.i_qs_head) {
		iolp = list_entry(p, struct io_list, head);
		LIST_DEL(&iolp->head);

		ASSERT(iolp->iop->type == IOP_Q);
		(void)__io_put(iolp->iop);

		free(iolp);
	}
}

/*
 * Careful surgery
 * (1) Need to remove D & its I & M's
 * (2) Need to leave I's Q and M's Q's -- *no* io_put (__io_put instead)
 */
void handle_requeue(struct io *iop)
{
	struct io *d_iop;
	struct io_list *iolp;
	struct list_head *p, *q;

	d_iop = dip_find_start(dip_get_head_dev(iop->t.device, IOP_D),
			       iop->t.sector);
	if (d_iop) {
		list_for_each_safe(p, q, &d_iop->u.d.d_im_head) {
			iolp = list_entry(p, struct io_list, head);
			LIST_DEL(&iolp->head);

			if (iolp->iop->type == IOP_M)
				(void)__io_put(iolp->iop->u.m.m_q);
			else
				extract_i(iolp->iop);

			iolp->iop->users = 0;
			io_free(iolp->iop);
			free(iolp);
		}

		d_iop->users = 0;
		io_free(d_iop);
	}

	release_iop(iop);

}

void __add_trace(struct io *iop)
{
	n_traces++;

	if (verbose && (n_traces % 10000) == 0) {
		printf("%10lu t, %10lu m %1lu f\r",
		       n_traces, n_io_allocs, n_io_frees);
		fflush(stdout);
		if ((n_traces % 1000000) == 0) printf("\n");
	}

	switch (iop->t.action & 0xffff) {
	case __BLK_TA_QUEUE:		handle_queue(iop);	break;
	case __BLK_TA_BACKMERGE:	handle_merge(iop);	break;
	case __BLK_TA_FRONTMERGE:	handle_merge(iop);	break;
	case __BLK_TA_ISSUE:		handle_issue(iop);	break;
	case __BLK_TA_COMPLETE:		handle_complete(iop);	break;
	case __BLK_TA_INSERT:		handle_insert(iop);	break;
	case __BLK_TA_SPLIT:		handle_split(iop);	break;
	case __BLK_TA_REMAP:		handle_remap(iop);	break;
	case __BLK_TA_REQUEUE:		handle_requeue(iop);	break;
	}
}

void add_trace(struct io *iop)
{
	if (iop->t.action & BLK_TC_ACT(BLK_TC_NOTIFY)) {
		char *slash = strchr(iop->pdu, '/');

		if (slash)
			*slash = '\0';
		add_process(iop->t.pid, iop->pdu);
		release_iop(iop);
	}
	else if (iop->t.action & BLK_TC_ACT(BLK_TC_PC))
		release_iop(iop);
	else
		__add_trace(iop);
}
