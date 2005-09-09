/*
 * block queue tracing parse application
 *
 * Copyright (C) 2005 Jens Axboe <axboe@suse.de>
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
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <errno.h>
#include <signal.h>
#include <locale.h>

#include "blktrace.h"
#include "rbtree.h"

#define SECONDS(x) 		((unsigned long long)(x) / 1000000000)
#define NANO_SECONDS(x)		((unsigned long long)(x) % 1000000000)

static int backwards;
static unsigned long long genesis_time, last_reported_time;

struct io_stats {
	unsigned long qreads, qwrites, creads, cwrites, mreads, mwrites;
	unsigned long ireads, iwrites;
	unsigned long long qread_kb, qwrite_kb, cread_kb, cwrite_kb;
	unsigned long long iread_kb, iwrite_kb;
};

struct per_cpu_info {
	int cpu;
	int nelems;

	int fd;
	char fname[128];

	struct io_stats io_stats;
};

struct per_process_info {
	char name[16];
	__u32 pid;
	struct io_stats io_stats;
	struct per_process_info *hash_next, *list_next;
};

#define PPI_HASH_SHIFT	(8)
static struct per_process_info *ppi_hash[1 << PPI_HASH_SHIFT];
static struct per_process_info *ppi_list;

#define S_OPTS	"i:o:b:st"
static struct option l_opts[] = {
	{
		.name = "input",
		.has_arg = 1,
		.flag = NULL,
		.val = 'i'
	},
	{
		.name = "output",
		.has_arg = 1,
		.flag = NULL,
		.val = 'o'
	},
	{
		.name = "batch",
		.has_arg = 1,
		.flag = NULL,
		.val = 'b'
	},
	{
		.name = "per program stats",
		.has_arg = 0,
		.flag = NULL,
		.val = 's'
	},
	{
		.name = "track ios",
		.has_arg = 0,
		.flag = NULL,
		.val = 't'
	},
	{
		.name = NULL,
		.has_arg = 0,
		.flag = NULL,
		.val = 0
	}
};

static struct rb_root rb_sort_root;
static struct rb_root rb_track_root;

/*
 * for sorting the displayed output
 */
struct trace {
	struct blk_io_trace *bit;
	struct rb_node rb_node;
};

/*
 * for tracking individual ios
 */
struct io_track {
	struct rb_node rb_node;

	__u64 sector;
	__u32 pid;
	unsigned long long queue_time;
	unsigned long long dispatch_time;
	unsigned long long completion_time;
};

static int max_cpus;
static struct per_cpu_info *per_cpu_info;

static unsigned long long events;

static char *dev, *output_name;
static FILE *ofp;

static int per_process_stats;
static int track_ios;

#define RB_BATCH_DEFAULT	(1024)
static int rb_batch = RB_BATCH_DEFAULT;

#define is_done()	(*(volatile int *)(&done))
static volatile int done;

static inline unsigned long hash_long(unsigned long val)
{
#if __WORDSIZE == 32
	val *= 0x9e370001UL;
#elif __WORDSIZE == 64
	val *= 0x9e37fffffffc0001UL;
#else
#error unknown word size
#endif

	return val >> (__WORDSIZE - PPI_HASH_SHIFT);
}

static inline void add_process_to_hash(struct per_process_info *ppi)
{
	const int hash_idx = hash_long(ppi->pid);

	ppi->hash_next = ppi_hash[hash_idx];
	ppi_hash[hash_idx] = ppi;
}

static inline void add_process_to_list(struct per_process_info *ppi)
{
	ppi->list_next = ppi_list;
	ppi_list = ppi;
}

static struct per_process_info *find_process_by_pid(__u32 pid)
{
	const int hash_idx = hash_long(pid);
	struct per_process_info *ppi;

	ppi = ppi_hash[hash_idx];
	while (ppi) {
		if (ppi->pid == pid)
			return ppi;

		ppi = ppi->hash_next;
	}

	return NULL;
}

