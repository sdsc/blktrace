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

#include "blktrace.h"
#include "rbtree.h"

#define MAX_CPUS	(512)

struct per_file_info {
	int cpu;
	int nelems;

	int fd;
	char fname[128];

	FILE *ofp;
	char ofname[128];

	unsigned long long start_time;

	unsigned long qreads, qwrites, creads, cwrites, mreads, mwrites;
	unsigned long ireads, iwrites;
	unsigned long long qread_kb, qwrite_kb, cread_kb, cwrite_kb;
	unsigned long long iread_kb, iwrite_kb;
};

static struct rb_root rb_root;

struct trace {
	struct blk_io_trace *bit;
	unsigned int cpu;
	struct rb_node rb_node;
};

static struct per_file_info per_file_info[MAX_CPUS];
static struct per_file_info *cur_file;

static unsigned long long events;

static int max_cpus;

static inline void account_m(struct per_file_info *pfi, int rw,
			     unsigned int bytes)
{
	if (rw) {
		pfi->mwrites++;
		pfi->qwrite_kb += bytes >> 10;
	} else {
		pfi->mreads++;
		pfi->qread_kb += bytes >> 10;
	}
}

static inline void account_q(struct per_file_info *pfi, int rw,
			     unsigned int bytes)
{
	if (rw) {
		pfi->qwrites++;
		pfi->qwrite_kb += bytes >> 10;
	} else {
		pfi->qreads++;
		pfi->qread_kb += bytes >> 10;
	}
}

static inline void account_c(struct per_file_info *pfi, int rw,
			     unsigned int bytes)
{
	if (rw) {
		pfi->cwrites++;
		pfi->cwrite_kb += bytes >> 10;
	} else {
		pfi->creads++;
		pfi->cread_kb += bytes >> 10;
	}
}

static inline void account_i(struct per_file_info *pfi, int rw,
			     unsigned int bytes)
{
	if (rw) {
		pfi->iwrites++;
		pfi->iwrite_kb += bytes >> 10;
	} else {
		pfi->ireads++;
		pfi->iread_kb += bytes >> 10;
	}
}

static void output(char *s)
{
	printf("%s", s);
	fprintf(cur_file->ofp,"%s",s);
}

static char hstring[256];
static char tstring[256];

static inline char *setup_header(struct blk_io_trace *t, char act)
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

	sprintf(hstring, "%3d %15ld %12Lu %5u %c %3s", cur_file->cpu,
		(unsigned long)t->sequence, (unsigned long long)t->time, t->pid,
		act, rwbs);

	return hstring;
}

static void log_complete(struct blk_io_trace *t, char act)
{
	sprintf(tstring,"%s %Lu + %u [%d]\n", setup_header(t, act),
		(unsigned long long)t->sector, t->bytes >> 9, t->error);
	output(tstring);
}

static void log_queue(struct blk_io_trace *t, char act)
{
	sprintf(tstring,"%s %Lu + %u\n", setup_header(t, act),
		(unsigned long long)t->sector, t->bytes >> 9);
	output(tstring);
}

static void log_issue(struct blk_io_trace *t, char act)
{
	sprintf(tstring,"%s %Lu + %u\n", setup_header(t, act),
		(unsigned long long)t->sector, t->bytes >> 9);
	output(tstring);
}

static void log_merge(struct blk_io_trace *t, char act)
{
	sprintf(tstring,"%s   %Lu + %u\n", setup_header(t, act),
		(unsigned long long)t->sector, t->bytes >> 9);
	output(tstring);
}

static void log_generic(struct blk_io_trace *t, char act)
{
	sprintf(tstring,"%s %Lu + %u\n", setup_header(t, act),
		(unsigned long long)t->sector, t->bytes >> 9);
	output(tstring);
}

