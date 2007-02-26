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
		__u64 my_delta = (time > (*cur_p)->end) ? time - (*cur_p)->end : 1;
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

static inline void avg_update_n(struct avg_info *ap, __u64 t, int n)
{
        if (ap->n == 0) {
                ap->min = ap->max = t;
		ap->total = (n * t);
	}
        else {
                if (t < ap->min)
                        ap->min = t;
                else if (t > ap->max)
                        ap->max = t;
                ap->total += (n * t);
        }

	ap->n += n;
}

static inline void avg_unupdate(struct avg_info *ap, __u64 t)
{
	ap->n--;
	ap->total -= t;
}

static inline void update_lq(__u64 *last_q, struct avg_info *avg, __u64 time)
{
	if (*last_q != ((__u64)-1))
		avg_update(avg, (time > *last_q) ? time - *last_q : 1);
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

	memset(iop, 0, sizeof(struct io));
	INIT_LIST_HEAD(&iop->down_list);
	INIT_LIST_HEAD(&iop->up_list);

#if defined(DEBUG)
	iop->f_head.next = LIST_POISON1;
	iop->c_pending.next = LIST_POISON1;
	iop->retry.next = LIST_POISON1;
#endif

	return iop;
}

static inline void io_free(struct io *iop)
{
#	if defined(DEBUG)
		memset(iop, 0, sizeof(*iop));
#	endif
	list_add_tail(&iop->f_head, &free_ios);
}

static inline int io_setup(struct io *iop, enum iop_type type)
{
	iop->type = type;
	iop->dip = dip_add(iop->t.device, iop);
	if (iop->linked) {
		iop->pip = find_process(iop->t.pid, NULL);
		iop->bytes_left = iop->t.bytes;
	}

	return iop->linked;
}

static inline void io_release(struct io *iop)
{
	ASSERT(iop->f_head.next == LIST_POISON1);
	ASSERT(iop->c_pending.next == LIST_POISON1);
	ASSERT(iop->retry.next == LIST_POISON1);
	ASSERT(list_empty(&iop->up_list));
	ASSERT(list_empty(&iop->down_list));

	if (iop->linked)
		dip_rem(iop);
	if (iop->pdu) 
		free(iop->pdu);

	io_free(iop);
}

#define UPDATE_AVGS(_avg, _iop, _pip, _time) do {			\
		avg_update(&all_avgs. _avg , _time);			\
		avg_update(&_iop->dip->avgs. _avg , _time);		\
		if (_pip) avg_update(&_pip->avgs. _avg , _time);	\
	} while (0)

#define UPDATE_AVGS_N(_avg, _iop, _pip, _time, _n) do {			\
		avg_update_n(&all_avgs. _avg , _time, _n);		\
		avg_update_n(&_iop->dip->avgs. _avg , _time, _n);	\
		if (_pip) avg_update_n(&_pip->avgs. _avg , _time,_n);	\
	} while (0)

#define UNUPDATE_AVGS(_avg, _iop, _pip, _time) do {			\
		avg_unupdate(&all_avgs. _avg , _time);			\
		avg_unupdate(&_iop->dip->avgs. _avg , _time);		\
		if (_pip) avg_unupdate(&_pip->avgs. _avg , _time);	\
	} while (0)

static inline void update_q2c(struct io *iop, __u64 c_time)
{
#if defined(DEBUG)
	if (per_io_ofp) fprintf(per_io_ofp, "q2c %13.9f\n", BIT_TIME(c_time));
#endif
	UPDATE_AVGS(q2c, iop, iop->pip, c_time);
}

static inline void update_q2a(struct io *iop, __u64 a_time)
{
#if defined(DEBUG)
	if (per_io_ofp) fprintf(per_io_ofp, "q2a %13.9f\n", BIT_TIME(a_time));
#endif
	UPDATE_AVGS(q2a, iop, iop->pip, a_time);
}

static inline void update_q2i(struct io *iop, __u64 i_time)
{
#if defined(DEBUG)
	if (per_io_ofp) fprintf(per_io_ofp, "q2i %13.9f\n", BIT_TIME(i_time));
#endif
	UPDATE_AVGS(q2i, iop, iop->pip, i_time);
}

static inline void unupdate_q2i(struct io *iop, __u64 i_time)
{
	UNUPDATE_AVGS(q2i, iop, iop->pip, i_time);
}

static inline void update_i2d(struct io *iop, __u64 d_time)
{
#if defined(DEBUG)
	if (per_io_ofp) fprintf(per_io_ofp, "i2d %13.9f\n", BIT_TIME(d_time));
#endif
	UPDATE_AVGS(i2d, iop, iop->pip, d_time);
}

static inline void unupdate_i2d(struct io *iop, __u64 d_time)
{
	UNUPDATE_AVGS(i2d, iop, iop->pip, d_time);
}