static inline int trace_rb_insert(struct trace *t)
{
	struct rb_node **p = &rb_sort_root.rb_node;
	struct rb_node *parent = NULL;
	struct trace *__t;

	while (*p) {
		parent = *p;
		__t = rb_entry(parent, struct trace, rb_node);

		if (t->bit->sequence < __t->bit->sequence)
			p = &(*p)->rb_left;
		else if (t->bit->sequence > __t->bit->sequence)
			p = &(*p)->rb_right;
		else {
			fprintf(stderr, "sequence alias!\n");
			return 1;
		}
	}

	rb_link_node(&t->rb_node, parent, p);
	rb_insert_color(&t->rb_node, &rb_sort_root);
	return 0;
}

static inline int track_rb_insert(struct io_track *iot)
{
	struct rb_node **p = &rb_track_root.rb_node;
	struct rb_node *parent = NULL;
	struct io_track *__iot;

	while (*p) {
		parent = *p;
		
		__iot = rb_entry(parent, struct io_track, rb_node);

		if (iot->sector < __iot->sector)
			p = &(*p)->rb_left;
		else if (iot->sector > __iot->sector)
			p = &(*p)->rb_right;
		else {
			fprintf(stderr, "sequence alias!\n");
			return 1;
		}
	}

	rb_link_node(&iot->rb_node, parent, p);
	rb_insert_color(&iot->rb_node, &rb_track_root);
	return 0;
}

static struct io_track *__find_track(__u64 sector)
{
	struct rb_node **p = &rb_track_root.rb_node;
	struct rb_node *parent = NULL;
	struct io_track *__iot;

	while (*p) {
		parent = *p;
		
		__iot = rb_entry(parent, struct io_track, rb_node);

		if (sector < __iot->sector)
			p = &(*p)->rb_left;
		else if (sector > __iot->sector)
			p = &(*p)->rb_right;
		else
			return __iot;
	}

	return NULL;
}

static struct io_track *find_track(__u64 sector)
{
	struct io_track *iot = __find_track(sector);

	iot = __find_track(sector);
	if (!iot) {
		iot = malloc(sizeof(*iot));
		iot->sector = sector;
		track_rb_insert(iot);
	}

	return iot;
}

static void log_track_merge(struct blk_io_trace *t)
{
	struct io_track *iot;

	if (!track_ios)
		return;
	if ((t->action & BLK_TC_ACT(BLK_TC_FS)) == 0)
		return;

	iot = __find_track(t->sector - (t->bytes >> 10));
	if (!iot) {
		fprintf(stderr, "Trying to merge on non-existing request\n");
		return;
	}

	rb_erase(&iot->rb_node, &rb_track_root);
	iot->sector -= t->bytes >> 10;
	track_rb_insert(iot);
}

static void log_track_queue(struct blk_io_trace *t)
{
	struct io_track *iot;

	if (!track_ios)
		return;

	iot = find_track(t->sector);
	iot->queue_time = t->time;
}

/*
 * return time between queue and issue
 */
static unsigned long long log_track_issue(struct blk_io_trace *t)
{
	struct io_track *iot;

	if (!track_ios)
		return -1;
	if ((t->action & BLK_TC_ACT(BLK_TC_FS)) == 0)
		return -1;

	iot = __find_track(t->sector);
	if (!iot) {
		fprintf(stderr, "Trying to issue on non-existing request\n");
		return -1;
	}

	iot->dispatch_time = t->time;
	return iot->dispatch_time - iot->queue_time;
}

/*
 * return time between dispatch and complete
 */
static unsigned long long log_track_complete(struct blk_io_trace *t)
{
	unsigned long long elapsed;
	struct io_track *iot;

	if (!track_ios)
		return -1;
	if ((t->action & BLK_TC_ACT(BLK_TC_FS)) == 0)
		return -1;

	iot = __find_track(t->sector);
	if (!iot) {
		fprintf(stderr, "Trying to dispatch on non-existing request\n");
		return -1;
	}

	iot->completion_time = t->time;
	elapsed = iot->completion_time - iot->dispatch_time;

	/*
	 * kill the trace, we don't need it after completion
	 */
	rb_erase(&iot->rb_node, &rb_track_root);
	free(iot);

	return elapsed;
}


static struct io_stats *find_process_io_stats(__u32 pid, char *name)
{
	struct per_process_info *ppi = find_process_by_pid(pid);

	if (!ppi) {
		ppi = malloc(sizeof(*ppi));
		memset(ppi, 0, sizeof(*ppi));
		strncpy(ppi->name, name, sizeof(ppi->name));
		ppi->pid = pid;
		add_process_to_hash(ppi);
		add_process_to_list(ppi);
	}

