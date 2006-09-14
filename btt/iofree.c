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
#include <stdio.h>

#include "globals.h"

void io_free_resources(struct io *iop)
{
	switch (iop->type) {
	case IOP_X: io_unlink(&iop->u.x.x_q); break;
	case IOP_M: io_unlink(&iop->u.m.m_q); break;
	case IOP_C: io_unlink(&iop->u.c.c_d); break;

	case IOP_A:
		switch (iop->u.a.ap_type) {
		case A_NONE: break;
		case A_Q: io_unlink(&iop->u.a.ap.a_q); break;
		case A_A: io_unlink(&iop->u.a.ap.a_a); break;
		default:
			ASSERT(0);
			/*NOTREACHED*/
		}
		break;

	case IOP_Q:
		switch (iop->u.q.qp_type) {
		case Q_NONE: break;
		case Q_A: io_unlink(&iop->u.q.qp.q_a); break;
		case Q_X: io_unlink(&iop->u.q.qp.q_x); break;
		default:
			ASSERT(0);
			/*NOTREACHED*/
		}
		break;

	case IOP_D: {
		struct io_list *iolp;
		struct list_head *p, *q;

		list_for_each_safe(p, q, &iop->u.d.d_im_head) {
			iolp = list_entry(p, struct io_list, head);
			io_unlink(&iolp->iop);

			LIST_DEL(&iolp->head);
			free(iolp);
		}
		break;
	}

	case IOP_I: {
		struct io_list *iolp;
		struct list_head *p, *q;

		list_for_each_safe(p, q, &iop->u.i.i_qs_head) {
			iolp = list_entry(p, struct io_list, head);
			io_unlink(&iolp->iop);

			LIST_DEL(&iolp->head);
			free(iolp);
		}
		break;
	}

	case IOP_Y:
		io_unlink(&iop->u.y.y_c1);
		io_unlink(&iop->u.y.y_c2);
		break;

	default:
		ASSERT(0);
		/*NOTREACHED*/
	}
}
