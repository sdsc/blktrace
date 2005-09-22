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
#include <ctype.h>

#include "blktrace.h"
#include "rbtree.h"

static char blkparse_version[] = "0.90";

#define SECONDS(x) 		((unsigned long long)(x) / 1000000000)
#define NANO_SECONDS(x)		((unsigned long long)(x) % 1000000000)
#define DOUBLE_TO_NANO_ULL(d)	((unsigned long long)((d) * 1000000000))

#define MINORBITS	20
#define MINORMASK	((1U << MINORBITS) - 1)
#define MAJOR(dev)	((unsigned int) ((dev) >> MINORBITS))
#define MINOR(dev)	((unsigned int) ((dev) & MINORMASK))

#define min(a, b)	((a) < (b) ? (a) : (b))

#define HEADER		"%D %2c %8s %5T.%9t %5p %2a %3d "

struct io_stats {
	unsigned long qreads, qwrites, creads, cwrites, mreads, mwrites;
	unsigned long ireads, iwrites;
	unsigned long long qread_kb, qwrite_kb, cread_kb, cwrite_kb;
	unsigned long long iread_kb, iwrite_kb;
	unsigned long io_unplugs, timer_unplugs;
};

struct per_cpu_info {
	int cpu;
	int nelems;

	int fd;
	char fname[128];

	struct io_stats io_stats;
};

struct per_dev_info {
	dev_t id;
	char *name;

	int backwards;
	unsigned long long events;
	unsigned long long last_reported_time;
	struct io_stats io_stats;
	unsigned long last_sequence;
	unsigned long skips;

	int ncpus;
	struct per_cpu_info *cpus;
};

struct per_process_info {
	char name[16];
	__u32 pid;
	struct io_stats io_stats;
	struct per_process_info *hash_next, *list_next;

	/*
	 * individual io stats
	 */
	unsigned long long longest_allocation_wait[2];
	unsigned long long longest_dispatch_wait[2];
	unsigned long long longest_completion_wait[2];
};

#define PPI_HASH_SHIFT	(8)
static struct per_process_info *ppi_hash[1 << PPI_HASH_SHIFT];
static struct per_process_info *ppi_list;
static int ppi_list_entries;

#define S_OPTS	"i:o:b:stqw:f:F:v"
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
static struct rb_root rb_track_root;

static struct trace *trace_list;

/*
 * for tracking individual ios
 */
struct io_track {
	struct rb_node rb_node;

	dev_t device;
	__u64 sector;
	__u32 pid;
	unsigned long long allocation_time;
	unsigned long long queue_time;
	unsigned long long dispatch_time;
	unsigned long long completion_time;
};

static int ndevices;
static struct per_dev_info *devices;
static char *get_dev_name(struct per_dev_info *, char *, int);

static FILE *ofp;
static char *output_name;

static unsigned long long genesis_time;
static unsigned long long stopwatch_start;	/* start from zero by default */
static unsigned long long stopwatch_end = ULONG_LONG_MAX;	/* "infinity" */

static int per_process_stats;
static int track_ios;

#define RB_BATCH_DEFAULT	(1024)
static int rb_batch = RB_BATCH_DEFAULT;

static int pipeline;

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
	ppi_list_entries++;
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

	if (genesis_time == 0 || t->bit->time < genesis_time)
		genesis_time = t->bit->time;

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

static struct io_track *find_track(__u32 pid, dev_t device, __u64 sector)
{
	struct io_track *iot;

