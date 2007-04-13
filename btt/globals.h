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
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "blktrace.h"
#include "rbtree.h"
#include "list.h"

/*
 * 0 == 1 blk
 * 1 == 2 blks
 * ...
 * 1022 == 1023 blks
 * 1023 == 1024 blks
 * 1024 == > 1024 blks
 */
#define N_HIST_BKTS	1025

#define BIT_TIME(t)	((double)SECONDS(t) + ((double)NANO_SECONDS(t) / 1.0e9))

#define BIT_START(iop)	((iop)->t.sector)
#define BIT_END(iop)	((iop)->t.sector + ((iop)->t.bytes >> 9))
#define IOP_READ(iop)	((iop)->t.action & BLK_TC_ACT(BLK_TC_READ))
#define IOP_RW(iop)	(IOP_READ(iop) ? 1 : 0)

#define TO_SEC(nanosec)	((double)(nanosec) / 1.0e9)
#define TO_MSEC(nanosec) (1000.0 * TO_SEC(nanosec))

#if defined(DEBUG)
#define DBG_PING()	dbg_ping()
#define ASSERT(truth)   do {						\
				if (!(truth)) {				\
					DBG_PING();			\
					assert(truth);			\
				}					\
			} while (0)


#define LIST_DEL(hp)	list_del(hp)
#else
#define ASSERT(truth)
#define DBG_PING()
#define LIST_DEL(hp)	do {						\
				ASSERT((hp)->next != NULL);		\
				ASSERT(!list_empty(hp));		\
				list_del(hp);				\
			} while (0)
#endif

enum iop_type {
	IOP_Q = 0,
	IOP_X = 1,
	IOP_A = 2,
	IOP_I = 3,
	IOP_M = 4,
	IOP_D = 5,
	IOP_C = 6,
	IOP_R = 7,
	IOP_L = 8, 	// IOP_A -> IOP_A + IOP_L
};
#define N_IOP_TYPES	(IOP_L + 1)

struct file_info {
	struct file_info *next;
	FILE *ofp;
	char oname[1];
};

struct mode {
	int most_seeks, nmds;
	long long *modes;
};

struct io;
struct io_list {
	struct list_head head;
	struct io *iop;
	int cy_users;
};

struct avg_info {
	__u64 min, max, total;
	double avg;
	int n;
};

struct avgs_info {
        struct avg_info q2q;
	struct avg_info q2c;
	struct avg_info q2a;		/* Q to (A or X) */
	struct avg_info q2i;		/* Q to (I or M) */
	struct avg_info i2d;		/* (I or M) to D */
	struct avg_info d2c;

	struct avg_info blks;		/* Blocks transferred */
};

struct range_info {
	struct list_head head;		/* on: qranges OR cranges */
	__u64 start, end;
};

struct region_info {
	struct list_head qranges;
	struct list_head cranges;
	struct range_info *qr_cur, *cr_cur;
};

struct p_info {
	struct region_info regions;
	struct avgs_info avgs;
	__u64 last_q;
	__u32 pid;
	char name[1];
};

struct devmap {
	struct devmap *next;
	unsigned int host, bus, target, lun, irq, cpu;
	char model[64];
	char device[32], node[32], pci[32], devno[32];
};

struct stats {
	__u64 rqm[2], ios[2], sec[2], wait, svctm;
	double last_qu_change, last_dev_change, tot_qusz, idle_time;
	int cur_qusz, cur_dev;
};

struct stats_t {
	double n;
	double rqm_s[2], ios_s[2], sec_s[2];
	double avgrq_sz, avgqu_sz, await, svctm, p_util;
};

struct d_info {
	struct list_head all_head, hash_head;
	void *heads;
	struct region_info regions;
	struct devmap *map;
	void *seek_handle;
	FILE *d2c_ofp, *q2c_ofp;
	struct avgs_info avgs;
	struct stats stats, all_stats;
	__u64 last_q, n_ds;
	__u32 device;

	int pre_culling;
	int is_plugged, nplugs, n_timer_unplugs;
	double start_time, last_plug, plugged_time, end_time;
};

struct io {
	struct rb_node rb_node;
	struct list_head f_head, c_pending, retry, rm_head;
	struct list_head down_list, up_list;
	struct d_info *dip;
	struct p_info *pip;
	void *pdu;
	struct blk_io_trace t;
	__u64 bytes_left;
	int linked, on_retry_list, down_len, up_len, on_rm_list;
	enum iop_type type;
#if defined(COUNT_IOS)
	struct list_head cio_head;
#endif
};

struct bilink {
	struct list_head down_head, up_head;
	struct io *diop, *uiop;
};

/* bt_timeline.c */

extern char bt_timeline_version[], *devices, *exes, *input_name, *output_name;
extern char *seek_name, *iostat_name, *d2c_name, *q2c_name, *per_io_name;
extern double range_delta;
extern FILE *ranges_ofp, *avgs_ofp, *iostat_ofp, *per_io_ofp;;
extern int verbose, done, time_bounded, output_all_data;
extern unsigned int n_devs;
extern unsigned long n_traces;
extern struct list_head all_devs, all_procs, retries, rmhd;
extern struct avgs_info all_avgs;
extern __u64 last_q, next_retry_check;
extern struct region_info all_regions;
extern struct list_head free_ios;
extern __u64 iostat_interval, iostat_last_stamp;
extern time_t genesis, last_vtrace;
extern double t_astart, t_aend;
extern __u64 q_histo[N_HIST_BKTS], d_histo[N_HIST_BKTS];
#if defined(DEBUG)
extern int rb_tree_size;
#endif
#if defined(COUNT_IOS)
extern unsigned long nios_reused, nios_alloced, nios_freed;
extern struct list_head cios;
#endif

