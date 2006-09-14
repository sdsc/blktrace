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
static inline void dbg_ping(void) { }

static inline void *zmalloc(size_t len)
{
	return memset(malloc(len), 0, len);
}

static inline int is_dev(struct io *iop, unsigned int mjr, unsigned int mnr)
{
	return MAJOR(iop->t.device) == mjr && MINOR(iop->t.device) == mnr;
}

static inline int in_bit(struct io *in, struct io *out)
{
	return (BIT_START(out) <= BIT_START(in)) &&
	       (BIT_END(in) <= BIT_END(out));
}

static inline int is_bit(struct io *i1, struct io *i2)
{
	return (BIT_START(i1) == BIT_START(i2)) && (BIT_END(i1) == BIT_END(i2));
}

static inline struct range_info *new_cur(__u64 time)
{
	struct range_info *cur = zmalloc(sizeof(*cur));

	INIT_LIST_HEAD(&cur->head);
	cur->start = time;
	return cur;
}

static inline  void update_range(struct list_head *head_p,
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

static inline void update_lq(__u64 *last_q, struct avg_info *avg, __u64 time)
{
	if (*last_q != ((__u64)-1))
		avg_update(avg, time - *last_q);
	*last_q = time;
}

static inline struct list_head *dip_get_head(struct d_info *dip,
                                             enum iop_type type)
{
	return &dip->iop_heads[type];
}

static inline struct list_head *dip_get_head_dev(__u32 dev, enum iop_type type)
{
	struct d_info *dip = __dip_find(dev);

	if (!dip) 
		return NULL;
	return dip_get_head(__dip_find(dev), type);
}

static inline void dip_update_q(struct d_info *dip, struct io *iop)
{
	update_lq(&dip->last_q, &dip->avgs.q2q, iop->t.time);
	update_qregion(&dip->regions, iop->t.time);
}

static inline void dip_rem(struct io *iop)
{
	LIST_DEL(&iop->dev_head);
}

static inline void *my_malloc(struct my_mem **head_p, size_t len)
{
	struct my_mem *this = *head_p;

	if (this)
		*head_p = this->next;
	else
		this = malloc(len);

	return this;
}

static inline void *my_zmalloc(struct my_mem **head_p, size_t len)
{
	return memset(my_malloc(head_p, len), 0, len);
}

static inline void my_free(struct my_mem **head_p, void *p)
{
	struct my_mem *this = p;

	this->next = *head_p;
	*head_p = this;
}

static inline void io_setup(struct io *iop, enum iop_type type)
{
	iop->type = type;
	iop->dip = dip_add(iop->t.device, iop);
	iop->pip = find_process(iop->t.pid, NULL);

	n_io_allocs++;
	list_add_tail(&iop->all_head, &all_ios);
}

static inline void io_free(struct io *iop)
{
	ASSERT(iop->users == 0);

	LIST_DEL(&iop->all_head);
	dip_rem(iop);
	IO_FREE(iop);
	n_io_frees++;
}

static inline void io_get(struct io *iop)
{
	iop->users++;
}


static inline int __io_put(struct io *iop)
{
	return --iop->users;
}

static inline void io_put(struct io *iop)
{
	if (__io_put(iop) == 0) {
		io_free_resources(iop);
		io_free(iop);
	}
}

static inline void io_link(struct io **p_dst, struct io *iop)
{
	ASSERT(iop != NULL);
	io_get(iop);
	*p_dst = iop;
}

static inline void io_unlink(struct io **p_dst)
{
	ASSERT(*p_dst != NULL);
	io_put(*p_dst);

#if defined(DEBUG)
	*p_dst = NULL;
#endif
}

#define UPDATE_AVGS(_avg, _iop, _pip, _time) do {			\
		avg_update(&all_avgs. _avg , _time);			\
		avg_update(&_iop->dip->avgs. _avg , _time);		\
		if (_pip) avg_update(&_pip->avgs. _avg , _time);	\
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

static inline void update_d2c(struct io *iop, __u64 c_time)
{
	UPDATE_AVGS(d2c, iop, iop->pip, c_time);
}

static inline void update_blks(struct io *iop)
{
	__u64 nblks = iop->t.bytes >> 9;
	avg_update(&all_avgs.blks, nblks);
	avg_update(&iop->dip->avgs.blks, nblks);
	if (iop->pip)
		avg_update(&iop->pip->avgs.blks, nblks);
}