	return &ppi->io_stats;
}

static void resize_cpu_info(int cpuid)
{
	int new_space, new_max = cpuid + 1;
	char *new_start;

	per_cpu_info = realloc(per_cpu_info, new_max * sizeof(*per_cpu_info));
	if (!per_cpu_info) {
		fprintf(stderr, "Cannot allocate CPU info -- %d x %d bytes\n",
			new_max, (int) sizeof(*per_cpu_info));
		exit(1);
	}

	new_start = (char *)per_cpu_info + (max_cpus * sizeof(*per_cpu_info));
	new_space = (new_max - max_cpus) * sizeof(*per_cpu_info);
	memset(new_start, 0, new_space);
	max_cpus = new_max;
}

static struct per_cpu_info *get_cpu_info(int cpu)
{
	struct per_cpu_info *pci;

	if (cpu >= max_cpus)
		resize_cpu_info(cpu);

	/*
	 * ->cpu might already be set, but just set it unconditionally
	 */
	pci = &per_cpu_info[cpu];
	pci->cpu = cpu;

	return pci;
}

static inline void check_time(struct blk_io_trace *bit)
{
	unsigned long long this = bit->time;
	unsigned long long last = last_reported_time;

	backwards = (this < last) ? 'B' : ' ';
	last_reported_time = this;
}

static inline void __account_m(struct io_stats *ios, struct blk_io_trace *t,
			       int rw)
{
	if (rw) {
		ios->mwrites++;
		ios->qwrite_kb += t->bytes >> 10;
	} else {
		ios->mreads++;
		ios->qread_kb += t->bytes >> 10;
	}
}

static inline void account_m(struct blk_io_trace *t, struct per_cpu_info *pci,
			     int rw)
{
	__account_m(&pci->io_stats, t, rw);

	if (per_process_stats) {
		struct io_stats *ios = find_process_io_stats(t->pid, t->comm);

		__account_m(ios, t, rw);
	}
}

static inline void __account_q(struct io_stats *ios, struct blk_io_trace *t,
			       int rw)
{
	if (rw) {
		ios->qwrites++;
		ios->qwrite_kb += t->bytes >> 10;
	} else {
		ios->qreads++;
		ios->qread_kb += t->bytes >> 10;
	}
}

static inline void account_q(struct blk_io_trace *t, struct per_cpu_info *pci,
			     int rw)
{
	__account_q(&pci->io_stats, t, rw);

	if (per_process_stats) {
		struct io_stats *ios = find_process_io_stats(t->pid, t->comm);

		__account_q(ios, t, rw);
	}
}

static inline void __account_c(struct io_stats *ios, int rw, unsigned int bytes)
{
	if (rw) {
		ios->cwrites++;
		ios->cwrite_kb += bytes >> 10;
	} else {
		ios->creads++;
		ios->cread_kb += bytes >> 10;
	}
}

static inline void account_c(struct blk_io_trace *t, struct per_cpu_info *pci,
			     int rw, int bytes)
{
	__account_c(&pci->io_stats, rw, bytes);

	if (per_process_stats) {
		struct io_stats *ios = find_process_io_stats(t->pid, t->comm);

		__account_c(ios, rw, bytes);
	}
}

static inline void __account_i(struct io_stats *ios, int rw, unsigned int bytes)
{
	if (rw) {
		ios->iwrites++;
		ios->iwrite_kb += bytes >> 10;
	} else {
		ios->ireads++;
		ios->iread_kb += bytes >> 10;
	}
}

static inline void account_i(struct blk_io_trace *t, struct per_cpu_info *pci,
			     int rw)
{
	__account_i(&pci->io_stats, rw, t->bytes);

	if (per_process_stats) {
		struct io_stats *ios = find_process_io_stats(t->pid, t->comm);

		__account_i(ios, rw, t->bytes);
	}
}

static void output(struct per_cpu_info *pci, char *s)
{
	fprintf(ofp, "%s", s);
}

static char hstring[256];
static char tstring[256];

static inline char *setup_header(struct per_cpu_info *pci,
				 struct blk_io_trace *t, char act)
{
	int w = t->action & BLK_TC_ACT(BLK_TC_WRITE);
	int b = t->action & BLK_TC_ACT(BLK_TC_BARRIER);
	int s = t->action & BLK_TC_ACT(BLK_TC_SYNC);
	char rwbs[4];
	int i = 0;