	iot = __find_track(device, sector);
	if (!iot) {
		iot = malloc(sizeof(*iot));
		iot->pid = pid;
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

	iot = find_track(t->pid, t->device, t->sector);
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

	iot = find_track(t->pid, t->device, t->sector);
	iot->queue_time = t->time;
	elapsed = iot->queue_time - iot->allocation_time;

	if (per_process_stats) {
		struct per_process_info *ppi = find_process_by_pid(iot->pid);
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
		struct per_process_info *ppi = find_process_by_pid(iot->pid);
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
		struct per_process_info *ppi = find_process_by_pid(iot->pid);
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

	for (i = 0; i < ndevices; i++)
		if (devices[i].id == id)
			return &devices[i];

	if (resize_devices(NULL) != 0)
		return NULL;

	pdi = &devices[ndevices - 1];
	pdi->id = id;
	pdi->last_sequence = -1;
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

#define VALID_SPECS	"BCDFGMPQRSTU"
static char *override_format[256];
static inline int valid_spec(int spec)
{
	return strchr(VALID_SPECS, spec) != NULL;
}

static void set_all_format_specs(char *optarg)
{
	char *p;

	for (p = VALID_SPECS; *p; p++)
		if (override_format[(int)(*p)] == NULL)
			override_format[(int)(*p)] = strdup(optarg);
}

static int add_format_spec(char *optarg)
{
	int spec = optarg[0];

	if (!valid_spec(spec)) {
		fprintf(stderr,"Bad format specifier %c\n", spec);
		return 1;
	}
	if (optarg[1] != ',') {
		fprintf(stderr,"Bad format specifier - need ',' %s\n", optarg);
		return 1;
	}
	optarg += 2;
	if (*optarg == '\0') {
		fprintf(stderr,"Bad format specifier - need fmt %s\n", optarg);
		return 1;
	}

	/*
	 * Set both merges (front and back)
	 */
	if (spec == 'M') {
		override_format['B'] = strdup(optarg);
		override_format['M'] = strdup(optarg);
	} else
		override_format[spec] = strdup(optarg);

	return 0;
}

static void print_field(char *act, struct per_cpu_info *pci,
			struct blk_io_trace *t, unsigned long long elapsed,
			int pdu_len, unsigned char *pdu_buf, char field,
			int minus, int has_w, int width)
{
	char format[64];

	if (has_w) {
		if (minus)
			sprintf(format, "%%-%d", width);
		else
			sprintf(format, "%%%d", width);
	} else
		sprintf(format, "%%");

	switch (field) {
	case 'a':
		fprintf(ofp, strcat(format, "s"), act);
		break;
	case 'c':
		fprintf(ofp, strcat(format, "d"), pci->cpu);
		break;
	case 'C':
		fprintf(ofp, strcat(format, "s"), t->comm);
		break;
	case 'd': {
		char rwbs[4];
		int i = 0;
		int w = t->action & BLK_TC_ACT(BLK_TC_WRITE);
		int b = t->action & BLK_TC_ACT(BLK_TC_BARRIER);
		int s = t->action & BLK_TC_ACT(BLK_TC_SYNC);
		if (w)
			rwbs[i++] = 'W';
		else
			rwbs[i++] = 'R';
		if (b)
			rwbs[i++] = 'B';
		if (s)
			rwbs[i++] = 'S';
		rwbs[i] = '\0';
		fprintf(ofp, strcat(format, "s"), rwbs);
		break;
	}
	case 'D':	/* format width ignored */
		fprintf(ofp,"%3d,%-3d", MAJOR(t->device), MINOR(t->device));
		break;
	case 'e':
		fprintf(ofp, strcat(format, "d"), t->error);
		break;
	case 'M':
		fprintf(ofp, strcat(format, "d"), MAJOR(t->device));
		break;
	case 'm':
		fprintf(ofp, strcat(format, "d"), MINOR(t->device));
		break;
	case 'n':
		fprintf(ofp, strcat(format, "u"), t->bytes >> 9);
		break;
	case 'p':
		fprintf(ofp, strcat(format, "u"), t->pid);
		break;
	case 'P':	/* format width ignored */
		if ((pdu_len > 0) && (pdu_buf != NULL)) {
			int i;
			unsigned char *p = pdu_buf;
			for (i = 0; i < pdu_len; i++) {
				if (i)
					fprintf(ofp, " ");

				fprintf(ofp, "%02x", *p++);
			}
		}
		break;
	case 's':
		fprintf(ofp, strcat(format, "ld"), t->sequence);
		break;
	case 'S':
		fprintf(ofp, strcat(format, "lu"), t->sector);
		break;
	case 't':
		sprintf(format, "%%0%dlu", has_w ? width : 9);
		fprintf(ofp, format, NANO_SECONDS(t->time));
		break;
	case 'T':
		fprintf(ofp, strcat(format, "d"), SECONDS(t->time));
		break;
	case 'u':
		if (elapsed == -1ULL) {
			fprintf(stderr, "Expecting elapsed value\n");
			exit(1);
		}
		fprintf(ofp, strcat(format, "llu"), elapsed / 1000);
		break;
	case 'U': {
		__u64 *depth = (__u64 *) ((char *) t + sizeof(*t));
		fprintf(ofp, strcat(format, "u"),
					(unsigned int) be64_to_cpu(*depth));
		break;
	}
	default:
		fprintf(ofp,strcat(format, "c"), field);
		break;
	}
}

static char *parse_field(char *act, struct per_cpu_info *pci, 
			 struct blk_io_trace *t, unsigned long long elapsed, 
			 int pdu_len, unsigned char *pdu_buf, 
			 char *master_format)
{
	int minus = 0;
	int has_w = 0;
	int width = 0;
	char *p = master_format;

	if (*p == '-') {
		minus = 1;
		p++;
	}
	if (isdigit(*p)) {
		has_w = 1;
		do {
			width = (width * 10) + (*p++ - '0');
		} while ((*p) && (isdigit(*p)));
	}
	if (*p) {
		print_field(act, pci, t, elapsed, pdu_len, pdu_buf, *p++,
			    minus, has_w, width);
	}
	return p;
}

static char *fmt_select(int fmt_spec, struct blk_io_trace *t,
			unsigned long long elapsed)
{
	char *fmt;
	char scratch_format[1024];

	if (override_format[fmt_spec] != NULL)
		return override_format[fmt_spec];

	switch (fmt_spec) {
	case 'C': 	/* Complete */
		if (t->action & BLK_TC_ACT(BLK_TC_PC)) {
			strcpy(scratch_format, HEADER);
			strcat(scratch_format, "(%P) ");
		} else {
			strcpy(scratch_format, HEADER "%S + %n ");
			if (elapsed != -1ULL)
				strcat(scratch_format, "(%8u) ");
		}
		strcat(scratch_format, "[%e]\n");
		fmt = scratch_format;
		break;

	case 'D': 	/* Issue */
		if (t->action & BLK_TC_ACT(BLK_TC_PC)) {
			strcpy(scratch_format, HEADER);
			strcat(scratch_format, "%n (%P) ");
		} else {
			strcpy(scratch_format, HEADER "%S + %n ");
			if (elapsed != -1ULL)
				strcat(scratch_format, "(%8u) ");
		}
		strcat(scratch_format,"[%C]\n");
		fmt = scratch_format;
		break;

	case 'I': 	/* Insert */
		if (t->action & BLK_TC_ACT(BLK_TC_PC)) {
			strcpy(scratch_format, HEADER);
			strcat(scratch_format, "%n (%P) ");
		} else {
			strcpy(scratch_format, HEADER "%S + %n ");
			if (elapsed != -1ULL)
				strcat(scratch_format, "(%8u) ");
		}
		strcat(scratch_format,"[%C]\n");
		fmt = scratch_format;
		break;

	case 'Q': 	/* Queue */
	case 'W':	/* Bounce */
		strcpy(scratch_format, HEADER "%S + %n ");
		if (elapsed != -1ULL)
			strcat(scratch_format, "(%8u) ");
		strcat(scratch_format,"[%C]\n");
		fmt = scratch_format;
		break;

	case 'B':	/* Back merge */
	case 'F':	/* Front merge */
	case 'M':	/* Front or back merge */
		fmt = HEADER "%S + %n [%C]\n";
		break;

	case 'P':	/* Plug */
		fmt = HEADER "[%C]\n";
		break;

	case 'G':	/* Get request */
	case 'S':	/* Sleep request */
		fmt = HEADER "%S + %n [%C]\n";
		break;

	case 'U':	/* Unplug IO */
	case 'T':	/* Unplug timer */
		fmt = HEADER "[%C] %U\n";
		break;

	case 'X': 	/* Split */
		strcpy(scratch_format, HEADER "%S / %U ");
		strcat(scratch_format,"[%C]\n");
		fmt = scratch_format;
		break;

	default:
		fprintf(stderr,"FATAL: Invalid format spec %c\n", fmt_spec);
		exit(1);
		/*NOTREACHED*/
	}

	return fmt;
}

static void process_fmt(char *act, struct per_cpu_info *pci,
			   struct blk_io_trace *t, unsigned long long elapsed,
			   int pdu_len, unsigned char *pdu_buf)
{
	char *p = fmt_select(act[0], t, elapsed);

	while (*p) {
		switch (*p) {
		case '%': 	/* Field specifier */
			p++;
			if (*p == '%')
				fprintf(ofp, "%c", *p++);
			else if (!*p)
				fprintf(ofp, "%c", '%');
			else
				p = parse_field(act, pci, t, elapsed,
						pdu_len, pdu_buf, p);
			break;
		case '\\': {	/* escape */
			switch (p[1]) {
			case 'b': fprintf(ofp, "\b"); break;
			case 'n': fprintf(ofp, "\n"); break;
			case 'r': fprintf(ofp, "\r"); break;
			case 't': fprintf(ofp, "\t"); break;
			default:
				fprintf(stderr,	
					"Invalid escape char in format %c\n",
					p[1]);
				exit(1);
				/*NOTREACHED*/
			}
			p += 2;
			break;
		}
		default:
			fprintf(ofp, "%c", *p++);
			break;
		}
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

		snprintf(name, sizeof(name)-1, "%s (%u)", ppi->name, ppi->pid);
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

static struct blk_io_trace *find_trace(void *p, unsigned long offset)
{
	unsigned long max_offset = offset;
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

static inline int verify_and_add_trace(struct trace *t)
{
	if (verify_trace(t->bit))
		return 1;
	if (trace_rb_insert(t))
		return 1;

	return 0;
}

static int sort_entries(void)
{
	struct trace *t;
	int nr = 0;

	while ((t = trace_list) != NULL) {
		trace_list = t->next;
		verify_and_add_trace(t);
		nr++;
	}

	return nr;
}

static void show_entries_rb(int piped)
{
	struct per_dev_info *pdi = NULL;
	struct per_cpu_info *pci = NULL;
	struct blk_io_trace *bit;
	struct rb_node *n;
	struct trace *t;
	__u32 device = 0;
	int cpu = 0;

	n = rb_first(&rb_sort_root);
	while (n != NULL) {

		if (done)
			break;

		t = rb_entry(n, struct trace, rb_node);
		bit = t->bit;

		if (!pdi || device != bit->device) {
			device = bit->device;
			pdi = get_dev_info(device);
		}

		if (!pdi) {
			fprintf(stderr, "Unknown device ID? (%d,%d)\n",
				MAJOR(bit->device), MINOR(bit->device));
			break;
		}

		if (bit->cpu > pdi->ncpus) {
			fprintf(stderr, "Unknown CPU ID? (%d, device %d,%d)\n",
				cpu, MAJOR(bit->device), MINOR(bit->device));
			break;
		}

		if (!pci || cpu != bit->cpu) {
			cpu = bit->cpu;
			pci = get_cpu_info(pdi, cpu);
		}

		/*
		 * back off displaying more info if we are out of sync
		 * on SMP systems. to prevent stalling on lost events,
		 * only allow an event to us a few times
		 */
		if (bit->sequence != (pdi->last_sequence + 1)
		    && pdi->last_sequence != -1) {
			if (piped && t->skipped < 5) {
				t->skipped++;
				break;
			} else {
				fprintf(stderr, "skipping from %lu to %u\n", pdi->last_sequence, bit->sequence);
				pdi->skips++;
			}
		}

		pdi->last_sequence = bit->sequence;

		bit->time -= genesis_time;
		if (bit->time >= stopwatch_end)
			break;

		if (bit->time >= stopwatch_start) {
			check_time(pdi, bit);

			dump_trace(bit, pci, pdi);
		}

		if (piped) {
			rb_erase(&t->rb_node, &rb_sort_root);
			free(bit);
			free(t);
			n = rb_first(&rb_sort_root);		
		} else
			n = rb_next(n);
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

/*
 * Find the traces in 'tb' and add them to the list for sorting and
 * displaying
 */
static int find_sort_entries(void *tb, unsigned long size)
{
	struct blk_io_trace *bit;
	struct trace *t;
	void *start = tb;
	int nr = 0;

	while (tb - start <= size - sizeof(*bit)) {
		bit = find_trace(tb, size - (tb - start));
		if (!bit)
			break;

		t = malloc(sizeof(*t));
		memset(t, 0, sizeof(*t));
		t->bit = bit;

		trace_to_cpu(bit);

		verify_and_add_trace(t);

		tb += sizeof(*bit) + bit->pdu_len;
		nr++;
	}

	return nr;
}

static int do_file(void)
{
	int i, j, nfiles = 0;

	for (i = 0; i < ndevices; i++) {
		for (j = 0;; j++, nfiles++) {
			struct per_dev_info *pdi;
			struct per_cpu_info *pci;
			struct stat st;
			void *tb;

			pdi = &devices[i];
			pdi->last_sequence = -1;
			pci = get_cpu_info(pdi, j);
			pci->cpu = j;

			snprintf(pci->fname, sizeof(pci->fname)-1,
				 "%s.blktrace.%d", pdi->name, j);
			if (stat(pci->fname, &st) < 0)
				break;
			if (!st.st_size)
				continue;

			printf("Processing %s\n", pci->fname);

			tb = malloc(st.st_size);
			if (!tb) {
				fprintf(stderr, "Out of memory, skip file %s\n",
					pci->fname);
				continue;
			}

			pci->fd = open(pci->fname, O_RDONLY);
			if (pci->fd < 0) {
				perror(pci->fname);
				free(tb);
				continue;
			}

			if (read_data(pci->fd, tb, st.st_size, 1)) {
				close(pci->fd);
				free(tb);
				continue;
			}

			pci->nelems = find_sort_entries(tb, st.st_size);

			printf("Completed %s (CPU%d %d, entries)\n",
				pci->fname, j, pci->nelems);
			close(pci->fd);
		}
	}

	if (!nfiles) {
		fprintf(stderr, "No files found\n");
		return 1;
	}

	show_entries_rb(0);
	return 0;
}

static int read_sort_events(int fd)
{
	int events = 0;

	do {
		struct blk_io_trace *bit;
		struct trace *t;
		int pdu_len;
		__u32 magic;

		bit = malloc(sizeof(*bit));

		if (read_data(fd, bit, sizeof(*bit), !events))
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

		t = malloc(sizeof(*t));
		memset(t, 0, sizeof(*t));
		t->bit = bit;

		trace_to_cpu(bit);
		t->next = trace_list;
		trace_list = t;

		events++;
	} while (!is_done() && events < rb_batch);

	return events;
}

static int do_stdin(void)
{
	int fd;

	fd = dup(STDIN_FILENO);
	do {
		int events;

		events = read_sort_events(fd);
		if (!events)
			break;
	
		if (sort_entries() == -1)
			break;

		show_entries_rb(1);
	} while (1);

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
