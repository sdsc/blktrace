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
#include <limits.h>

#include "blktrace.h"
#include "rbtree.h"
#include "jhash.h"

static char blkparse_version[] = "0.90";

struct per_dev_info {
	dev_t id;
	char *name;

	int backwards;
	unsigned long long events;
	unsigned long long last_reported_time;
	unsigned long long last_read_time;
	struct io_stats io_stats;
	unsigned long last_sequence;
	unsigned long skips;

	int nfiles;
	int ncpus;
	struct per_cpu_info *cpus;
};

struct per_process_info {
	char name[16];
	__u32 pid;
	struct io_stats io_stats;
	struct per_process_info *hash_next, *list_next;
	int more_than_one;

	/*
	 * individual io stats
	 */
	unsigned long long longest_allocation_wait[2];
	unsigned long long longest_dispatch_wait[2];
	unsigned long long longest_completion_wait[2];
};

#define PPI_HASH_SHIFT	(8)
#define PPI_HASH_SIZE	(1 << PPI_HASH_SHIFT)
#define PPI_HASH_MASK	(PPI_HASH_SIZE - 1)
static struct per_process_info *ppi_hash_table[PPI_HASH_SIZE];
static struct per_process_info *ppi_list;
static int ppi_list_entries;

#define S_OPTS	"i:o:b:stqw:f:F:vn"
static struct option l_opts[] = {
	{
		.name = "input",
		.has_arg = required_argument,
		.flag = NULL,
		.val = 'i'
	},
	{
		.name = "output",
		.has_arg = required_argument,
		.flag = NULL,
		.val = 'o'
	},
	{
		.name = "batch",
		.has_arg = required_argument,
		.flag = NULL,
		.val = 'b'
	},
	{
		.name = "per program stats",
		.has_arg = no_argument,
		.flag = NULL,
		.val = 's'
	},
	{
		.name = "track ios",
		.has_arg = no_argument,
		.flag = NULL,
		.val = 't'
	},
	{
		.name = "quiet",
		.has_arg = no_argument,
		.flag = NULL,
		.val = 'q'
	},
	{
		.name = "stopwatch",
		.has_arg = required_argument,
		.flag = NULL,
		.val = 'w'
	},
	{
		.name = "format",
		.has_arg = required_argument,
		.flag = NULL,
		.val = 'f'
	},
	{
		.name = "format-spec",
		.has_arg = required_argument,
		.flag = NULL,
		.val = 'F'
	},
	{
		.name = "hash by name",
		.has_arg = no_argument,
		.flag = NULL,
		.val = 'n'
	},
	{
		.name = "version",
		.has_arg = no_argument,
		.flag = NULL,
		.val = 'v'
	},
};

/*
 * for sorting the displayed output
 */
struct trace {
	struct blk_io_trace *bit;
	struct rb_node rb_node;
	struct trace *next;
	int skipped;
};

static struct rb_root rb_sort_root;
static unsigned long rb_sort_entries;

static struct rb_root rb_track_root;

static struct trace *trace_list;

/*
 * allocation cache
 */
static struct blk_io_trace *bit_alloc_list;
static struct trace *t_alloc_list;

/*
 * for tracking individual ios
 */
struct io_track {
	struct rb_node rb_node;

	dev_t device;
	__u64 sector;
	__u32 pid;
	char comm[16];
	unsigned long long allocation_time;
	unsigned long long queue_time;
	unsigned long long dispatch_time;
	unsigned long long completion_time;
};

static int ndevices;
static struct per_dev_info *devices;
static char *get_dev_name(struct per_dev_info *, char *, int);

FILE *ofp = NULL;
static char *output_name;

static unsigned long long genesis_time;
static unsigned long long last_allowed_time;
static unsigned long long stopwatch_start;	/* start from zero by default */
static unsigned long long stopwatch_end = ULONG_LONG_MAX;	/* "infinity" */

static int per_process_stats;
static int track_ios;
static int ppi_hash_by_pid = 1;

#define RB_BATCH_DEFAULT	(512)
static int rb_batch = RB_BATCH_DEFAULT;

static int pipeline;

#define is_done()	(*(volatile int *)(&done))
static volatile int done;

#define JHASH_RANDOM	(0x3af5f2ee)

static inline int ppi_hash_pid(__u32 pid)
{
	return jhash_1word(pid, JHASH_RANDOM) & PPI_HASH_MASK;
}

static inline int ppi_hash_name(const char *name)
{
	return jhash(name, 16, JHASH_RANDOM) & PPI_HASH_MASK;
}

static inline int ppi_hash(struct per_process_info *ppi)
{
	if (ppi_hash_by_pid)
		return ppi_hash_pid(ppi->pid);

	return ppi_hash_name(ppi->name);
}

static inline void add_process_to_hash(struct per_process_info *ppi)
{
	const int hash_idx = ppi_hash(ppi);

	ppi->hash_next = ppi_hash_table[hash_idx];
	ppi_hash_table[hash_idx] = ppi;
}

