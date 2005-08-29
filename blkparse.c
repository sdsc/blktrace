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
};

static struct rb_root rb_root;

struct trace {
	struct blk_io_trace *bit;
	unsigned int cpu;
	struct rb_node rb_node;
};

static struct per_file_info per_file_info[MAX_CPUS];
static struct per_file_info *cur_file;

static unsigned long qreads, qwrites, creads, cwrites, mreads, mwrites;
static unsigned long long qread_kb, qwrite_kb, cread_kb, cwrite_kb;
static unsigned long long events;

static int max_cpus;

static inline void account_m(int rw, unsigned int bytes)
{
	if (rw) {
		mwrites++;
		qwrite_kb += bytes >> 10;
	} else {
		mreads++;
		qread_kb += bytes >> 10;
	}
}

static inline void account_q(int rw, unsigned int bytes)
{
	if (rw) {
		qwrites++;
		qwrite_kb += bytes >> 10;
	} else {
		qreads++;
		qread_kb += bytes >> 10;
	}
}

static inline void account_c(int rw, unsigned int bytes)
{
	if (rw) {
		cwrites++;
		cwrite_kb += bytes >> 10;
	} else {
		creads++;
		cread_kb += bytes >> 10;
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

static int dump_trace_pc(struct blk_io_trace *t)
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

static void dump_trace_fs(struct blk_io_trace *t)
{
	int w = t->action & BLK_TC_ACT(BLK_TC_WRITE);

	switch (t->action & 0xffff) {
		case __BLK_TA_QUEUE:
			account_q(w, t->bytes);
			log_queue(t, 'Q');
			break;
		case __BLK_TA_BACKMERGE:
			account_m(w, t->bytes);
			log_merge(t, 'M');
			break;
		case __BLK_TA_FRONTMERGE:
			account_m(w, t->bytes);
			log_merge(t, 'F');
			break;
		case __BLK_TA_GETRQ:
			log_generic(t, 'G');
			break;
		case __BLK_TA_SLEEPRQ:
			log_generic(t, 'S');
			break;
		case __BLK_TA_REQUEUE:
			log_queue(t, 'R');
			break;
		case __BLK_TA_ISSUE:
			log_issue(t, 'D');
			break;
		case __BLK_TA_COMPLETE:
			account_c(w, t->bytes);
			log_complete(t, 'C');
			break;
		default:
			fprintf(stderr, "Bad fs action %x\n", t->action);
			return;
	}
}

static int dump_trace(struct blk_io_trace *t)
{
	int ret = 0;

	if (t->action & BLK_TC_ACT(BLK_TC_PC))
		ret = dump_trace_pc(t);
	else
		dump_trace_fs(t);

	events++;
	return ret;
}

static void show_stats(void)
{
	printf("\nReads:");
	printf("\tQueued:    %'8lu, %'8LuKiB\n", qreads, qread_kb);
	printf("\tCompleted: %'8lu, %'8LuKiB\n", creads, cread_kb);
	printf("\tMerges:    %'8lu\n", mreads);

	printf("Writes:");
	printf("\tQueued:    %'8lu, %'8LuKiB\n", qwrites, qwrite_kb);
	printf("\tCompleted: %'8lu, %'8LuKiB\n", cwrites, cwrite_kb);
	printf("\tMerges:    %'8lu\n", mwrites);

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

		if (dump_trace(bit))
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

	for (max_cpus = 0, i = 0, nfiles = 0; i < MAX_CPUS; i++) {
		struct per_file_info *pfi = &per_file_info[i];
		struct stat st;
		void *tb;

		pfi->cpu = i;
		pfi->start_time = 0;

		snprintf(pfi->fname, sizeof(pfi->fname)-1,"%s_out.%d", dev, i);
		if (stat(pfi->fname, &st) < 0)
			break;

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
		nfiles++;
		max_cpus++;
		pfi->nelems = ret;
		printf("\t%2d %10s %15d\n", i, pfi->fname, pfi->nelems);

	}

	if (nfiles) {
		show_entries();
		show_stats();
		return 0;
	}

	fprintf(stderr, "No files found\n");
	return 1;
}