	if (w)
		rwbs[i++] = 'W';
	else
		rwbs[i++] = 'R';
	if (b)
		rwbs[i++] = 'B';
	if (s)
		rwbs[i++] = 'S';

	rwbs[i] = '\0';

	sprintf(hstring, "%2d %15ld %5Lu.%09Lu %5u %c %3s",
		pci->cpu,
		(unsigned long)t->sequence, SECONDS(t->time), 
		NANO_SECONDS(t->time), t->pid, act, rwbs);

	return hstring;
}

static void log_complete(struct per_cpu_info *pci, struct blk_io_trace *t,
			 char act)
{
	unsigned long long elapsed = log_track_complete(t);

	if (elapsed != -1ULL) {
		double usec = (double) elapsed / 1000;

		sprintf(tstring,"%s %Lu + %u (%4.2f) [%d]\n",
			setup_header(pci, t, act),
			(unsigned long long)t->sector, t->bytes >> 9,
			usec, t->error);
	} else {
		sprintf(tstring,"%s %Lu + %u [%d]\n", setup_header(pci, t, act),
			(unsigned long long)t->sector, t->bytes >> 9, t->error);
	}
	
	output(pci, tstring);
}

static void log_queue(struct per_cpu_info *pci, struct blk_io_trace *t,
		      char act)
{
	log_track_queue(t);

	sprintf(tstring,"%s %Lu + %u [%s]\n", setup_header(pci, t, act),
		(unsigned long long)t->sector, t->bytes >> 9, t->comm);
	output(pci, tstring);
}

static void log_issue(struct per_cpu_info *pci, struct blk_io_trace *t,
		      char act)
{
	unsigned long long elapsed = log_track_issue(t);

	if (elapsed != -1ULL) {
		double usec = (double) elapsed / 1000;

		sprintf(tstring,"%s %Lu + %u (%4.2f) [%s]\n",
			setup_header(pci, t, act),
			(unsigned long long)t->sector, t->bytes >> 9,
			usec, t->comm);
	} else {
		sprintf(tstring,"%s %Lu + %u [%s]\n", setup_header(pci, t, act),
			(unsigned long long)t->sector, t->bytes >> 9, t->comm);
	}

	output(pci, tstring);
}

static void log_merge(struct per_cpu_info *pci, struct blk_io_trace *t,
		      char act)
{
	log_track_merge(t);

	sprintf(tstring,"%s %Lu + %u [%s]\n", setup_header(pci, t, act),
		(unsigned long long)t->sector, t->bytes >> 9, t->comm);
	output(pci, tstring);
}

static void log_generic(struct per_cpu_info *pci, struct blk_io_trace *t,
			char act)
{
	sprintf(tstring,"%s %Lu + %u [%s]\n", setup_header(pci, t, act),
		(unsigned long long)t->sector, t->bytes >> 9, t->comm);
	output(pci, tstring);
}

static int log_pc(struct per_cpu_info *pci, struct blk_io_trace *t, char act)
{
	unsigned char *buf;
	int i;

	sprintf(tstring,"%s ", setup_header(pci, t, act));
	output(pci, tstring);

	buf = (unsigned char *) t + sizeof(*t);
	for (i = 0; i < t->pdu_len; i++) {
		sprintf(tstring,"%02x ", buf[i]);
		output(pci, tstring);
	}

	if (act == 'C') {
		sprintf(tstring,"[%d]\n", t->error);
		output(pci, tstring);
	} else {
		sprintf(tstring,"[%s]\n", t->comm);
		output(pci, tstring);
	}
	return 0;
}

static int dump_trace_pc(struct blk_io_trace *t, struct per_cpu_info *pci)
{
	int ret = 0;

	switch (t->action & 0xffff) {
		case __BLK_TA_QUEUE:
			log_generic(pci, t, 'Q');
			break;
		case __BLK_TA_GETRQ:
			log_generic(pci, t, 'G');
			break;
		case __BLK_TA_SLEEPRQ:
			log_generic(pci, t, 'S');
			break;
		case __BLK_TA_REQUEUE:
			log_generic(pci, t, 'R');
			break;
		case __BLK_TA_ISSUE:
			ret = log_pc(pci, t, 'D');
			break;
		case __BLK_TA_COMPLETE:
			log_pc(pci, t, 'C');
			break;
		default:
			fprintf(stderr, "Bad pc action %x\n", t->action);
			ret = 1;
			break;
	}
	
	return ret;
}

