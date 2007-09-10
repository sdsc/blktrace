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

static void handle_igm(struct io *igm_iop)
{
	LIST_HEAD(head);
	struct io *q_iop = dip_find_sec(igm_iop->dip, IOP_Q, igm_iop->t.sector);

	if (igm_iop->type == IOP_I) {
		if (q_iop)
			q_iop->i_time = igm_iop->t.time;
		return;
	}

	if (igm_iop->type == IOP_G) 
		iostat_getrq(igm_iop);
	else {
		assert(igm_iop->type == IOP_M);
		iostat_merge(igm_iop);
	}

	if (q_iop) {
		update_q2i(q_iop, tdelta(q_iop->t.time, igm_iop->t.time));
		q_iop->gm_time = igm_iop->t.time;
		q_iop->is_getrq = (igm_iop->type == IOP_G);
	}
}

void trace_insert(struct io *i_iop)
{
	if (io_setup(i_iop, IOP_I))
		handle_igm(i_iop);

	io_release(i_iop);
}

void trace_getrq(struct io *g_iop)
{
	if (io_setup(g_iop, IOP_G))
		handle_igm(g_iop);

	io_release(g_iop);
}

void trace_merge(struct io *m_iop)
{
	if (io_setup(m_iop, IOP_M))
		handle_igm(m_iop);

	io_release(m_iop);
}