static inline void add_process_to_list(struct per_process_info *ppi)
{
	ppi->list_next = ppi_list;
	ppi_list = ppi;
	ppi_list_entries++;
}

static struct per_process_info *find_process_by_name(char *name)
{
	const int hash_idx = ppi_hash_name(name);
	struct per_process_info *ppi;

	ppi = ppi_hash_table[hash_idx];
	while (ppi) {
		if (!strcmp(ppi->name, name))
			return ppi;

		ppi = ppi->hash_next;
	}

	return NULL;
}

static struct per_process_info *find_process_by_pid(__u32 pid)
{
	const int hash_idx = ppi_hash_pid(pid);
	struct per_process_info *ppi;

	ppi = ppi_hash_table[hash_idx];
	while (ppi) {
		if (ppi->pid == pid)
			return ppi;

		ppi = ppi->hash_next;
	}

	return NULL;
}

static struct per_process_info *find_process(__u32 pid, char *name)
{
	struct per_process_info *ppi;

	if (ppi_hash_by_pid)
		return find_process_by_pid(pid);

	ppi = find_process_by_name(name);
	if (ppi && ppi->pid != pid)
		ppi->more_than_one = 1;

	return ppi;
}

static inline int trace_rb_insert(struct trace *t)
{
	struct rb_node **p = &rb_sort_root.rb_node;
	struct rb_node *parent = NULL;
	struct trace *__t;

	while (*p) {
		parent = *p;
		__t = rb_entry(parent, struct trace, rb_node);

		if (t->bit->time < __t->bit->time)
			p = &(*p)->rb_left;
		else if (t->bit->time > __t->bit->time)
			p = &(*p)->rb_right;
		else if (t->bit->device < __t->bit->device)
			p = &(*p)->rb_left;
		else if (t->bit->device > __t->bit->device)
			p = &(*p)->rb_right;
		else if (t->bit->sequence < __t->bit->sequence)
			p = &(*p)->rb_left;
		else if (t->bit->sequence > __t->bit->sequence)
			p = &(*p)->rb_right;
		else if (t->bit->device == __t->bit->device) {
			fprintf(stderr,
				"sequence alias (%d) on device %d,%d!\n",
				t->bit->sequence,
				MAJOR(t->bit->device), MINOR(t->bit->device));
			return 1;
		}
	}

	rb_sort_entries++;
	rb_link_node(&t->rb_node, parent, p);
	rb_insert_color(&t->rb_node, &rb_sort_root);
	return 0;
}

static struct trace *trace_rb_find(dev_t device, unsigned long sequence)
{
	struct rb_node **p = &rb_sort_root.rb_node;
	struct rb_node *parent = NULL;
	struct trace *__t;

	while (*p) {
		parent = *p;
		__t = rb_entry(parent, struct trace, rb_node);

		if (device < __t->bit->device)
			p = &(*p)->rb_left;
		else if (device > __t->bit->device)
			p = &(*p)->rb_right;
		else if (sequence < __t->bit->sequence)
			p = &(*p)->rb_left;
		else if (sequence > __t->bit->sequence)
			p = &(*p)->rb_right;
		else
			return __t;
	}

	return NULL;
}

static inline int track_rb_insert(struct io_track *iot)
{
	struct rb_node **p = &rb_track_root.rb_node;
	struct rb_node *parent = NULL;
	struct io_track *__iot;

	while (*p) {
		parent = *p;

		__iot = rb_entry(parent, struct io_track, rb_node);

		if (iot->device < __iot->device)
			p = &(*p)->rb_left;
		else if (iot->device > __iot->device)
			p = &(*p)->rb_right;
		else if (iot->sector < __iot->sector)
			p = &(*p)->rb_left;
		else if (iot->sector > __iot->sector)
			p = &(*p)->rb_right;
		else {
			fprintf(stderr,
				"sector alias (%Lu) on device %d,%d!\n",
				(unsigned long long) iot->sector,
				MAJOR(iot->device), MINOR(iot->device));
			return 1;
		}
	}

	rb_link_node(&iot->rb_node, parent, p);
	rb_insert_color(&iot->rb_node, &rb_track_root);
	return 0;
}

static struct io_track *__find_track(dev_t device, __u64 sector)
{
	struct rb_node **p = &rb_track_root.rb_node;
	struct rb_node *parent = NULL;
	struct io_track *__iot;

	while (*p) {
		parent = *p;
		
		__iot = rb_entry(parent, struct io_track, rb_node);

		if (device < __iot->device)
			p = &(*p)->rb_left;
		else if (device > __iot->device)
			p = &(*p)->rb_right;
		else if (sector < __iot->sector)
			p = &(*p)->rb_left;
		else if (sector > __iot->sector)
			p = &(*p)->rb_right;
		else
			return __iot;
	}

	return NULL;
}