static void dump_trace_fs(struct blk_io_trace *t, struct per_cpu_info *pci)
{
	int w = t->action & BLK_TC_ACT(BLK_TC_WRITE);
	int act = t->action & 0xffff;

	switch (act) {
		case __BLK_TA_QUEUE:
			account_q(t, pci, w);
			log_queue(pci, t, 'Q');
			break;
		case __BLK_TA_BACKMERGE:
			account_m(t, pci, w);
			log_merge(pci, t, 'M');
			break;
		case __BLK_TA_FRONTMERGE:
			account_m(t, pci, w);
			log_merge(pci, t, 'F');
			break;
		case __BLK_TA_GETRQ:
			log_generic(pci, t, 'G');
			break;
		case __BLK_TA_SLEEPRQ:
			log_generic(pci, t, 'S');
			break;
		case __BLK_TA_REQUEUE:
			account_c(t, pci, w, -t->bytes);
			log_queue(pci, t, 'R');
			break;
		case __BLK_TA_ISSUE:
			account_i(t, pci, w);
			log_issue(pci, t, 'D');
			break;
		case __BLK_TA_COMPLETE:
			account_c(t, pci, w, t->bytes);
			log_complete(pci, t, 'C');
			break;
		default:
			fprintf(stderr, "Bad fs action %x\n", t->action);
			break;
	}
}

static int dump_trace(struct blk_io_trace *t, struct per_cpu_info *pci)
{
	int ret = 0;

	if (t->action & BLK_TC_ACT(BLK_TC_PC))
		ret = dump_trace_pc(t, pci);
	else
		dump_trace_fs(t, pci);

	events++;
	return ret;
}

static void dump_io_stats(struct io_stats *ios, char *msg)
{
	fprintf(ofp, "%s\n", msg);

	fprintf(ofp, " Reads Queued:    %'8lu, %'8LuKiB\t", ios->qreads, ios->qread_kb);
	fprintf(ofp, " Writes Queued:    %'8lu, %'8LuKiB\n", ios->qwrites,ios->qwrite_kb);

	fprintf(ofp, " Read Dispatches: %'8lu, %'8LuKiB\t", ios->ireads, ios->iread_kb);
	fprintf(ofp, " Write Dispatches: %'8lu, %'8LuKiB\n", ios->iwrites,ios->iwrite_kb);
	fprintf(ofp, " Reads Completed: %'8lu, %'8LuKiB\t", ios->creads, ios->cread_kb);
	fprintf(ofp, " Writes Completed: %'8lu, %'8LuKiB\n", ios->cwrites,ios->cwrite_kb);
	fprintf(ofp, " Read Merges:     %'8lu%8c\t", ios->mreads, ' ');

	fprintf(ofp, " Write Merges:     %'8lu\n", ios->mwrites);
}

static void show_process_stats(void)
{
	struct per_process_info *ppi;

	ppi = ppi_list;
	while (ppi) {
		dump_io_stats(&ppi->io_stats, ppi->name);
		ppi = ppi->list_next;
	}

	fprintf(ofp, "\n");
}

static void show_cpu_stats(void)
{
	struct per_cpu_info foo, *pci;
	struct io_stats *ios;
	int i, pci_events = 0;

	memset(&foo, 0, sizeof(foo));

	for (i = 0; i < max_cpus; i++) {
		char cpu[8];

		pci = &per_cpu_info[i];
		ios = &pci->io_stats;

		if (!pci->nelems)
			continue;

		foo.io_stats.qreads += ios->qreads;
		foo.io_stats.qwrites += ios->qwrites;
		foo.io_stats.creads += ios->creads;
		foo.io_stats.cwrites += ios->cwrites;
		foo.io_stats.mreads += ios->mreads;
		foo.io_stats.mwrites += ios->mwrites;
		foo.io_stats.ireads += ios->ireads;
		foo.io_stats.iwrites += ios->iwrites;
		foo.io_stats.qread_kb += ios->qread_kb;
		foo.io_stats.qwrite_kb += ios->qwrite_kb;
		foo.io_stats.cread_kb += ios->cread_kb;
		foo.io_stats.cwrite_kb += ios->cwrite_kb;
		foo.io_stats.iread_kb += ios->iread_kb;
		foo.io_stats.iwrite_kb += ios->iwrite_kb;

		snprintf(cpu, sizeof(cpu) - 1, "CPU%d:", i);
		dump_io_stats(ios, cpu);
		pci_events++;
	}

	if (pci_events > 1) {
		fprintf(ofp, "\n");
		dump_io_stats(&foo.io_stats, "Total:");
	}

	fprintf(ofp, "\nEvents: %'Lu\n", events);
}