static int log_pc(struct blk_io_trace *t, char act)
{
	unsigned char *buf;
	int i;

	sprintf(tstring,"%s ", setup_header(t, act));
	output(tstring);

	buf = (unsigned char *) t + sizeof(*t);
	for (i = 0; i < t->pdu_len; i++) {
		sprintf(tstring,"%02x ", buf[i]);
		output(tstring);
	}

	if (act == 'C') {
		sprintf(tstring,"[%d]", t->error);
		output(tstring);
	}

	printf("\n");
	return 0;
}

static int dump_trace_pc(struct blk_io_trace *t, struct per_file_info *pfi)
{
	int ret = 0;

	switch (t->action & 0xffff) {
		case __BLK_TA_QUEUE:
			log_generic(t, 'Q');
			break;
		case __BLK_TA_GETRQ:
			log_generic(t, 'G');
			break;
		case __BLK_TA_SLEEPRQ:
			log_generic(t, 'S');
			break;
		case __BLK_TA_REQUEUE:
			log_generic(t, 'R');
			break;
		case __BLK_TA_ISSUE:
			ret = log_pc(t, 'D');
			break;
		case __BLK_TA_COMPLETE:
			log_pc(t, 'C');
			break;
		default:
			fprintf(stderr, "Bad pc action %x\n", t->action);
			ret = 1;
			break;
	}
	
	return ret;
}

static void dump_trace_fs(struct blk_io_trace *t, struct per_file_info *pfi)
{
	int w = t->action & BLK_TC_ACT(BLK_TC_WRITE);

	switch (t->action & 0xffff) {
		case __BLK_TA_QUEUE:
			account_q(pfi, w, t->bytes);
			log_queue(t, 'Q');
			break;
		case __BLK_TA_BACKMERGE:
			account_m(pfi, w, t->bytes);
			log_merge(t, 'M');
			break;
		case __BLK_TA_FRONTMERGE:
			account_m(pfi, w, t->bytes);
			log_merge(t, 'F');
			break;
		case __BLK_TA_GETRQ:
			log_generic(t, 'G');
			break;
		case __BLK_TA_SLEEPRQ:
			log_generic(t, 'S');
			break;
		case __BLK_TA_REQUEUE:
			account_c(pfi, w, -t->bytes);
			log_queue(t, 'R');
			break;
		case __BLK_TA_ISSUE:
			account_i(pfi, w, t->bytes);
			log_issue(t, 'D');
			break;
		case __BLK_TA_COMPLETE:
			account_c(pfi, w, t->bytes);
			log_complete(t, 'C');
			break;
		default:
			fprintf(stderr, "Bad fs action %x\n", t->action);
			return;
	}
}

static int dump_trace(struct blk_io_trace *t, struct per_file_info *pfi)
{
	int ret = 0;

	if (t->action & BLK_TC_ACT(BLK_TC_PC))
		ret = dump_trace_pc(t, pfi);
	else
		dump_trace_fs(t, pfi);

	events++;
	return ret;
}

static void dump_pfi_stats(struct per_file_info *pfi)
{
	printf("\tReads:\n");
	printf("\t\tQueued:    %'8lu, %'8LuKiB\n", pfi->qreads, pfi->qread_kb);
	printf("\t\tDispatched %'8lu, %'8LuKiB\n", pfi->ireads, pfi->iread_kb);
	printf("\t\tCompleted: %'8lu, %'8LuKiB\n", pfi->creads, pfi->cread_kb);
	printf("\t\tMerges:    %'8lu\n", pfi->mreads);

	printf("\tWrites:\n");
	printf("\t\tQueued:    %'8lu, %'8LuKiB\n", pfi->qwrites,pfi->qwrite_kb);
	printf("\t\tDispatched %'8lu, %'8LuKiB\n", pfi->iwrites,pfi->iwrite_kb);
	printf("\t\tCompleted: %'8lu, %'8LuKiB\n", pfi->cwrites,pfi->cwrite_kb);
	printf("\t\tMerges:    %'8lu\n", pfi->mwrites);
}

