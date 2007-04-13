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
	struct io *d_iop;

	if ((io_setup(r_iop, IOP_R) == 0) ||
	    (d_iop = dip_find_sec(r_iop->dip, IOP_D, 
	    					BIT_START(r_iop))) == NULL) {
		io_release(r_iop);
		return;
	}
	dip_rem(d_iop);

#	if defined(DEBUG)
		ASSERT(ready_issue(d_iop, r_iop) != 0);
#	else
		(void)ready_issue(d_iop, r_iop);
#	endif

	run_unissue(d_iop, r_iop, r_iop);
	add_rmhd(r_iop);

	release_iops();
}
