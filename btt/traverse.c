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

int tl_map[] = {
	IOP_Q,
	IOP_A,	/* IOP_X */
	IOP_A,
	IOP_I,	/* IOP_M */
	IOP_I,
	IOP_D,
	IOP_C,
	IOP_Y,
};


typedef void (*iop_func_t)(__u64 *, struct io*);

void iop_q_func(__u64 *timeline, struct io *iop);
void iop_x_func(__u64 *timeline, struct io *iop);
void iop_a_func(__u64 *timeline, struct io *iop);
void iop_m_func(__u64 *timeline, struct io *iop);
void iop_i_func(__u64 *timeline, struct io *iop);
void iop_d_func(__u64 *timeline, struct io *iop);
void iop_c_func(__u64 *timeline, struct io *iop);
void iop_y_func(__u64 *timeline, struct io *iop);

iop_func_t traverse_func[] = {
	iop_q_func,
	iop_x_func,
	iop_a_func,
	iop_m_func,
	iop_i_func,
	iop_d_func,
	iop_c_func,
	iop_y_func,
};

void __traverse(__u64 *timeline, struct io *iop)
{
	if (iop != NULL && !iop->traversed) {
		iop->traversed++;
		timeline[tl_map[iop->type]] = iop->t.time;
		(traverse_func[iop->type])(timeline, iop);
		timeline[tl_map[iop->type]] = 0;
	}
}

void traverse(struct io *iop)
{
	__u64 timeline[N_IOP_TYPES];

	memset(timeline, 0, N_IOP_TYPES * sizeof(__u64));
	__traverse(timeline, iop);
}

void iop_q_update(__u64 *timeline, struct io *iop, __u64 q_time)
{
	update_q2c(iop, timeline[IOP_C] - q_time);

	if (timeline[IOP_A] > 0.0)	// IOP_X too
		update_q2a(iop, timeline[IOP_A] - q_time);
	else				//IOP_M too
		update_q2i(iop, timeline[IOP_I] - q_time);

	update_i2d(iop, timeline[IOP_D] - timeline[IOP_I]);
	update_d2c(iop, timeline[IOP_C] - timeline[IOP_D]);
}

void iop_q_func(__u64 *timeline, struct io *iop)
{
	iop_q_update(timeline, iop, iop->t.time);
	if (iop->u.q.qp_type == Q_A)
		__traverse(timeline, iop->u.q.qp.q_a);
	else if (iop->u.q.qp_type == Q_X)
		__traverse(timeline, iop->u.q.qp.q_x);
}

void iop_x_func(__u64 *timeline, struct io *iop)
{
	__traverse(timeline, iop->u.x.x_q);
}

void iop_a_func(__u64 *timeline, struct io *iop)
{
	if (iop->u.a.ap_type == A_Q)
		__traverse(timeline, iop->u.a.ap.a_q);
	else if (iop->u.a.ap_type == A_A)
		__traverse(timeline, iop->u.a.ap.a_a);
}

void iop_m_func(__u64 *timeline, struct io *iop)
{
	__traverse(timeline, iop->u.m.m_q);
}

void iop_i_func(__u64 *timeline, struct io *iop)
{
	struct list_head *p;
	struct io_list *iolp;

	__list_for_each(p, &iop->u.i.i_qs_head) {
		iolp = list_entry(p, struct io_list, head);
		__traverse(timeline, iolp->iop);
	}
}

void iop_d_func(__u64 *timeline, struct io *iop)
{
	struct list_head *p;
	struct io_list *iolp;

	__list_for_each(p, &iop->u.d.d_im_head) {
		iolp = list_entry(p, struct io_list, head);
		__traverse(timeline, iolp->iop);
	}
}

void iop_c_func(__u64 *timeline, struct io *iop)
{
	__traverse(timeline, iop->u.c.c_d);
}

void iop_y_func(__u64 *timeline, struct io *iop)
{
	__traverse(timeline, iop->u.y.y_c1);
	__traverse(timeline, iop->u.y.y_c2);
}