static struct io_track *find_track(__u32 pid, char *comm, dev_t device,
				   __u64 sector)
{
	struct io_track *iot;

	iot = __find_track(device, sector);
	if (!iot) {
		iot = malloc(sizeof(*iot));
		iot->pid = pid;
		memcpy(iot->comm, comm, sizeof(iot->comm));
		iot->device = device;
		iot->sector = sector;
		track_rb_insert(iot);
	}

	return iot;
}

static void log_track_frontmerge(struct blk_io_trace *t)
{
	struct io_track *iot;

	if (!track_ios)
		return;

	iot = __find_track(t->device, t->sector + (t->bytes >> 9));
	if (!iot) {
		fprintf(stderr, "failed to find mergeable event\n");
		return;
	}

	rb_erase(&iot->rb_node, &rb_track_root);
	iot->sector -= t->bytes >> 9;
	track_rb_insert(iot);
}

static void log_track_getrq(struct blk_io_trace *t)
{
	struct io_track *iot;

	if (!track_ios)
		return;

	iot = find_track(t->pid, t->comm, t->device, t->sector);
	iot->allocation_time = t->time;
}


/*
 * return time between rq allocation and insertion
 */
static unsigned long long log_track_insert(struct blk_io_trace *t)
{
	unsigned long long elapsed;
	struct io_track *iot;

	if (!track_ios)
		return -1;

	iot = find_track(t->pid, t->comm, t->device, t->sector);
	iot->queue_time = t->time;
	elapsed = iot->queue_time - iot->allocation_time;

	if (per_process_stats) {
		struct per_process_info *ppi = find_process(iot->pid,iot->comm);
		int w = (t->action & BLK_TC_ACT(BLK_TC_WRITE)) != 0;

		if (ppi && elapsed > ppi->longest_allocation_wait[w])
			ppi->longest_allocation_wait[w] = elapsed;
	}

	return elapsed;
}

/*
 * return time between queue and issue
 */
static unsigned long long log_track_issue(struct blk_io_trace *t)
{
	unsigned long long elapsed;
	struct io_track *iot;

	if (!track_ios)
		return -1;
	if ((t->action & BLK_TC_ACT(BLK_TC_FS)) == 0)
		return -1;

	iot = __find_track(t->device, t->sector);
	if (!iot) {
		fprintf(stderr, "failed to find issue event\n");
		return -1;
	}

	iot->dispatch_time = t->time;
	elapsed = iot->dispatch_time - iot->queue_time;

	if (per_process_stats) {
		struct per_process_info *ppi = find_process(iot->pid,iot->comm);
		int w = (t->action & BLK_TC_ACT(BLK_TC_WRITE)) != 0;

		if (ppi && elapsed > ppi->longest_dispatch_wait[w])
			ppi->longest_dispatch_wait[w] = elapsed;
	}

	return elapsed;
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

	iot = __find_track(t->device, t->sector);
	if (!iot) {
		fprintf(stderr, "failed to find complete event\n");
		return -1;
	}

	iot->completion_time = t->time;
	elapsed = iot->completion_time - iot->dispatch_time;

	if (per_process_stats) {
		struct per_process_info *ppi = find_process(iot->pid,iot->comm);
		int w = (t->action & BLK_TC_ACT(BLK_TC_WRITE)) != 0;

		if (ppi && elapsed > ppi->longest_completion_wait[w])
			ppi->longest_completion_wait[w] = elapsed;
	}

	/*
	 * kill the trace, we don't need it after completion
	 */
	rb_erase(&iot->rb_node, &rb_track_root);
	free(iot);

	return elapsed;
}


static struct io_stats *find_process_io_stats(__u32 pid, char *name)
{
	struct per_process_info *ppi = find_process(pid, name);

	if (!ppi) {
		ppi = malloc(sizeof(*ppi));
		memset(ppi, 0, sizeof(*ppi));
		memcpy(ppi->name, name, 16);
		ppi->pid = pid;
		add_process_to_hash(ppi);
		add_process_to_list(ppi);
	}

	return &ppi->io_stats;
}

static void resize_cpu_info(struct per_dev_info *pdi, int cpu)
{
	struct per_cpu_info *cpus = pdi->cpus;
	int ncpus = pdi->ncpus;
	int new_count = cpu + 1;
	int new_space, size;
	char *new_start;

	size = new_count * sizeof(struct per_cpu_info);
	cpus = realloc(cpus, size);
	if (!cpus) {
		char name[20];
		fprintf(stderr, "Out of memory, CPU info for device %s (%d)\n",
			get_dev_name(pdi, name, sizeof(name)), size);
		exit(1);
	}

	new_start = (char *)cpus + (ncpus * sizeof(struct per_cpu_info));
	new_space = (new_count - ncpus) * sizeof(struct per_cpu_info);
	memset(new_start, 0, new_space);

	pdi->ncpus = new_count;
	pdi->cpus = cpus;
}

static struct per_cpu_info *get_cpu_info(struct per_dev_info *pdi, int cpu)
{
	struct per_cpu_info *pci;

	if (cpu >= pdi->ncpus)
		resize_cpu_info(pdi, cpu);

	pci = &pdi->cpus[cpu];
	pci->cpu = cpu;
	return pci;
}


