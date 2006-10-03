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

static inline struct range_info *new_cur(__u64 time)
{
	struct range_info *cur = malloc(sizeof(struct range_info));

	INIT_LIST_HEAD(&cur->head);
	cur->start = time;
	return cur;
}

static inline void update_range(struct list_head *head_p,
				struct range_info **cur_p, __u64 time)
{
	if (*cur_p == NULL)
		*cur_p = new_cur(time);
	else {
		__u64 my_delta = time - (*cur_p)->end;
		if (BIT_TIME(my_delta) >= range_delta) {
			list_add_tail(&(*cur_p)->head, head_p);
			*cur_p = new_cur(time);
		}
	}

	(*cur_p)->end = time;
}

static inline void init_region(struct region_info *reg)
{
	INIT_LIST_HEAD(&reg->qranges);
	INIT_LIST_HEAD(&reg->cranges);
	reg->qr_cur = reg->cr_cur = NULL;
}

static inline void update_qregion(struct region_info *reg, __u64 time)
{
	update_range(&reg->qranges, &reg->qr_cur, time);
}

static inline void update_cregion(struct region_info *reg, __u64 time)
{
	update_range(&reg->cranges, &reg->cr_cur, time);
}

static inline void avg_update(struct avg_info *ap, __u64 t)
{
        if (ap->n++ == 0)
                ap->min = ap->total = ap->max = t;
        else {
                if (t < ap->min)
                        ap->min = t;
                else if (t > ap->max)
                        ap->max = t;
                ap->total += t;
        }
}

static inline void avg_unupdate(struct avg_info *ap, __u64 t)
{
	ap->n--;
	ap->total -= t;
}

static inline void update_lq(__u64 *last_q, struct avg_info *avg, __u64 time)
{
	if (*last_q != ((__u64)-1))
		avg_update(avg, time - *last_q);
	*last_q = time;
}

static inline void dip_update_q(struct d_info *dip, struct io *iop)
{
	update_lq(&dip->last_q, &dip->avgs.q2q, iop->t.time);
	update_qregion(&dip->regions, iop->t.time);
}

static inline struct io *io_alloc(void)
{
	struct io *iop;

	if (!list_empty(&free_ios)) {
		iop = list_entry(free_ios.next, struct io, f_head);
		LIST_DEL(&iop->f_head);
	}
	else
		iop = malloc(sizeof(struct io));

	return memset(iop, 0, sizeof(struct io));
}

static inline void io_free(struct io *iop)
{
	list_add_tail(&iop->f_head, &free_ios);
}

static inline void io_setup(struct io *iop, enum iop_type type, int link)
{
	iop->type = type;
	iop->dip = dip_add(iop->t.device, iop, link);
	iop->pip = find_process(iop->t.pid, NULL);
	iop->linked = link;
}

static inline void io_release(struct io *iop)
{
	if (iop->linked) {
		dip_rem(iop);
		iop->linked = 0;
	}
	if (iop->pdu) 
		free(iop->pdu);
	io_free(iop);
}

#define UPDATE_AVGS(_avg, _iop, _pip, _time) do {			\
		avg_update(&all_avgs. _avg , _time);			\
		avg_update(&_iop->dip->avgs. _avg , _time);		\
		if (_pip) avg_update(&_pip->avgs. _avg , _time);	\
	} while (0)

#define UNUPDATE_AVGS(_avg, _iop, _pip, _time) do {			\
		avg_unupdate(&all_avgs. _avg , _time);			\
		avg_unupdate(&_iop->dip->avgs. _avg , _time);		\
		if (_pip) avg_unupdate(&_pip->avgs. _avg , _time);	\
	} while (0)

static inline void update_q2c(struct io *iop, __u64 c_time)
{
	UPDATE_AVGS(q2c, iop, iop->pip, c_time);
}

static inline void update_q2a(struct io *iop, __u64 a_time)
{
	UPDATE_AVGS(q2a, iop, iop->pip, a_time);
}

static inline void update_q2i(struct io *iop, __u64 i_time)
{
	UPDATE_AVGS(q2i, iop, iop->pip, i_time);
}

static inline void update_i2d(struct io *iop, __u64 d_time)
{
	UPDATE_AVGS(i2d, iop, iop->pip, d_time);
}

static inline void unupdate_i2d(struct io *iop, __u64 d_time)
{
	UNUPDATE_AVGS(i2d, iop, iop->pip, d_time);
}

static inline void update_d2c(struct io *iop, __u64 c_time)
{
	UPDATE_AVGS(d2c, iop, iop->pip, c_time);
}

static inline void update_blks(struct io *iop)
{
	__u64 nblks = iop->t.bytes >> 9;
	avg_update(&all_avgs.blks, nblks);
	ASSERT(iop->dip != NULL);
	avg_update(&iop->dip->avgs.blks, nblks);
	if (iop->pip)
		avg_update(&iop->pip->avgs.blks, nblks);
}

static inline struct rb_root *__get_root(struct d_info *dip, enum iop_type type)
{
	struct rb_root *roots = dip->heads;
	return &roots[type];
}

static inline void *dip_rb_mkhds(void)
{
	size_t len = N_IOP_TYPES * sizeof(struct rb_root);
	return memset(malloc(len), 0, len);
}

static inline void dip_rb_ins(struct d_info *dip, struct io *iop)
{
	rb_insert(__get_root(dip, iop->type), iop);
}

static inline void dip_rb_rem(struct io *iop)
{
	rb_erase(&iop->rb_node, __get_root(iop->dip, iop->type));
}

static inline void dip_rb_fe(struct d_info *dip, enum iop_type type, 
		             struct io *iop, 
			     void (*fnc)(struct io *iop, struct io *this), 
			     struct list_head *head)
{
	rb_foreach(__get_root(dip, type)->rb_node, iop, fnc, head);
}

static inline struct io *dip_rb_find_sec(struct d_info *dip, 
		                         enum iop_type type, __u64 sec)
{
	return rb_find_sec(__get_root(dip, type), sec);
}