#define min(a, b)	((a) < (b) ? (a) : (b))

static struct blk_io_trace *find_trace(void *p, unsigned long offset, int nr)
{
	unsigned long max_offset = min(offset,nr * sizeof(struct blk_io_trace));
	unsigned long off;
	struct blk_io_trace *bit;
	__u32 magic;

	for (off = 0; off < max_offset; off++) {
		bit = p + off;

		magic = be32_to_cpu(bit->magic);
		if ((magic & 0xffffff00) == BLK_IO_TRACE_MAGIC)
			return bit;
	}

	return NULL;
}

static int sort_entries(void *traces, unsigned long offset, int nr)
{
	struct per_cpu_info *pci;
	struct blk_io_trace *bit;
	struct trace *t;
	void *start = traces;
	int nelems = 0;

	while (traces - start <= offset - sizeof(*bit)) {
		if (!nr)
			break;

		bit = find_trace(traces, offset - (traces - start), nr);
		if (!bit)
			break;

		t = malloc(sizeof(*t));
		t->bit = bit;
		memset(&t->rb_node, 0, sizeof(t->rb_node));

		trace_to_cpu(bit);

		if (verify_trace(bit))
			break;

		pci = get_cpu_info(bit->cpu);
		pci->nelems++;

		if (trace_rb_insert(t))
			return -1;

		traces += sizeof(*bit) + bit->pdu_len;
		nelems++;
		nr--;
	}

	return nelems;
}

static void free_entries_rb(void)
{
	struct rb_node *n;

	while ((n = rb_first(&rb_sort_root)) != NULL) {
		struct trace *t = rb_entry(n, struct trace, rb_node);

		rb_erase(&t->rb_node, &rb_sort_root);
		free(t);
	}
}

static void show_entries_rb(void)
{
	struct blk_io_trace *bit;
	struct rb_node *n;
	struct trace *t;
	int cpu;

	n = rb_first(&rb_sort_root);
	if (!n)
		return;

	do {
		t = rb_entry(n, struct trace, rb_node);
		bit = t->bit;

		cpu = bit->cpu;
		if (cpu > max_cpus) {
			fprintf(stderr, "CPU number too large (%d)\n", cpu);
			break;
		}

		if (genesis_time == 0)
			genesis_time = bit->time;
		bit->time -= genesis_time;

		check_time(bit);

		if (dump_trace(bit, &per_cpu_info[cpu]))
			break;

	} while ((n = rb_next(n)) != NULL);
}

static int read_data(int fd, void *buffer, int bytes, int block)
{
	int ret, bytes_left, fl;
	void *p;

	fl = fcntl(fd, F_GETFL);

	if (!block)
		fcntl(fd, F_SETFL, fl | O_NONBLOCK);
	else
		fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);

	bytes_left = bytes;
	p = buffer;
	while (bytes_left > 0) {
		ret = read(fd, p, bytes_left);
		if (!ret)
			return 1;
		else if (ret < 0) {
			if (errno != EAGAIN)
				perror("read");
			return -1;
		} else {
			p += ret;
			bytes_left -= ret;
		}
	}

	return 0;
}

