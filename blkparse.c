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

#include "blktrace.h"
#include "rbtree.h"

#define SECONDS(x) 	((unsigned long long)(x) / 1000000000)
#define NANO_SECONDS(x)	((unsigned long long)(x) % 1000000000)

static int backwards;
static unsigned long long genesis_time, last_reported_time;

struct per_cpu_info {
	int cpu;
	int nelems;

	int fd;
	char fname[128];

	FILE *ofp;
	char ofname[128];

	unsigned long qreads, qwrites, creads, cwrites, mreads, mwrites;
	unsigned long ireads, iwrites;
	unsigned long long qread_kb, qwrite_kb, cread_kb, cwrite_kb;
	unsigned long long iread_kb, iwrite_kb;
};

#define S_OPTS	"i:o:"
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
		.name = NULL,
		.has_arg = 0,
		.flag = NULL,
		.val = 0
	}
};

static struct rb_root rb_root;

struct trace {
	struct blk_io_trace *bit;
	struct rb_node rb_node;
};

static int max_cpus;
static struct per_cpu_info *per_cpu_info;

static unsigned long long events;

static char *dev, *output_name;

#define is_done()	(*(volatile int *)(&done))
static volatile int done;

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

static inline void account_m(struct per_cpu_info *pci, int rw,
			     unsigned int bytes)
{
	if (rw) {
		pci->mwrites++;
		pci->qwrite_kb += bytes >> 10;
	} else {
		pci->mreads++;
		pci->qread_kb += bytes >> 10;
	}
}

static inline void account_q(struct per_cpu_info *pci, int rw,
			     unsigned int bytes)
{
	if (rw) {
		pci->qwrites++;
		pci->qwrite_kb += bytes >> 10;
	} else {
		pci->qreads++;
		pci->qread_kb += bytes >> 10;
	}
}

static inline void account_c(struct per_cpu_info *pci, int rw,
			     unsigned int bytes)
{
	if (rw) {
		pci->cwrites++;
		pci->cwrite_kb += bytes >> 10;
	} else {
		pci->creads++;
		pci->cread_kb += bytes >> 10;
	}
}

static inline void account_i(struct per_cpu_info *pci, int rw,
			     unsigned int bytes)
{
	if (rw) {
		pci->iwrites++;
		pci->iwrite_kb += bytes >> 10;
	} else {
		pci->ireads++;
		pci->iread_kb += bytes >> 10;
	}
}

static void output(struct per_cpu_info *pci, char *s)
{
	printf("%s", s);

	if (pci->ofp)
		fprintf(pci->ofp, "%s", s);
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

	sprintf(hstring, "%c %3d %15ld %5Lu.%09Lu %5u %c %3s", backwards,
		pci->cpu,
		(unsigned long)t->sequence, SECONDS(t->time), 
		NANO_SECONDS(t->time), t->pid, act, rwbs);

	return hstring;
}

static void log_complete(struct per_cpu_info *pci, struct blk_io_trace *t,
			 char act)
{
	sprintf(tstring,"%s %Lu + %u [%d]\n", setup_header(pci, t, act),
		(unsigned long long)t->sector, t->bytes >> 9, t->error);
	output(pci, tstring);
}

static void log_queue(struct per_cpu_info *pci, struct blk_io_trace *t,
		      char act)
{
	sprintf(tstring,"%s %Lu + %u [%s]\n", setup_header(pci, t, act),
		(unsigned long long)t->sector, t->bytes >> 9, t->comm);
	output(pci, tstring);
}

static void log_issue(struct per_cpu_info *pci, struct blk_io_trace *t,
		      char act)
{
	sprintf(tstring,"%s %Lu + %u [%s]\n", setup_header(pci, t, act),
		(unsigned long long)t->sector, t->bytes >> 9, t->comm);
	output(pci, tstring);
}

