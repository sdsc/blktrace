#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>

#include "blktrace.h"
#include "rbtree.h"

#define NELEMS(pfi) ((pfi)->stat.st_size / sizeof(struct blk_io_trace))

#define MAX_CPUS	(512)

struct per_file_info {
	int cpu;
	int nelems;
	struct stat stat;

	int fd;
	char *fname;
	FILE *ofp;
	char *ofname;
	int dfd;
	char *dname;

	void *trace_buf;

	unsigned long long start_time;
};

static struct rb_root rb_root;

struct trace {
	struct blk_io_trace *bit;
	struct rb_node rb_node;
};

struct per_file_info per_file_info[MAX_CPUS];
struct per_file_info *current;

static unsigned long qreads, qwrites, creads, cwrites, mreads, mwrites;
static unsigned long long qread_kb, qwrite_kb, cread_kb, cwrite_kb;
static unsigned long long events, missed_events;

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

void output(char *s)
{
	printf("%s", s);
	fprintf(current->ofp,"%s",s);
}

char hstring[256];
char tstring[256];

inline char *setup_header(struct blk_io_trace *t, char act)
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

	sprintf(hstring, "%3d %15ld %12Lu %5u %c %3s", current->cpu,
		(unsigned long)t->sequence, (unsigned long long)t->time, t->pid,
		act, rwbs);

	return hstring;
}

void log_complete(struct blk_io_trace *t, char act)
{
	sprintf(tstring,"%s %Lu + %u [%d]\n", setup_header(t, act),
		(unsigned long long)t->sector, t->bytes >> 9, t->error);
	output(tstring);
}

void log_queue(struct blk_io_trace *t, char act)
{
	sprintf(tstring,"%s %Lu + %u\n", setup_header(t, act),
		(unsigned long long)t->sector, t->bytes >> 9);
	output(tstring);
}

void log_issue(struct blk_io_trace *t, char act)
{
	sprintf(tstring,"%s %Lu + %u\n", setup_header(t, act),
		(unsigned long long)t->sector, t->bytes >> 9);
	output(tstring);
}

void log_merge(struct blk_io_trace *t, char act)
{
	sprintf(tstring,"%s   %Lu + %u\n", setup_header(t, act),
		(unsigned long long)t->sector, t->bytes >> 9);
	output(tstring);
}

void log_generic(struct blk_io_trace *t, char act)
{
	sprintf(tstring,"%s %Lu + %u\n", setup_header(t, act),
		(unsigned long long)t->sector, t->bytes >> 9);
	output(tstring);
}

void log_pc(struct blk_io_trace *t, char act)
{
	int i, ret;
	unsigned char buf[64];

	sprintf(tstring,"%s\n", setup_header(t, act));
	output(tstring);

	if (t->pdu_len > sizeof(buf)) {
		fprintf(stderr, "Payload too large %d\n", t->pdu_len);
		return;
	}

	ret = read(current->dfd, buf, t->pdu_len);
	if (ret != t->pdu_len) {
		fprintf(stderr,"read(%d) failed on %s - %d\n", t->pdu_len, 
			current->dname, ret);
		exit(1);
	}

	for (i = 0; i < t->pdu_len; i++) {
		sprintf(tstring,"%02x ", buf[i]);
		output(tstring);
	}

	if (act == 'C') {
		sprintf(tstring,"[%d]", t->error);
		output(tstring);
	}

	printf("\n");
}

void dump_trace_pc(struct blk_io_trace *t)
{
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
			log_pc(t, 'D');
			break;
		case __BLK_TA_COMPLETE:
			log_pc(t, 'C');
			break;
		default:
			fprintf(stderr, "Bad pc action %x\n", t->action);
			return;
	}
	
	events++;
}

void dump_trace_fs(struct blk_io_trace *t)
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
	
	events++;
}

void dump_trace(struct blk_io_trace *t)
{
	if (t->action & BLK_TC_ACT(BLK_TC_PC))
		dump_trace_pc(t);
	else
		dump_trace_fs(t);
}

void show_stats(void)
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
	printf("Missed events: %'Lu\n", missed_events);
}