static int do_file(void)
{
	int i, nfiles;

	for (i = 0, nfiles = 0;; i++, nfiles++) {
		struct per_cpu_info *pci;
		struct stat st;
		void *tb;

		pci = get_cpu_info(i);

		snprintf(pci->fname, sizeof(pci->fname)-1,"%s_out.%d", dev, i);
		if (stat(pci->fname, &st) < 0)
			break;
		if (!st.st_size)
			continue;

		printf("Processing %s\n", pci->fname);

		tb = malloc(st.st_size);

		pci->fd = open(pci->fname, O_RDONLY);
		if (pci->fd < 0) {
			perror(pci->fname);
			break;
		}

		if (read_data(pci->fd, tb, st.st_size, 1))
			break;

		if (sort_entries(tb, st.st_size, ~0U) == -1)
			break;

		close(pci->fd);
		printf("\t%2d %10s %15d\n", i, pci->fname, pci->nelems);

	}

	if (!nfiles) {
		fprintf(stderr, "No files found\n");
		return 1;
	}

	show_entries_rb();
	return 0;
}

static void resize_buffer(void **buffer, long *size, long offset)
{
	long old_size = *size;

	*size *= 2;
	*buffer = realloc(*buffer, *size);
	memset(*buffer + offset, 0, *size - old_size);
}

static int read_sort_events(int fd, void **buffer)
{
	long offset, max_offset;
	int events;

	max_offset = 128 * sizeof(struct blk_io_trace);
	*buffer = malloc(max_offset);
	events = 0;
	offset = 0;

	do {
		struct blk_io_trace *t;
		int pdu_len;

		if (max_offset - offset < sizeof(*t))
			resize_buffer(buffer, &max_offset, offset);

		if (read_data(fd, *buffer + offset, sizeof(*t), !events)) {
			if (events)
				break;

			usleep(1000);
			continue;
		}

		t = *buffer + offset;
		offset += sizeof(*t);

		pdu_len = be16_to_cpu(t->pdu_len);
		if (pdu_len) {
			if (max_offset - offset < pdu_len)
				resize_buffer(buffer, &max_offset, offset);

			if (read_data(fd, *buffer + offset, pdu_len, 1))
				break;

			offset += pdu_len;
		}

		events++;
	} while (!is_done() && events < rb_batch);

	return events;
}

static int do_stdin(void)
{
	int fd;
	void *ptr;

	fd = dup(STDIN_FILENO);
	do {
		int events;

		events = read_sort_events(fd, &ptr);
		if (!events)
			break;
	
		if (sort_entries(ptr, ~0UL, events) == -1)
			break;

		show_entries_rb();
		free_entries_rb();
	} while (1);

	close(fd);
	free(ptr);
	return 0;
}

static void flush_output(void)
{
	fflush(ofp);
}

static void handle_sigint(int sig)
{
	done = 1;
	flush_output();
}

static void usage(char *prog)
{
	fprintf(stderr, "Usage: %s -i <name> [-o <output>][-s]\n", prog);
}

int main(int argc, char *argv[])
{
	char *ofp_buffer;
	int c, ret, mode;

	while ((c = getopt_long(argc, argv, S_OPTS, l_opts, NULL)) != -1) {
		switch (c) {
		case 'i':
			dev = optarg;
			break;
		case 'o':
			output_name = optarg;
			break;
		case 'b':
			rb_batch = atoi(optarg);
			if (rb_batch <= 0)
				rb_batch = RB_BATCH_DEFAULT;
			break;
		case 's':
			per_process_stats = 1;
			break;
		case 't':
			track_ios = 1;
			break;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (!dev) {
		usage(argv[0]);
		return 1;
	}

	memset(&rb_sort_root, 0, sizeof(rb_sort_root));
	memset(&rb_track_root, 0, sizeof(rb_track_root));

	signal(SIGINT, handle_sigint);
	signal(SIGHUP, handle_sigint);
	signal(SIGTERM, handle_sigint);

	setlocale(LC_NUMERIC, "en_US");

	if (!output_name) {
		ofp = fdopen(STDOUT_FILENO, "w");
		mode = _IOLBF;
	} else {
		char ofname[128];

		snprintf(ofname, sizeof(ofname) - 1, "%s.log", output_name);
		ofp = fopen(ofname, "w");
		mode = _IOFBF;
	}

	if (!ofp) {
		perror("fopen");
		return 1;
	}

	ofp_buffer = malloc(4096);	
	if (setvbuf(ofp, ofp_buffer, mode, 4096)) {
		perror("setvbuf");
		return 1;
	}

	if (!strcmp(dev, "-"))
		ret = do_stdin();
	else
		ret = do_file();

	if (per_process_stats)
		show_process_stats();

	show_cpu_stats();

	flush_output();
	return ret;
}
