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

typedef struct io *iop_t;
typedef iop_t (*iop_func_t)(__u64 *, struct io*);

struct io *iop_q_func(__u64 *timeline, struct io *iop);
struct io *iop_x_func(__u64 *timeline, struct io *iop);
struct io *iop_a_func(__u64 *timeline, struct io *iop);
struct io *iop_m_func(__u64 *timeline, struct io *iop);
struct io *iop_i_func(__u64 *timeline, struct io *iop);
struct io *iop_d_func(__u64 *timeline, struct io *iop);
struct io *iop_c_func(__u64 *timeline, struct io *iop);
struct io *iop_y_func(__u64 *timeline, struct io *iop);

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
	while (iop != NULL && !iop->traversed) {
		iop->traversed++;
		iop = traverse_func[iop->type](timeline, iop);
	}
}

void traverse(struct io *iop)
{
	int i;
	__u64 timeline[N_IOP_TYPES];

	for (i = 0; i < N_IOP_TYPES; i++)
		timeline[i] = 0.0;

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

struct io *iop_q_func(__u64 *timeline, struct io *iop)
{
	timeline[IOP_Q] = iop->t.time;
	iop_q_update(timeline, iop, iop->t.time);

	if (iop->u.q.qp_type == Q_A)
		return iop->u.q.qp.q_a;
	else if (iop->u.q.qp_type == Q_X)
		return iop->u.q.qp.q_x;

	/* Q_NONE */
	return NULL;
}

struct io *iop_x_func(__u64 *timeline, struct io *iop)
{
	timeline[IOP_A] = iop->t.time;	// Cover X & A in one slice
	return iop->u.x.x_q;
}

struct io *iop_a_func(__u64 *timeline, struct io *iop)
{
	timeline[IOP_A] = iop->t.time;
	return iop->u.a.a_q;
}

struct io *iop_m_func(__u64 *timeline, struct io *iop)
{
	timeline[IOP_I] = iop->t.time;	// Cover M & I in one slice
	return iop->u.m.m_q;
}

struct io *iop_i_func(__u64 *timeline, struct io *iop)
{
	struct list_head *p;
	struct io_list *iolp;

	timeline[IOP_I] = iop->t.time;
	__list_for_each(p, &iop->u.i.i_qs_head) {
		iolp = list_entry(p, struct io_list, head);
		__traverse(timeline, iolp->iop);
	}

	return NULL;
}

struct io *iop_d_func(__u64 *timeline, struct io *iop)
{
	struct list_head *p;
	struct io_list *iolp;

	timeline[IOP_D] = iop->t.time;
	__list_for_each(p, &iop->u.d.d_im_head) {
		iolp = list_entry(p, struct io_list, head);
		__traverse(timeline, iolp->iop);
	}

	return NULL;
}

struct io *iop_c_func(__u64 *timeline, struct io *iop)
{
	timeline[IOP_C] = iop->t.time;
	return iop->u.c.c_d;
}

struct io *iop_y_func(__u64 *timeline, struct io *iop)
{
	timeline[IOP_Y] = iop->t.time;
	__traverse(timeline, iop->u.y.y_c1);
	__traverse(timeline, iop->u.y.y_c2);
	return NULL;
}