static void show_stats(int nfiles)
{
	struct per_file_info foo, *pfi;
	int i;

	memset(&foo, 0, sizeof(foo));

	for (i = 0; i < nfiles; i++) {
		pfi = &per_file_info[i];

		if (!pfi->nelems)
			continue;

		foo.qreads += pfi->qreads;
		foo.qwrites += pfi->qwrites;
		foo.creads += pfi->creads;
		foo.cwrites += pfi->cwrites;
		foo.mreads += pfi->mreads;
		foo.mwrites += pfi->mwrites;
		foo.qread_kb += pfi->qread_kb;
		foo.qwrite_kb += pfi->qwrite_kb;
		foo.cread_kb += pfi->cread_kb;
		foo.cwrite_kb += pfi->cwrite_kb;

		printf("CPU%d:\n", i);
		dump_pfi_stats(pfi);
	}

	if (nfiles > 1) {
		printf("Total:\n");
		dump_pfi_stats(&foo);
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

static int sort_entries(void *traces, unsigned long offset, int cpu)
{
	struct blk_io_trace *bit;
	struct trace *t;
	void *start = traces;
	int nelems = 0;

	while (traces - start <= offset - sizeof(*bit)) {
		bit = traces;

		t = malloc(sizeof(*t));
		t->bit = bit;
		t->cpu = cpu;
		memset(&t->rb_node, 0, sizeof(t->rb_node));

		trace_to_cpu(bit);

		if (verify_trace(bit))
			break;

		if (trace_rb_insert(t))
			return -1;

		traces += sizeof(*bit) + bit->pdu_len;
		nelems++;
	}

	return nelems;
}

static void show_entries(void)
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

		cpu = t->cpu;
		if (cpu > max_cpus) {
			fprintf(stderr, "CPU number too large (%d)\n", cpu);
			break;
		}

		cur_file = &per_file_info[cpu];

		/*
		 * offset time by first trace event.
		 *
		 * NOTE: This is *cpu* relative, thus you can not
		 * compare times ACROSS cpus.
		 */
		if (cur_file->start_time == 0)
			cur_file->start_time = bit->time;

		bit->time -= cur_file->start_time;

		if (dump_trace(bit, cur_file))
			break;

	} while ((n = rb_next(n)) != NULL);
}

int main(int argc, char *argv[])
{
	int i, nfiles, ret;
	char *dev;

	if (argc != 2) {
		fprintf(stderr, "Usage: %s <dev>\n", argv[0]);
		return 1;
	}

	dev = argv[1];

	memset(&rb_root, 0, sizeof(rb_root));
	memset(per_file_info, 0, sizeof(per_file_info));

	for (max_cpus = 0, i = 0, nfiles = 0; i < MAX_CPUS; i++, nfiles++, max_cpus++) {
		struct per_file_info *pfi = &per_file_info[i];
		struct stat st;
		void *tb;

		pfi->cpu = i;
		pfi->start_time = 0;

		snprintf(pfi->fname, sizeof(pfi->fname)-1,"%s_out.%d", dev, i);
		if (stat(pfi->fname, &st) < 0)
			break;
		if (!st.st_size)
			continue;

		snprintf(pfi->ofname, sizeof(pfi->ofname)-1, "%s_log.%d", dev, i);
		pfi->ofp = fopen(pfi->ofname, "w");
		if (pfi->ofp == NULL) {
			perror(pfi->ofname);
			break;
		}

		printf("Processing %s\n", pfi->fname);

		tb = malloc(st.st_size);

		pfi->fd = open(pfi->fname, O_RDONLY);
		if (pfi->fd < 0) {
			perror(pfi->fname);
			break;
		}

		if (read(pfi->fd, tb, st.st_size) != st.st_size) {
			fprintf(stderr, "error reading\n");
			break;
		}

		ret = sort_entries(tb, st.st_size, i);
		if (ret == -1)
			break;

		close(pfi->fd);
		pfi->nelems = ret;
		printf("\t%2d %10s %15d\n", i, pfi->fname, pfi->nelems);

	}

	if (nfiles) {
		show_entries();
		show_stats(nfiles);
		return 0;
	}

	fprintf(stderr, "No files found\n");
	return 1;
}