int compar(const void *t1, const void *t2)
{
	long v1 = (long)(((struct blk_io_trace *)t1)->sequence);
	long v2 = (long)(((struct blk_io_trace *)t2)->sequence);

	return v1 - v2;
}

inline int trace_rb_insert(struct trace *t)
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
			fprintf(stderr, "sequence alias %u!\n", t->bit->sequence);
			return 1;
		}
	}

	rb_link_node(&t->rb_node, parent, p);
	rb_insert_color(&t->rb_node, &rb_root);
	return 0;
}

int sort_entries(void *traces, unsigned long offset)
{
	struct blk_io_trace *bit;
	struct trace *t;
	void *start = traces;
	int nelems = 0;

	memset(&rb_root, 0, sizeof(rb_root));

	do {
		bit = traces;
		t = malloc(sizeof(*t));
		t->bit = bit;
		memset(&t->rb_node, 0, sizeof(t->rb_node));

		if (trace_rb_insert(t))
			return -1;

		traces += sizeof(*bit) + bit->pdu_len;
		nelems++;
	} while (traces < start + offset);

	return nelems;
}

void show_entries(void)
{
	struct rb_node *n = rb_first(&rb_root);
	struct blk_io_trace *bit;
	struct trace *t;
	int cpu;

	do {
		if (!n)
			break;

		t = rb_entry(n, struct trace, rb_node);
		bit = t->bit;

		cpu = bit->magic;
		if (cpu >= MAX_CPUS) {
			fprintf(stderr, "CPU number too large (%d)\n", cpu);
			return;
		}

		current = &per_file_info[cpu];

		/*
		 * offset time by first trace event.
		 *
		 * NOTE: This is *cpu* relative, thus you can not
		 * compare times ACROSS cpus.
		 */
		if (current->start_time == 0)
			current->start_time = bit->time;

		bit->time -= current->start_time;

		dump_trace(bit);
	} while ((n = rb_next(n)) != NULL);
}

int main(int argc, char *argv[])
{
	char *dev;
	int i, nfiles, nelems, ret;
	struct per_file_info *pfi;

	if (argc != 2) {
		fprintf(stderr, "Usage %s <dev>\n", argv[0]);
		return 1;
	}

	dev = argv[1];

	nfiles = nelems = 0;
	for (i = 0, pfi = &per_file_info[0]; i < MAX_CPUS; i++, pfi++) {
		pfi->cpu = i;
		pfi->start_time = 0;

		pfi->fname = malloc(128);
		sprintf(pfi->fname, "%s_out.%d", dev, i);
		if (stat(pfi->fname, &pfi->stat) < 0)
			break;

		pfi->dname = malloc(128);
		snprintf(pfi->dname, 127, "%s_dat.%d", dev, i);
		pfi->dfd = open(pfi->dname, O_RDONLY);
		if (pfi->dfd < 0) {
			perror(pfi->dname);
			return 1;
		}

		pfi->ofname = malloc(128);
		snprintf(pfi->ofname, 127, "%s_log.%d", dev, i);
		pfi->ofp = fopen(pfi->ofname, "w");
		if (pfi->ofp == NULL) {
			perror(pfi->ofname);
			return 1;
		}

		printf("Processing %s\n", pfi->fname);

		pfi->trace_buf = malloc(pfi->stat.st_size);

		pfi->fd = open(pfi->fname, O_RDONLY);
		if (pfi->fd < 0) {
			perror(pfi->fname);
			return 1;
		}
		if (read(pfi->fd, pfi->trace_buf, pfi->stat.st_size) != pfi->stat.st_size) {
			fprintf(stderr, "error reading\n");
			return 1;
		}

		ret = sort_entries(pfi->trace_buf, pfi->stat.st_size);
		if (ret == -1)
			return 1;

		close(pfi->fd);
		nfiles++;
		pfi->nelems = ret;
		nelems += pfi->nelems;
		printf("\t%2d %10s %15d\n", i, pfi->fname, pfi->nelems);

	}

	show_entries();

	show_stats();
	return 0;
}