static void log_merge(struct per_cpu_info *pci, struct blk_io_trace *t,
		      char act)
{
	sprintf(tstring,"%s   %Lu + %u [%s]\n", setup_header(pci, t, act),
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

	switch (t->action & 0xffff) {
		case __BLK_TA_QUEUE:
			account_q(pci, w, t->bytes);
			log_queue(pci, t, 'Q');
			break;
		case __BLK_TA_BACKMERGE:
			account_m(pci, w, t->bytes);
			log_merge(pci, t, 'M');
			break;
		case __BLK_TA_FRONTMERGE:
			account_m(pci, w, t->bytes);
			log_merge(pci, t, 'F');
			break;
		case __BLK_TA_GETRQ:
			log_generic(pci, t, 'G');
			break;
		case __BLK_TA_SLEEPRQ:
			log_generic(pci, t, 'S');
			break;
		case __BLK_TA_REQUEUE:
			account_c(pci, w, -t->bytes);
			log_queue(pci, t, 'R');
			break;
		case __BLK_TA_ISSUE:
			account_i(pci, w, t->bytes);
			log_issue(pci, t, 'D');
			break;
		case __BLK_TA_COMPLETE:
			account_c(pci, w, t->bytes);
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

	if (output_name && !pci->ofp) {
		snprintf(pci->ofname, sizeof(pci->ofname) - 1,
				"%s_log.%d", output_name, pci->cpu);

		pci->ofp = fopen(pci->ofname, "w");
		if (pci->ofp == NULL) {
			perror(pci->ofname);
			return 1;
		}
	}

	if (t->action & BLK_TC_ACT(BLK_TC_PC))
		ret = dump_trace_pc(t, pci);
	else
		dump_trace_fs(t, pci);

	events++;
	return ret;
}

static void dump_pci_stats(struct per_cpu_info *pci)
{
	printf("\tReads:\n");
	printf("\t\tQueued:    %'8lu, %'8LuKiB\n", pci->qreads, pci->qread_kb);
	printf("\t\tDispatched %'8lu, %'8LuKiB\n", pci->ireads, pci->iread_kb);
	printf("\t\tCompleted: %'8lu, %'8LuKiB\n", pci->creads, pci->cread_kb);
	printf("\t\tMerges:    %'8lu\n", pci->mreads);

	printf("\tWrites:\n");
	printf("\t\tQueued:    %'8lu, %'8LuKiB\n", pci->qwrites,pci->qwrite_kb);
	printf("\t\tDispatched %'8lu, %'8LuKiB\n", pci->iwrites,pci->iwrite_kb);
	printf("\t\tCompleted: %'8lu, %'8LuKiB\n", pci->cwrites,pci->cwrite_kb);
	printf("\t\tMerges:    %'8lu\n", pci->mwrites);
}

static void show_stats(void)
{
	struct per_cpu_info foo, *pci;
	int i, pci_events = 0;

	memset(&foo, 0, sizeof(foo));

	for (i = 0; i < max_cpus; i++) {
		pci = &per_cpu_info[i];

		if (!pci->nelems)
			continue;

		foo.qreads += pci->qreads;
		foo.qwrites += pci->qwrites;
		foo.creads += pci->creads;
		foo.cwrites += pci->cwrites;
		foo.mreads += pci->mreads;
		foo.mwrites += pci->mwrites;
		foo.qread_kb += pci->qread_kb;
		foo.qwrite_kb += pci->qwrite_kb;
		foo.cread_kb += pci->cread_kb;
		foo.cwrite_kb += pci->cwrite_kb;

		printf("CPU%d:\n", i);
		dump_pci_stats(pci);
		pci_events++;
	}

	if (pci_events > 1) {
		printf("Total:\n");
		dump_pci_stats(&foo);
	}

	printf("Events: %'Lu\n", events);
}

static inline int trace_rb_insert(struct trace *t)
{
	struct rb_node **p = &rb_root.rb_node;
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
	rb_insert_color(&t->rb_node, &rb_root);
	return 0;
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

		bit = traces;

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

	while ((n = rb_first(&rb_root)) != NULL) {
		struct trace *t = rb_entry(n, struct trace, rb_node);

		rb_erase(&t->rb_node, &rb_root);
		free(t);
	}
}

static void show_entries_rb(void)
{
	struct blk_io_trace *bit;
	struct rb_node *n;
	struct trace *t;
	int cpu;

	n = rb_first(&rb_root);
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
		pci->ofp = NULL;

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

static void resize_buffer(void **buffer, long *old_size)
{
	long cur_size = *old_size;
	void *ptr;

	*old_size *= 2;
	ptr = malloc(*old_size);
	memcpy(ptr, *buffer, cur_size);
	free(*buffer);
	*buffer = ptr;
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
			resize_buffer(buffer, &max_offset);

		if (read_data(fd, *buffer + offset, sizeof(*t), !events)) {
			if (events)
				break;

			usleep(1000);
			continue;
		}

		t = *buffer + offset;
		offset += sizeof(*t);

		pdu_len = be16_to_cpu(t->pdu_len);

		if (max_offset - offset < pdu_len)
			resize_buffer(buffer, &max_offset);

		if (read_data(fd, *buffer + offset, pdu_len, 1))
			break;

		offset += pdu_len;
		events++;
	} while (!is_done());

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
	
		sort_entries(ptr, ~0UL, events);
		show_entries_rb();
		free_entries_rb();
	} while (1);

	close(fd);
	free(ptr);
	return 0;
}

static void flush_output(void)
{
	int i;

	for (i = 0; i < max_cpus; i++) {
		struct per_cpu_info *pci = &per_cpu_info[i];

		if (pci->ofp) {
			fflush(pci->ofp);
			fclose(pci->ofp);
			pci->ofp = NULL;
		}
	}
}

static void handle_sigint(int sig)
{
	done = 1;
	flush_output();
}

static void usage(char *prog)
{
	fprintf(stderr, "Usage: %s -i <name> [-o <output>]\n", prog);
}

int main(int argc, char *argv[])
{
	int c, ret;

	while ((c = getopt_long(argc, argv, S_OPTS, l_opts, NULL)) != -1) {
		switch (c) {
		case 'i':
			dev = strdup(optarg);
			break;
		case 'o':
			output_name = strdup(optarg);
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

	memset(&rb_root, 0, sizeof(rb_root));

	signal(SIGINT, handle_sigint);
	signal(SIGHUP, handle_sigint);
	signal(SIGTERM, handle_sigint);

	if (!strcmp(dev, "-"))
		ret = do_stdin();
	else
		ret = do_file();

	show_stats();
	flush_output();
	return ret;
}