static int resize_devices(char *name)
{
	int size = (ndevices + 1) * sizeof(struct per_dev_info);

	devices = realloc(devices, size);
	if (!devices) {
		fprintf(stderr, "Out of memory, device %s (%d)\n", name, size);
		return 1;
	}
	memset(&devices[ndevices], 0, sizeof(struct per_dev_info));
	devices[ndevices].name = name;
	ndevices++;
	return 0;
}

static struct per_dev_info *get_dev_info(dev_t id)
{
	struct per_dev_info *pdi;
	int i;

	for (i = 0; i < ndevices; i++) {
		if (!devices[i].id)
			devices[i].id = id;
		if (devices[i].id == id)
			return &devices[i];
	}

	if (resize_devices(NULL) != 0)
		return NULL;

	pdi = &devices[ndevices - 1];
	pdi->id = id;
	pdi->last_sequence = 0;
	pdi->last_read_time = 0;
	return pdi;
}

static char *get_dev_name(struct per_dev_info *pdi, char *buffer, int size)
{
	if (pdi->name)
		snprintf(buffer, size, "%s", pdi->name);
	else
		snprintf(buffer, size, "%d,%d", MAJOR(pdi->id), MINOR(pdi->id));
	return buffer;
}

static void check_time(struct per_dev_info *pdi, struct blk_io_trace *bit)
{
	unsigned long long this = bit->time;
	unsigned long long last = pdi->last_reported_time;

	pdi->backwards = (this < last) ? 'B' : ' ';
	pdi->last_reported_time = this;
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

static inline void __account_queue(struct io_stats *ios, struct blk_io_trace *t,
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

static inline void account_queue(struct blk_io_trace *t,
				 struct per_cpu_info *pci, int rw)
{
	__account_queue(&pci->io_stats, t, rw);

	if (per_process_stats) {
		struct io_stats *ios = find_process_io_stats(t->pid, t->comm);

		__account_queue(ios, t, rw);
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

static inline void __account_issue(struct io_stats *ios, int rw,
				   unsigned int bytes)
{
	if (rw) {
		ios->iwrites++;
		ios->iwrite_kb += bytes >> 10;
	} else {
		ios->ireads++;
		ios->iread_kb += bytes >> 10;
	}
}

static inline void account_issue(struct blk_io_trace *t,
				 struct per_cpu_info *pci, int rw)
{
	__account_issue(&pci->io_stats, rw, t->bytes);

	if (per_process_stats) {
		struct io_stats *ios = find_process_io_stats(t->pid, t->comm);

		__account_issue(ios, rw, t->bytes);
	}
}

static inline void __account_unplug(struct io_stats *ios, int timer)
{
	if (timer)
		ios->timer_unplugs++;
	else
		ios->io_unplugs++;
}

static inline void account_unplug(struct blk_io_trace *t,
				  struct per_cpu_info *pci, int timer)
{
	__account_unplug(&pci->io_stats, timer);

	if (per_process_stats) {
		struct io_stats *ios = find_process_io_stats(t->pid, t->comm);

		__account_unplug(ios, timer);
	}
}

static void log_complete(struct per_cpu_info *pci, struct blk_io_trace *t,
			 char *act)
{
	process_fmt(act, pci, t, log_track_complete(t), 0, NULL);
}

static void log_insert(struct per_cpu_info *pci, struct blk_io_trace *t,
		      char *act)
{
	process_fmt(act, pci, t, log_track_insert(t), 0, NULL);
}

static void log_queue(struct per_cpu_info *pci, struct blk_io_trace *t,
		      char *act)
{
	process_fmt(act, pci, t, -1, 0, NULL);
}

static void log_issue(struct per_cpu_info *pci, struct blk_io_trace *t,
		      char *act)
{
	process_fmt(act, pci, t, log_track_issue(t), 0, NULL);
}

static void log_merge(struct per_cpu_info *pci, struct blk_io_trace *t,
		      char *act)
{
	if (act[0] == 'F')
		log_track_frontmerge(t);

	process_fmt(act, pci, t, -1ULL, 0, NULL);
}

static void log_action(struct per_cpu_info *pci, struct blk_io_trace *t,
			char *act)
{
	process_fmt(act, pci, t, -1ULL, 0, NULL);
}

static void log_generic(struct per_cpu_info *pci, struct blk_io_trace *t,
			char *act)
{
	process_fmt(act, pci, t, -1ULL, 0, NULL);
}

static void log_unplug(struct per_cpu_info *pci, struct blk_io_trace *t,
		      char *act)
{
	process_fmt(act, pci, t, -1ULL, 0, NULL);
}

static void log_split(struct per_cpu_info *pci, struct blk_io_trace *t,
		      char *act)
{
	process_fmt(act, pci, t, -1ULL, 0, NULL);
}

static void log_pc(struct per_cpu_info *pci, struct blk_io_trace *t, char *act)
{
	unsigned char *buf = (unsigned char *) t + sizeof(*t);

	process_fmt(act, pci, t, -1ULL, t->pdu_len, buf);
}

static void dump_trace_pc(struct blk_io_trace *t, struct per_cpu_info *pci)
{
	int act = t->action & 0xffff;

	switch (act) {
		case __BLK_TA_QUEUE:
			log_generic(pci, t, "Q");
			break;
		case __BLK_TA_GETRQ:
			log_generic(pci, t, "G");
			break;
		case __BLK_TA_SLEEPRQ:
			log_generic(pci, t, "S");
			break;
		case __BLK_TA_REQUEUE:
			log_generic(pci, t, "R");
			break;
		case __BLK_TA_ISSUE:
			log_pc(pci, t, "D");
			break;
		case __BLK_TA_COMPLETE:
			log_pc(pci, t, "C");
			break;
		case __BLK_TA_INSERT:
			log_pc(pci, t, "I");
			break;
		default:
			fprintf(stderr, "Bad pc action %x\n", act);
			break;
	}
}

static void dump_trace_fs(struct blk_io_trace *t, struct per_cpu_info *pci)
{
	int w = t->action & BLK_TC_ACT(BLK_TC_WRITE);
	int act = t->action & 0xffff;

	switch (act) {
		case __BLK_TA_QUEUE:
			account_queue(t, pci, w);
			log_queue(pci, t, "Q");
			break;
		case __BLK_TA_INSERT:
			log_insert(pci, t, "I");
			break;
		case __BLK_TA_BACKMERGE:
			account_m(t, pci, w);
			log_merge(pci, t, "M");
			break;
		case __BLK_TA_FRONTMERGE:
			account_m(t, pci, w);
			log_merge(pci, t, "F");
			break;
		case __BLK_TA_GETRQ:
			log_track_getrq(t);
			log_generic(pci, t, "G");
			break;
		case __BLK_TA_SLEEPRQ:
			log_generic(pci, t, "S");
			break;
		case __BLK_TA_REQUEUE:
			account_c(t, pci, w, -t->bytes);
			log_queue(pci, t, "R");
			break;
		case __BLK_TA_ISSUE:
			account_issue(t, pci, w);
			log_issue(pci, t, "D");
			break;
		case __BLK_TA_COMPLETE:
			account_c(t, pci, w, t->bytes);
			log_complete(pci, t, "C");
			break;
		case __BLK_TA_PLUG:
			log_action(pci, t, "P");
			break;
		case __BLK_TA_UNPLUG_IO:
			account_unplug(t, pci, 0);
			log_unplug(pci, t, "U");
			break;
		case __BLK_TA_UNPLUG_TIMER:
			account_unplug(t, pci, 1);
			log_unplug(pci, t, "UT");
			break;
		case __BLK_TA_SPLIT:
			log_split(pci, t, "X");
			break;
		case __BLK_TA_BOUNCE:
			log_generic(pci, t, "B");
			break;
		default:
			fprintf(stderr, "Bad fs action %x\n", t->action);
			break;
	}
}

static void dump_trace(struct blk_io_trace *t, struct per_cpu_info *pci,
		       struct per_dev_info *pdi)
{
	if (t->action & BLK_TC_ACT(BLK_TC_PC))
		dump_trace_pc(t, pci);
	else
		dump_trace_fs(t, pci);

	pdi->events++;
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
	fprintf(ofp, " IO unplugs:      %'8lu%8c\t", ios->io_unplugs, ' ');
	fprintf(ofp, " Timer unplugs:    %'8lu\n", ios->timer_unplugs);
}

static void dump_wait_stats(struct per_process_info *ppi)
{
	unsigned long rawait = ppi->longest_allocation_wait[0] / 1000;
	unsigned long rdwait = ppi->longest_dispatch_wait[0] / 1000;
	unsigned long rcwait = ppi->longest_completion_wait[0] / 1000;
	unsigned long wawait = ppi->longest_allocation_wait[1] / 1000;
	unsigned long wdwait = ppi->longest_dispatch_wait[1] / 1000;
	unsigned long wcwait = ppi->longest_completion_wait[1] / 1000;

	fprintf(ofp, " Allocation wait: %'8lu%8c\t", rawait, ' ');
	fprintf(ofp, " Allocation wait:  %'8lu\n", wawait);
	fprintf(ofp, " Dispatch wait:   %'8lu%8c\t", rdwait, ' ');
	fprintf(ofp, " Dispatch wait:    %'8lu\n", wdwait);
	fprintf(ofp, " Completion wait: %'8lu%8c\t", rcwait, ' ');
	fprintf(ofp, " Completion wait:  %'8lu\n", wcwait);
}

static int ppi_name_compare(const void *p1, const void *p2)
{
	struct per_process_info *ppi1 = *((struct per_process_info **) p1);
	struct per_process_info *ppi2 = *((struct per_process_info **) p2);
	int res;

	res = strverscmp(ppi1->name, ppi2->name);
	if (!res)
		res = ppi1->pid > ppi2->pid;

	return res;
}

static void sort_process_list(void)
{
	struct per_process_info **ppis;
	struct per_process_info *ppi;
	int i = 0;

	ppis = malloc(ppi_list_entries * sizeof(struct per_process_info *));

	ppi = ppi_list;
	while (ppi) {
		ppis[i++] = ppi;
		ppi = ppi->list_next;
	}

	qsort(ppis, ppi_list_entries, sizeof(ppi), ppi_name_compare);

	i = ppi_list_entries - 1;
	ppi_list = NULL;
	while (i >= 0) {
		ppi = ppis[i];

		ppi->list_next = ppi_list;
		ppi_list = ppi;
		i--;
	}

	free(ppis);
}

static void show_process_stats(void)
{
	struct per_process_info *ppi;

	sort_process_list();

	ppi = ppi_list;
	while (ppi) {
		char name[64];

		if (ppi->more_than_one)
			sprintf(name, "%s (%u, ...)", ppi->name, ppi->pid);
		else
			sprintf(name, "%s (%u)", ppi->name, ppi->pid);

		dump_io_stats(&ppi->io_stats, name);
		dump_wait_stats(ppi);
		ppi = ppi->list_next;
	}

	fprintf(ofp, "\n");
}

static void show_device_and_cpu_stats(void)
{
	struct per_dev_info *pdi;
	struct per_cpu_info *pci;
	struct io_stats total, *ios;
	int i, j, pci_events;
	char line[3 + 8/*cpu*/ + 2 + 32/*dev*/ + 3];
	char name[32];

	for (pdi = devices, i = 0; i < ndevices; i++, pdi++) {

		memset(&total, 0, sizeof(total));
		pci_events = 0;

		if (i > 0)
			fprintf(ofp, "\n");

		for (pci = pdi->cpus, j = 0; j < pdi->ncpus; j++, pci++) {
			if (!pci->nelems)
				continue;

			ios = &pci->io_stats;
			total.qreads += ios->qreads;
			total.qwrites += ios->qwrites;
			total.creads += ios->creads;
			total.cwrites += ios->cwrites;
			total.mreads += ios->mreads;
			total.mwrites += ios->mwrites;
			total.ireads += ios->ireads;
			total.iwrites += ios->iwrites;
			total.qread_kb += ios->qread_kb;
			total.qwrite_kb += ios->qwrite_kb;
			total.cread_kb += ios->cread_kb;
			total.cwrite_kb += ios->cwrite_kb;
			total.iread_kb += ios->iread_kb;
			total.iwrite_kb += ios->iwrite_kb;
			total.timer_unplugs += ios->timer_unplugs;
			total.io_unplugs += ios->io_unplugs;

			snprintf(line, sizeof(line) - 1, "CPU%d (%s):",
				 j, get_dev_name(pdi, name, sizeof(name)));
			dump_io_stats(ios, line);
			pci_events++;
		}

		if (pci_events > 1) {
			fprintf(ofp, "\n");
			snprintf(line, sizeof(line) - 1, "Total (%s):",
				 get_dev_name(pdi, name, sizeof(name)));
			dump_io_stats(&total, line);
		}

		fprintf(ofp, "\nEvents (%s): %'Lu entries, %'lu skips\n",
			get_dev_name(pdi, line, sizeof(line)), pdi->events,
			pdi->skips);
	}
}

/*
 * struct trace and blktrace allocation cache, we do potentially
 * millions of mallocs for these structures while only using at most
 * a few thousand at the time
 */
static inline void t_free(struct trace *t)
{
	t->next = t_alloc_list;
	t_alloc_list = t;
}

static inline struct trace *t_alloc(void)
{
	struct trace *t = t_alloc_list;

	if (t) {
		t_alloc_list = t->next;
		return t;
	}

	return malloc(sizeof(*t));
}

static inline void bit_free(struct blk_io_trace *bit)
{
	/*
	 * abuse a 64-bit field for a next pointer for the free item
	 */
	bit->time = (__u64) (unsigned long) bit_alloc_list;
	bit_alloc_list = (struct blk_io_trace *) bit;
}

static inline struct blk_io_trace *bit_alloc(void)
{
	struct blk_io_trace *bit = bit_alloc_list;

	if (bit) {
		bit_alloc_list = (struct blk_io_trace *) (unsigned long) \
				 bit->time;
		return bit;
	}

	return malloc(sizeof(*bit));
}

static void find_genesis(void)
{
	struct trace *t = trace_list;

	genesis_time = -1ULL;
	while (t != NULL) {
		if (t->bit->time < genesis_time)
			genesis_time = t->bit->time;

		t = t->next;
	}
}

static inline int check_stopwatch(struct blk_io_trace *bit)
{
	if (bit->time < stopwatch_end &&
	    bit->time >= stopwatch_start)
		return 0;

	return 1;
}

/*
 * return youngest entry read
 */
static int sort_entries(unsigned long long *youngest)
{
	struct trace *t;

	if (!genesis_time)
		find_genesis();

	*youngest = -1ULL;
	while ((t = trace_list) != NULL) {
		struct blk_io_trace *bit = t->bit;

		trace_list = t->next;

		if (verify_trace(bit))
			continue;

		bit->time -= genesis_time;

		if (check_stopwatch(bit)) {
			bit_free(bit);
			t_free(t);
			continue;
		}

		if (trace_rb_insert(t))
			return -1;

		if (bit->time < *youngest)
			*youngest = bit->time;
	}

	return 0;
}

static void show_entries_rb(int force)
{
	struct per_dev_info *pdi = NULL;
	struct per_cpu_info *pci = NULL;
	struct blk_io_trace *bit;
	struct rb_node *n;
	struct trace *t;

	while ((n = rb_first(&rb_sort_root)) != NULL) {

		if (done)
			break;

		t = rb_entry(n, struct trace, rb_node);
		bit = t->bit;

		if (!pdi || pdi->id != bit->device)
			pdi = get_dev_info(bit->device);

		if (!pdi) {
			fprintf(stderr, "Unknown device ID? (%d,%d)\n",
				MAJOR(bit->device), MINOR(bit->device));
			break;
		}

		if (bit->cpu > pdi->ncpus) {
			fprintf(stderr, "Unknown CPU ID? (%d, device %d,%d)\n",
				bit->cpu, MAJOR(bit->device),
				MINOR(bit->device));
			break;
		}

		/*
		 * back off displaying more info if we are out of sync
		 * on SMP systems. to prevent stalling on lost events,
		 * only allow an event to skip us a few times
		 */
		if (bit->sequence > (pdi->last_sequence + 1) && !force) {
			struct trace *__t;

			/*
			 * the wanted sequence is really there, continue
			 * because this means that the log time is earlier
			 * on the trace we have now
			 */
			__t = trace_rb_find(pdi->id, pdi->last_sequence + 1);
			if (__t)
				goto ok;

			if (t->skipped < 5) {
				t->skipped++;
				break;
			} else
				pdi->skips++;
		}

ok:
		if (!force && bit->time > last_allowed_time)
			break;

		pdi->last_sequence = bit->sequence;

		check_time(pdi, bit);

		if (!pci || pci->cpu != bit->cpu)
			pci = get_cpu_info(pdi, bit->cpu);

		dump_trace(bit, pci, pdi);

		rb_erase(&t->rb_node, &rb_sort_root);
		rb_sort_entries--;
		bit_free(bit);
		t_free(t);
	}
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

static int read_events(int fd, int always_block)
{
	struct per_dev_info *pdi = NULL;
	int events = 0;

	while (!is_done() && events < rb_batch) {
		struct blk_io_trace *bit;
		struct trace *t;
		int pdu_len;
		__u32 magic;

		bit = bit_alloc();

		if (read_data(fd, bit, sizeof(*bit), !events || always_block))
			break;

		magic = be32_to_cpu(bit->magic);
		if ((magic & 0xffffff00) != BLK_IO_TRACE_MAGIC) {
			fprintf(stderr, "Bad magic %x\n", magic);
			break;
		}

		pdu_len = be16_to_cpu(bit->pdu_len);
		if (pdu_len) {
			void *ptr = realloc(bit, sizeof(*bit) + pdu_len);

			if (read_data(fd, ptr + sizeof(*bit), pdu_len, 1))
				break;

			bit = ptr;
		}

		t = t_alloc();
		memset(t, 0, sizeof(*t));
		t->bit = bit;

		trace_to_cpu(bit);

		t->next = trace_list;
		trace_list = t;

		if (!pdi || pdi->id != bit->device)
			pdi = get_dev_info(bit->device);

		if (bit->time > pdi->last_read_time)
			pdi->last_read_time = bit->time;

		events++;
	}

	return events;
}

static int do_file(void)
{
	struct per_cpu_info *pci;
	struct per_dev_info *pdi;
	int i, j, events, events_added;

	/*
	 * first prepare all files for reading
	 */
	for (i = 0; i < ndevices; i++) {
		pdi = &devices[i];
		pdi->nfiles = 0;
		pdi->last_sequence = 0;

		for (j = 0;; j++) {
			struct stat st;

			pci = get_cpu_info(pdi, j);
			pci->cpu = j;
			pci->fd = -1;

			snprintf(pci->fname, sizeof(pci->fname)-1,
				 "%s.blktrace.%d", pdi->name, pci->cpu);
			if (stat(pci->fname, &st) < 0)
				break;
			if (st.st_size) {
				pci->fd = open(pci->fname, O_RDONLY);
				if (pci->fd < 0) {
					perror(pci->fname);
					continue;
				}
			}

			printf("Input file %s added\n", pci->fname);
			pdi->nfiles++;
		}
	}

	/*
	 * now loop over the files reading in the data
	 */
	do {
		unsigned long long youngest;

		events_added = 0;
		last_allowed_time = -1ULL;

		for (i = 0; i < ndevices; i++) {
			pdi = &devices[i];

			for (j = 0; j < pdi->nfiles; j++) {

				pci = get_cpu_info(pdi, j);

				if (pci->fd == -1)
					continue;

				events = read_events(pci->fd, 1);
				if (!events) {
					close(pci->fd);
					pci->fd = -1;
					continue;
				}

				if (pdi->last_read_time < last_allowed_time)
					last_allowed_time = pdi->last_read_time;

				events_added += events;
			}
		}

		if (sort_entries(&youngest))
			break;

		if (youngest > stopwatch_end)
			break;

		show_entries_rb(0);

	} while (events_added);

	if (rb_sort_entries)
		show_entries_rb(1);

	return 0;
}

static int do_stdin(void)
{
	unsigned long long youngest;
	int fd;

	last_allowed_time = -1ULL;
	fd = dup(STDIN_FILENO);
	do {
		int events;

		events = read_events(fd, 0);
		if (!events)
			break;
	
		if (sort_entries(&youngest))
			break;

		if (youngest > stopwatch_end)
			break;

		show_entries_rb(0);
	} while (1);

	if (rb_sort_entries)
		show_entries_rb(1);

	close(fd);
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

/*
 * Extract start and duration times from a string, allowing
 * us to specify a time interval of interest within a trace.
 * Format: "duration" (start is zero) or "start:duration".
 */
static int find_stopwatch_interval(char *string)
{
	double value;
	char *sp;

	value = strtod(string, &sp);
	if (sp == string) {
		fprintf(stderr,"Invalid stopwatch timer: %s\n", string);
		return 1;
	}
	if (*sp == ':') {
		stopwatch_start = DOUBLE_TO_NANO_ULL(value);
		string = sp + 1;
		value = strtod(string, &sp);
		if (sp == string || *sp != '\0') {
			fprintf(stderr,"Invalid stopwatch duration time: %s\n",
				string);
			return 1;
		}
	} else if (*sp != '\0') {
		fprintf(stderr,"Invalid stopwatch start timer: %s\n", string);
		return 1;
	}
	stopwatch_end = DOUBLE_TO_NANO_ULL(value);
	if (stopwatch_end <= stopwatch_start) {
		fprintf(stderr, "Invalid stopwatch interval: %Lu -> %Lu\n",
			stopwatch_start, stopwatch_end);
		return 1;
	}

	return 0;
}

static char usage_str[] = \
	"[ -i <input name> ] [-o <output name> [ -s ] [ -t ] [ -q ]\n" \
	"[ -w start:stop ] [ -f output format ] [ -F format spec ] [ -v] \n\n" \
	"\t-i Input file containing trace data, or '-' for stdin\n" \
	"\t-o Output file. If not given, output is stdout\n" \
	"\t-b stdin read batching\n" \
	"\t-s Show per-program io statistics\n" \
	"\t-n Hash processes by name, not pid\n" \
	"\t-t Track individual ios. Will tell you the time a request took\n" \
	"\t   to get queued, to get dispatched, and to get completed\n" \
	"\t-q Quiet. Don't display any stats at the end of the trace\n" \
	"\t-w Only parse data between the given time interval in seconds.\n" \
	"\t   If 'start' isn't given, blkparse defaults the start time to 0\n" \
	"\t -f Output format. Customize the output format. The format field\n" \
	"\t    identifies can be found in the documentation\n" \
	"\t-F Format specification. Can be found in the documentation\n" \
	"\t-v Print program version info\n\n";

static void usage(char *prog)
{
	fprintf(stderr, "Usage: %s %s %s", prog, blkparse_version, usage_str);
}

int main(int argc, char *argv[])
{
	char *ofp_buffer;
	int c, ret, mode;
	int per_device_and_cpu_stats = 1;

	while ((c = getopt_long(argc, argv, S_OPTS, l_opts, NULL)) != -1) {
		switch (c) {
		case 'i':
			if (!strcmp(optarg, "-") && !pipeline)
				pipeline = 1;
			else if (resize_devices(optarg) != 0)
				return 1;
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
		case 'q':
			per_device_and_cpu_stats = 0;
			break;
		case 'w':
			if (find_stopwatch_interval(optarg) != 0)
				return 1;
			break;
		case 'f':
			set_all_format_specs(optarg);
			break;
		case 'F':
			if (add_format_spec(optarg) != 0)
				return 1;
			break;
		case 'n':
			ppi_hash_by_pid = 0;
			break;
		case 'v':
			printf("%s version %s\n", argv[0], blkparse_version);
			return 0;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	while (optind < argc) {
		if (!strcmp(argv[optind], "-") && !pipeline)
			pipeline = 1;
		else if (resize_devices(argv[optind]) != 0)
			return 1;
		optind++;
	}

	if (!pipeline && !ndevices) {
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

		snprintf(ofname, sizeof(ofname) - 1, "%s", output_name);
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

	if (pipeline)
		ret = do_stdin();
	else
		ret = do_file();

	if (per_process_stats)
		show_process_stats();

	if (per_device_and_cpu_stats)
		show_device_and_cpu_stats();

	flush_output();
	return ret;
}
