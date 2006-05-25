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

struct cy_list {
	struct list_head head;
	struct list_head list;
};

struct cy_list *pending_cys;

static inline void __rem_cy(struct io_list *iolp)
{
	io_unlink(&iolp->iop);
	if (--iolp->cy_users == 0) {
		LIST_DEL(&iolp->head);
		free(iolp);
	}
}

void rem_c(struct io *iop)
{
	struct list_head *p, *q;
	struct io_list *iolp;

	ASSERT(iop->type == IOP_C);

	list_for_each_safe(p, q, &pending_cys->list) {
		iolp = list_entry(p, struct io_list, head);

		if (iolp->iop == iop) {
			__rem_cy(iolp);
			break;
		}
	}
}

void run_cy_list(struct list_head *list)
{
	struct list_head *p, *q;
	struct io_list *iolp;

	list_for_each_safe(p, q, list) {
		iolp = list_entry(p, struct io_list, head);
		traverse(iolp->iop);
		__rem_cy(iolp);
	}
}

void add_cy(struct io *iop)
{
	struct io_list *iolp = malloc(sizeof(*iolp));

	ASSERT(iop->type == IOP_C || iop->type == IOP_Y);

	iolp->cy_users = 1;
	io_link(&iolp->iop, iop);

	list_add_tail(&iolp->head, &pending_cys->list);
	if (pending_xs == 0)
		run_cy_list(&pending_cys->list);
}

void cy_init(void)
{
	pending_cys = zmalloc(sizeof(*pending_cys));
	INIT_LIST_HEAD(&pending_cys->list);
}

void cy_shutdown(void)
{
	ASSERT(list_empty(&pending_cys->list));
	free(pending_cys);
}
