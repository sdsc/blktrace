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

#include "blktrace.h"
#include "list.h"

#define pdu_start(t)	(((void *) (t) + sizeof(struct blk_io_trace)))

#define BIT_TIME(t)	((double)SECONDS(t) + ((double)NANO_SECONDS(t) / 1.0e9))

#define BIT_START(iop)	((iop)->t.sector)
#define BIT_END(iop)	((iop)->t.sector + ((iop)->t.bytes >> 9))

#if defined(DEBUG)
#define ASSERT(truth)   do {						\
				if (!(truth)) {				\
					DBG_PING();			\
					assert(truth);			\
				}					\
			} while (0)

#define DBG_PING()		dbg_ping()

#define LIST_DEL(hp)	list_del(hp)

#else
#define ASSERT(truth)
#define DBG_PING()

#define LIST_DEL(hp)	do {						\
				if (((hp)->next != NULL) &&		\
				    ((hp)->next != LIST_POISON1))	\
					list_del(hp);			\
			} while (0)
#endif

#define IO_ZALLOC()	my_zmalloc(&free_ios, sizeof(struct io))
#define IO_FREE(iop)	my_free(&free_ios, iop)

enum iop_type {
	IOP_Q = 0,
	IOP_X = 1,
	IOP_A = 2,
	IOP_M = 3,
	IOP_I = 4,
	IOP_D = 5,
	IOP_C = 6,
	IOP_Y = 7,
};
#define N_IOP_TYPES	(IOP_Y + 1)

struct my_mem {
	struct my_mem *next;
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

struct p_info;
struct p_pid {
	struct list_head head;
	struct p_info *pip;
	__u32 pid;
};

struct p_info {
	struct list_head head;
	struct region_info regions;
	struct avgs_info avgs;
	char *name;
	__u64 last_q;
};

struct devmap {
	struct devmap *next;
	char device[32];
	char model[64];
	unsigned int host, bus, target, lun;
	char node[32], pci[32];
	unsigned int irq, cpu;
	char devno[32];
};

struct d_info {
	struct list_head head, hash_head;
	struct list_head iop_heads[N_IOP_TYPES];
	struct region_info regions;
	struct avgs_info avgs;
	__u64 last_q;
	__u32 device;
	__u64 n_ds;
	struct devmap *map;
};

struct io {
	struct list_head all_head, dev_head;
	struct d_info *dip;
	struct p_info *pip;
	void *pdu;

	struct blk_io_trace t;

	int users, traversed;
	enum iop_type type;

	union {
		struct {
			union {
				struct io *q_a;
				struct io *q_x;
			} qp;
			enum {
				Q_NONE = 10,
				Q_A = 20,
				Q_X = 30,
			} qp_type;
		}					  q;
		struct { struct io *x_q;		} x;
		struct { struct io *a_q;		} a;
		struct { struct io *m_q;		} m;
		struct { struct list_head i_qs_head;	} i;
		struct { struct list_head d_im_head;	} d;
		struct { struct io *c_d;		} c;
		struct { struct io *y_c1, *y_c2;	} y;
	} u;
};

extern char bt_timeline_version[], *devices, *exes, *input_name, *output_name;
extern double range_delta;
extern FILE *ranges_ofp, *avgs_ofp;
extern int is_lvm, verbose, ifd;
extern unsigned int n_devs;
extern unsigned long n_traces, n_io_allocs, n_io_frees;
extern struct list_head all_devs, all_ios, all_procs;
extern struct avgs_info all_avgs;
extern __u64 last_q;
extern struct region_info all_regions;
extern struct my_mem *free_ios, *free_bits;
extern char iop_map[];
extern unsigned int pending_xs;

void add_trace(struct io *iop);
int in_devices(struct blk_io_trace *t);
int output_avgs(FILE *ofp);
int output_ranges(FILE *ofp);
unsigned int do_read(int ifd, void *buf, int len);
void add_process(__u32 pid, char *name);
struct p_info *find_process(__u32 pid, char *name);
void pip_update_q(struct io *iop);
void handle_args(int argc, char *argv[]);
struct devmap *dev_map_find(__u32 device);
int dev_map_read(char *fname);
void add_cy(struct io *iop);
void rem_c(struct io *iop);
void cy_init(void);
void cy_shutdown(void);
struct d_info *__dip_find(__u32 device);
struct d_info *dip_add(__u32 device, struct io *iop);
void traverse(struct io *iop);
void io_free_resources(struct io *iop);

#include "inlines.h"
