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
#include <string.h>

#include "globals.h"

#define N_PID_HASH	128
#define PID_HASH(pid)	((pid) & (N_PID_HASH-1))
struct list_head	*pid_heads = NULL;

static void init_pid_heads(void)
{
	int i;

	pid_heads = zmalloc(N_PID_HASH * sizeof(struct list_head));
	for (i = 0; i < N_PID_HASH; i++)
		INIT_LIST_HEAD(&pid_heads[i]);
}

static inline struct p_info *pid_to_proc(__u32 pid)
{
	struct p_pid *pp;
	struct list_head *p, *head;

	if (pid_heads == NULL) init_pid_heads();

	head = &pid_heads[PID_HASH(pid)];
	__list_for_each(p, head) {
		pp = list_entry(p, struct p_pid, head);
		if (pp->pid == pid)
			return pp->pip;
	}

	return NULL;
}

void insert_proc_hash(struct p_info *pip, __u32 pid)
{
	struct p_pid *pp = zmalloc(sizeof(*pp));

	if (pid_heads == NULL) init_pid_heads();

	pp->pip = pip;
	pp->pid = pid;

	list_add_tail(&pp->head, &pid_heads[PID_HASH(pid)]);
}

int __find_process_pid(__u32 pid)
{
	return pid_to_proc(pid) != NULL;
}

struct p_info *find_process(__u32 pid, char *name)
{
	struct p_info *pip;
	struct list_head *p;

	if (pid != ((__u32)-1) && (pip = pid_to_proc(pid)))
		return pip;

	if (name) {
		__list_for_each(p, &all_procs) {
			pip = list_entry(p, struct p_info, head);
			if (name && !strcmp(pip->name, name)) {
				if (pid != ((__u32)-1))
					insert_proc_hash(pip, pid);
				return pip;
			}
		}
	}

	return NULL;
}

void add_process(__u32 pid, char *name)
{
	struct p_info *pip = find_process(pid, name);

	if (pip == NULL) {
		pip = zmalloc(sizeof(*pip));

		list_add_tail(&pip->head, &all_procs);
		insert_proc_hash(pip, pid);
		pip->last_q = (__u64)-1;
		pip->name = strdup(name);
		init_region(&pip->regions);
	}
}

void pip_update_q(struct io *iop)
{
	if (iop->pip) {
		update_lq(&iop->pip->last_q, &iop->pip->avgs.q2q, iop->t.time);
		update_qregion(&iop->pip->regions, iop->t.time);
	}
}