static inline void update_d2c(struct io *iop, int n, __u64 c_time)
{
	if (per_io_ofp) fprintf(per_io_ofp, "d2c %13.9f\n", n*BIT_TIME(c_time));
	UPDATE_AVGS_N(d2c, iop, iop->pip, c_time, n);
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

static inline int dip_rb_ins(struct d_info *dip, struct io *iop)
{
	return rb_insert(__get_root(dip, iop->type), iop);
}

static inline void dip_rb_rem(struct io *iop)
{
	rb_erase(&iop->rb_node, __get_root(iop->dip, iop->type));
#if defined(DEBUG)
	rb_tree_size--;
#endif
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

static inline void bump_retry(__u64 now)
{
	if (!list_empty(&retries))
		next_retry_check = now + (100 * 1000); // 100 usec
	else 
		next_retry_check = 0;
}

static inline void add_retry(struct io *iop)
{
	bump_retry(iop->t.time);
	if (!iop->on_retry_list) {
		list_add_tail(&iop->retry, &retries);
		iop->on_retry_list = 1;
	}
}

static inline void del_retry(struct io *iop)
{
	if (iop->on_retry_list) {
		LIST_DEL(&iop->retry);
		iop->on_retry_list = 0;
	}
	bump_retry(iop->t.time);
}

static inline __u64 tdelta(struct io *iop1, struct io *iop2)
{
	__u64 t1 = iop1->t.time;
	__u64 t2 = iop2->t.time;
	return (t1 < t2) ? (t2 - t1) : 1;
}

static inline int remapper_dev(__u32 dev)
{
	int mjr = MAJOR(dev);
	return mjr == 9 || mjr == 253 || mjr == 254;
}

static inline void dump_iop(struct io *iop, int extra_nl)
{
	if (per_io_ofp) 
		__dump_iop(per_io_ofp, iop, extra_nl);
}

static inline void dump_iop2(struct io *a_iop, struct io *l_iop)
{
	if (per_io_ofp) 
		__dump_iop2(per_io_ofp, a_iop, l_iop);
}

static inline int type2c(enum iop_type type)
{
	int c;

	switch (type) {
	case IOP_Q: c = 'Q'; break;
	case IOP_X: c = 'X'; break;
	case IOP_A: c = 'A'; break;
	case IOP_I: c = 'I'; break;
	case IOP_M: c = 'M'; break;
	case IOP_D: c = 'D'; break;
	case IOP_C: c = 'C'; break;
	case IOP_R: c = 'R'; break;
	case IOP_L: c = 'L'; break;
	default   : c = '?'; break;
	}

	return c;
}

static inline void bilink_free(struct bilink *blp)
{
	free(blp);
}

static inline struct bilink *bilink_alloc(struct io *diop, struct io *uiop)
{
	struct bilink *blp = malloc(sizeof(*blp));

	blp->diop = diop;
	blp->uiop = uiop;

	return blp;
}

static inline void bilink(struct io *diop, struct io *uiop)
{
	struct bilink *blp = bilink_alloc(diop, uiop);

	list_add_tail(&blp->down_head, &diop->up_list);
	list_add_tail(&blp->up_head, &uiop->down_list);

	diop->up_len++;
	uiop->down_len++;
}

static inline void biunlink(struct bilink *blp)
{
	LIST_DEL(&blp->down_head);
	LIST_DEL(&blp->up_head);
	blp->diop->up_len--;
	blp->uiop->down_len--;
	bilink_free(blp);
}

static inline struct io *bilink_first_down(struct io *iop, 
 							struct bilink **blp_p)
{
	struct bilink *blp;

	if (list_empty(&iop->down_list))
		return NULL;
	blp = list_entry(iop->down_list.next, struct bilink, up_head);

	if (blp_p != NULL) 
		*blp_p = blp;
	return blp->diop;
}

static inline struct io *bilink_first_up(struct io *iop, struct bilink **blp_p)
{
	struct bilink *blp;

	if (list_empty(&iop->up_list))
		return NULL;
	blp = list_entry(iop->up_list.next, struct bilink, down_head);

	if (blp_p != NULL) 
		*blp_p = blp;
	return blp->uiop;
}

typedef void (*bilink_func)(struct io *diop, struct io *uiop, void *param);
static inline void bilink_for_each_down(bilink_func func, struct io *uiop, 
							void *param, int ul)
{
	struct bilink *blp;
	struct list_head *p, *q;

	list_for_each_safe(p, q, &uiop->down_list) {
		blp = list_entry(p, struct bilink, up_head);
		func(blp->diop, uiop, param);
		if (ul)
			biunlink(blp);
	}
}

static inline int histo_idx(__u64 nbytes)
{
	int idx = (nbytes >> 9) - 1;
	return min(idx, N_HIST_BKTS-1);
}

static inline void update_q_histo(__u64 nbytes)
{
	q_histo[histo_idx(nbytes)]++;
}

static inline void update_d_histo(__u64 nbytes)
{
	d_histo[histo_idx(nbytes)]++;
}