/* args.c */
void handle_args(int argc, char *argv[]);

/* dev_map.c */
int dev_map_read(char *fname);
struct devmap *dev_map_find(__u32 device);

/* devs.c */
#if defined(DEBUG)
void dump_rb_trees(void);
#endif
void init_dev_heads(void);
struct d_info *dip_add(__u32 device, struct io *iop);
void dip_rem(struct io *iop);
struct d_info *__dip_find(__u32 device);
void dip_foreach_list(struct io *iop, enum iop_type type, struct list_head *hd);
void dip_foreach(struct io *iop, enum iop_type type, 
		 void (*fnc)(struct io *iop, struct io *this), int rm_after);
struct io *dip_find_sec(struct d_info *dip, enum iop_type type, __u64 sec);
void dip_foreach_out(void (*func)(struct d_info *, void *), void *arg);
void dip_plug(__u32 dev, double cur_time);
void dip_unplug(__u32 dev, double cur_time, int is_timer);

/* dip_rb.c */
int rb_insert(struct rb_root *root, struct io *iop);
struct io *rb_find_sec(struct rb_root *root, __u64 sec);
void rb_foreach(struct rb_node *n, struct io *iop, 
		      void (*fnc)(struct io *iop, struct io *this),
		      struct list_head *head);

/* iostat.c */
void iostat_init(void);
void iostat_insert(struct io *iop);
void iostat_merge(struct io *iop);
void iostat_issue(struct io *iop);
void iostat_unissue(struct io *iop);
void iostat_complete(struct io *d_iop, struct io *c_iop);
void iostat_check_time(__u64 stamp);
void iostat_dump_stats(__u64 stamp, int all);

/* latency.c */
void latency_init(struct d_info *dip);
void latency_clean(void);
void latency_d2c(struct d_info *dip, __u64 tstamp, __u64 latency);
void latency_q2c(struct d_info *dip, __u64 tstamp, __u64 latency);

/* misc.c */
int in_devices(struct blk_io_trace *t);
void add_file(struct file_info **fipp, FILE *fp, char *oname);
void clean_files(struct file_info **fipp);
void dbg_ping(void);

/* mmap.c */
void setup_ifile(char *fname);
void cleanup_ifile(void);
int next_trace(struct blk_io_trace *t, void **pdu);

/* output.c */
int output_avgs(FILE *ofp);
int output_ranges(FILE *ofp);
char *make_dev_hdr(char *pad, size_t len, struct d_info *dip);

/* proc.c */
void add_process(__u32 pid, char *name);
struct p_info *find_process(__u32 pid, char *name);
void pip_update_q(struct io *iop);
void pip_foreach_out(void (*f)(struct p_info *, void *), void *arg);

/* seek.c */
void *seeki_init(__u32 device);
void seek_clean(void);
void seeki_add(void *handle, struct io *iop);
double seeki_mean(void *handle);
long long seeki_nseeks(void *handle);
long long seeki_median(void *handle);
int seeki_mode(void *handle, struct mode *mp);

/* trace.c */
void __dump_iop(FILE *ofp, struct io *iop, int extra_nl);
void __dump_iop2(FILE *ofp, struct io *a_iop, struct io *l_iop);
void release_iops(void);
void add_trace(struct io *iop);
void do_retries(__u64 now);

/* trace_complete.c */
void trace_complete(struct io *c_iop);
void retry_complete(struct io *c_iop, __u64 now);

/* trace_im.c */
void run_im(struct io *im_iop, struct io *d_iop, struct io *c_iop);
void run_unim(struct io *im_iop, struct io *d_iop, struct io *c_iop);
int ready_im(struct io *im_iop, struct io *c_iop);
void trace_insert(struct io *i_iop);
void trace_merge(struct io *m_iop);

/* trace_issue.c */
void run_issue(struct io *d_iop, struct io *u_iop, struct io *c_iop);
void run_unissue(struct io *d_iop, struct io *u_iop, struct io *c_iop);
int ready_issue(struct io *d_iop, struct io *c_iop);
void trace_issue(struct io *d_iop);

/* trace_plug.c */
void trace_plug(struct io *p_iop);
void trace_unplug_io(struct io *u_iop);
void trace_unplug_timer(struct io *u_iop);

/* trace_queue.c */
void run_queue(struct io *q_iop, struct io *u_iop, struct io *c_iop);
int ready_queue(struct io *q_iop, struct io *c_iop);
void trace_queue(struct io *q_iop);

/* trace_remap.c */
void run_remap(struct io *a_iop, struct io *u_iop, struct io *c_iop);
int ready_remap(struct io *a_iop, struct io *c_iop);
void trace_remap(struct io *a_iop);

/* trace_requeue.c */
void trace_requeue(struct io *r_iop);

#include "inlines.h"
