/*
 * Copyright (C) 2012 Fusion-io
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public
 *  License v2 as published by the Free Software Foundation.
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
 *  Parts of this file were imported from Jens Axboe's blktrace sources (also GPL)
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <inttypes.h>
#include <string.h>
#include <asm/types.h>
#include <errno.h>
#include <sys/mman.h>
#include <time.h>
#include <math.h>

#include "plot.h"
#include "blkparse.h"
#include "list.h"
#include "tracers.h"

#define IO_HASH_TABLE_BITS  11
#define IO_HASH_TABLE_SIZE (1 << IO_HASH_TABLE_BITS)
static struct list_head io_hash_table[IO_HASH_TABLE_SIZE];
static u64 ios_in_flight = 0;


/*
 * Trace categories
 */
enum {
	BLK_TC_READ	= 1 << 0,	/* reads */
	BLK_TC_WRITE	= 1 << 1,	/* writes */
	BLK_TC_FLUSH	= 1 << 2,	/* flush */
	BLK_TC_SYNC	= 1 << 3,	/* sync */
	BLK_TC_QUEUE	= 1 << 4,	/* queueing/merging */
	BLK_TC_REQUEUE	= 1 << 5,	/* requeueing */
	BLK_TC_ISSUE	= 1 << 6,	/* issue */
	BLK_TC_COMPLETE	= 1 << 7,	/* completions */
	BLK_TC_FS	= 1 << 8,	/* fs requests */
	BLK_TC_PC	= 1 << 9,	/* pc requests */
	BLK_TC_NOTIFY	= 1 << 10,	/* special message */
	BLK_TC_AHEAD	= 1 << 11,	/* readahead */
	BLK_TC_META	= 1 << 12,	/* metadata */
	BLK_TC_DISCARD	= 1 << 13,	/* discard requests */
	BLK_TC_DRV_DATA	= 1 << 14,	/* binary driver data */
	BLK_TC_FUA	= 1 << 15,	/* fua requests */

	BLK_TC_END	= 1 << 15,	/* we've run out of bits! */
};

#define BLK_TC_SHIFT		(16)
#define BLK_TC_ACT(act)		((act) << BLK_TC_SHIFT)
#define BLK_DATADIR(a) (((a) >> BLK_TC_SHIFT) & (BLK_TC_READ | BLK_TC_WRITE))

/*
 * Basic trace actions
 */
enum {
	__BLK_TA_QUEUE = 1,		/* queued */
	__BLK_TA_BACKMERGE,		/* back merged to existing rq */
	__BLK_TA_FRONTMERGE,		/* front merge to existing rq */
	__BLK_TA_GETRQ,			/* allocated new request */
	__BLK_TA_SLEEPRQ,		/* sleeping on rq allocation */
	__BLK_TA_REQUEUE,		/* request requeued */
	__BLK_TA_ISSUE,			/* sent to driver */
	__BLK_TA_COMPLETE,		/* completed by driver */
	__BLK_TA_PLUG,			/* queue was plugged */
	__BLK_TA_UNPLUG_IO,		/* queue was unplugged by io */
	__BLK_TA_UNPLUG_TIMER,		/* queue was unplugged by timer */
	__BLK_TA_INSERT,		/* insert request */
	__BLK_TA_SPLIT,			/* bio was split */
	__BLK_TA_BOUNCE,		/* bio was bounced */
	__BLK_TA_REMAP,			/* bio was remapped */
	__BLK_TA_ABORT,			/* request aborted */
	__BLK_TA_DRV_DATA,		/* binary driver data */
};

/*
 * Notify events.
 */
enum blktrace_notify {
	__BLK_TN_PROCESS = 0,		/* establish pid/name mapping */
	__BLK_TN_TIMESTAMP,		/* include system clock */
	__BLK_TN_MESSAGE,               /* Character string message */
};

/*
 * Trace actions in full. Additionally, read or write is masked
 */
#define BLK_TA_QUEUE		(__BLK_TA_QUEUE | BLK_TC_ACT(BLK_TC_QUEUE))
#define BLK_TA_BACKMERGE	(__BLK_TA_BACKMERGE | BLK_TC_ACT(BLK_TC_QUEUE))
#define BLK_TA_FRONTMERGE	(__BLK_TA_FRONTMERGE | BLK_TC_ACT(BLK_TC_QUEUE))
#define	BLK_TA_GETRQ		(__BLK_TA_GETRQ | BLK_TC_ACT(BLK_TC_QUEUE))
#define	BLK_TA_SLEEPRQ		(__BLK_TA_SLEEPRQ | BLK_TC_ACT(BLK_TC_QUEUE))
#define	BLK_TA_REQUEUE		(__BLK_TA_REQUEUE | BLK_TC_ACT(BLK_TC_REQUEUE))
#define BLK_TA_ISSUE		(__BLK_TA_ISSUE | BLK_TC_ACT(BLK_TC_ISSUE))
#define BLK_TA_COMPLETE		(__BLK_TA_COMPLETE| BLK_TC_ACT(BLK_TC_COMPLETE))
#define BLK_TA_PLUG		(__BLK_TA_PLUG | BLK_TC_ACT(BLK_TC_QUEUE))
#define BLK_TA_UNPLUG_IO	(__BLK_TA_UNPLUG_IO | BLK_TC_ACT(BLK_TC_QUEUE))
#define BLK_TA_UNPLUG_TIMER	(__BLK_TA_UNPLUG_TIMER | BLK_TC_ACT(BLK_TC_QUEUE))
#define BLK_TA_INSERT		(__BLK_TA_INSERT | BLK_TC_ACT(BLK_TC_QUEUE))
#define BLK_TA_SPLIT		(__BLK_TA_SPLIT)
#define BLK_TA_BOUNCE		(__BLK_TA_BOUNCE)
#define BLK_TA_REMAP		(__BLK_TA_REMAP | BLK_TC_ACT(BLK_TC_QUEUE))
#define BLK_TA_ABORT		(__BLK_TA_ABORT | BLK_TC_ACT(BLK_TC_QUEUE))
#define BLK_TA_DRV_DATA		(__BLK_TA_DRV_DATA | BLK_TC_ACT(BLK_TC_DRV_DATA))

#define BLK_TN_PROCESS		(__BLK_TN_PROCESS | BLK_TC_ACT(BLK_TC_NOTIFY))
#define BLK_TN_TIMESTAMP	(__BLK_TN_TIMESTAMP | BLK_TC_ACT(BLK_TC_NOTIFY))
#define BLK_TN_MESSAGE		(__BLK_TN_MESSAGE | BLK_TC_ACT(BLK_TC_NOTIFY))

#define BLK_IO_TRACE_MAGIC	0x65617400
#define BLK_IO_TRACE_VERSION	0x07
/*
 * The trace itself
 */
struct blk_io_trace {
	__u32 magic;		/* MAGIC << 8 | version */
	__u32 sequence;		/* event number */
	__u64 time;		/* in nanoseconds */
	__u64 sector;		/* disk offset */
	__u32 bytes;		/* transfer length */
	__u32 action;		/* what happened */
	__u32 pid;		/* who did it */
	__u32 device;		/* device identifier (dev_t) */
	__u32 cpu;		/* on what cpu did it happen */
	__u16 error;		/* completion error */
	__u16 pdu_len;		/* length of data after this trace */
};

struct pending_io {
	/* sector offset of this IO */
	u64 sector;

	/* time this IO was dispatched */
	u64 dispatch_time;
	/* time this IO was finished */
	u64 completion_time;
	struct list_head hash_list;
};

#define MINORBITS 20
#define MINORMASK ((1 << MINORBITS) - 1)
#define SECONDS(x)              ((unsigned long long)(x) / 1000000000)
#define NANO_SECONDS(x)         ((unsigned long long)(x) % 1000000000)
#define DOUBLE_TO_NANO_ULL(d)   ((unsigned long long)((d) * 1000000000))
#define CHECK_MAGIC(t)          (((t)->magic & 0xffffff00) == BLK_IO_TRACE_MAGIC)

void init_io_hash_table(void)
{
	int i;
	struct list_head *head;

	for (i = 0; i < IO_HASH_TABLE_SIZE; i++) {
		head = io_hash_table + i;
		INIT_LIST_HEAD(head);
	}
}

/* taken from the kernel hash.h */
static inline u64 hash_sector(u64 val)
{
	u64 hash = val;

	/*  Sigh, gcc can't optimise this alone like it does for 32 bits. */
	u64 n = hash;
	n <<= 18;
	hash -= n;
	n <<= 33;
	hash -= n;
	n <<= 3;
	hash += n;
	n <<= 3;
	hash -= n;
	n <<= 4;
	hash += n;
	n <<= 2;
	hash += n;

	/* High bits are more random, so use them. */
	return hash >> (64 - IO_HASH_TABLE_BITS);
}

static int hash_table_insert(struct pending_io *ins_pio)
{
	u64 sector = ins_pio->sector;
	int slot = hash_sector(sector);
	struct list_head *head;
	struct pending_io *pio;

	head = io_hash_table + slot;
	list_for_each_entry(pio, head, hash_list) {
		if (pio->sector == sector)
			return -EEXIST;
	}
	list_add_tail(&ins_pio->hash_list, head);
	return 0;
}

static struct pending_io *hash_table_search(u64 sector)
{
	int slot = hash_sector(sector);
	struct list_head *head;
	struct pending_io *pio;

	head = io_hash_table + slot;
	list_for_each_entry(pio, head, hash_list) {
		if (pio->sector == sector)
			return pio;
	}
	return NULL;
}

static int hash_dispatched_io(struct blk_io_trace *io)
{
	struct pending_io *pio;
	int ret;

	pio = calloc(1, sizeof(*pio));
	pio->sector = io->sector;
	pio->dispatch_time = io->time;

	ret = hash_table_insert(pio);
	if (ret == -EEXIST) {
		/* crud, the IO isn't here */
		free(pio);
	}
	return ret;
}

static struct pending_io *hash_completed_io(struct blk_io_trace *io)
{
	struct pending_io *pio;

	pio = hash_table_search(io->sector);

	if (!pio)
		return NULL;
	return pio;
}

static void handle_notify(struct trace *trace)
{
	struct blk_io_trace *io = trace->io;
	void *payload = (char *)io + sizeof(*io);
	u32 two32[2];


	if (io->action != BLK_TN_TIMESTAMP)
		return;

	if (io->pdu_len != sizeof(two32))
		return;

	memcpy(two32, payload, sizeof(two32));
	trace->start_timestamp = io->time;
	trace->abs_start_time.tv_sec = two32[0];
	trace->abs_start_time.tv_nsec = two32[1];
	if (trace->abs_start_time.tv_nsec < 0) {
		trace->abs_start_time.tv_sec--;
		trace->abs_start_time.tv_nsec += 1000000000;
	}
}

int next_record(struct trace *trace)
{
	int skip = trace->io->pdu_len;
	u64 offset;

	trace->cur += sizeof(*trace->io) + skip;
	offset = trace->cur - trace->start;
	if (offset >= trace->len)
		return 1;

	trace->io = (struct blk_io_trace *)trace->cur;
	return 0;
}

void first_record(struct trace *trace)
{
	trace->cur = trace->start;
	trace->io = (struct blk_io_trace *)trace->cur;
}

u64 find_last_time(struct trace *trace)
{
	char *p = trace->start + trace->len;
	struct blk_io_trace *test;
	int search_len = 0;
	u64 found = 0;

	if (trace->len < sizeof(*trace->io))
		return 0;
	p -= sizeof(*trace->io);
	while (p >= trace->start) {
		test = (struct blk_io_trace *)p;
		if (CHECK_MAGIC(test) &&
		   !(test->action & BLK_TC_ACT(BLK_TC_NOTIFY))) {
			u64 offset = p - trace->start;
			if (offset + sizeof(*test) + test->pdu_len == trace->len) {
				return test->time;
			}
		}
		p--;
		search_len++;
		if (search_len > 8192) {
			break;
		}
	}

	/* searching backwards didn't work out, we'll have to scan the file */
	first_record(trace);
	while (1) {
		if (!(trace->io->action & BLK_TC_ACT(BLK_TC_NOTIFY)))
			found = trace->io->time;
		if (next_record(trace))
			break;
	}
	first_record(trace);
	return found;
}

u64 find_highest_offset(struct trace *trace)
{
	u64 found = 0;
	u64 max = 0;
	first_record(trace);
	while (1) {
		if (!(trace->io->action & BLK_TC_ACT(BLK_TC_NOTIFY))) {
			found = trace->io->sector << 9;
			found += trace->io->bytes;

			if (max < found) {
				max = found;
			}
		}
		if (next_record(trace))
			break;
	}
	first_record(trace);
	return max;
}

int filter_outliers(struct trace *trace, u64 max_offset,
		    u64 *yzoom_min, u64 *yzoom_max)
{
	int hits[11];
	u64 max_per_bucket[11];
	u64 bytes_per_bucket = max_offset / 10;
	int slot;
	int fat_count = 0;

	memset(hits, 0, sizeof(int) * 11);
	memset(max_per_bucket, 0, sizeof(u64) * 11);
	first_record(trace);
	while (1) {
		if (!(trace->io->action & BLK_TC_ACT(BLK_TC_NOTIFY))) {
			u64 top = (trace->io->sector << 9) + trace->io->bytes;
			slot = (int)(top / bytes_per_bucket);
			hits[slot]++;
			if (top > max_per_bucket[slot])
				max_per_bucket[slot] = top;
		}
		if (next_record(trace))
			break;
	}
	first_record(trace);
	for (slot = 0; slot < 11; slot++) {
		if (hits[slot] > fat_count) {
			fat_count = hits[slot];
		}
	}

	*yzoom_max = max_offset;
	for (slot = 10; slot >= 0; slot--) {
		double d = hits[slot];

		if (d >= (double)fat_count * .05) {
			*yzoom_max = max_per_bucket[slot];
			break;
		}
	}

	*yzoom_min = 0;
	for (slot = 0; slot < 10; slot++) {
		double d = hits[slot];

		if (d >= (double)fat_count * .05) {
			*yzoom_min = slot * bytes_per_bucket;
			break;
		}
	}
	return 0;
}

static char *find_trace_file(char *filename)
{
	int ret;
	struct stat st;
	char line[1024];
	char *dot;
	char *try;

	ret = stat(filename, &st);
	if (ret == 0)
		return strdup(filename);

	snprintf(line, 1024, "%s.%s", filename, "dump");
	ret = stat(filename, &st);
	if (ret == 0)
		return strdup(line);

	try = strdup(filename);
	dot = strrchr(try, '.');
	if (!dot || strcmp(".dump", dot) != 0) {
		if (dot)
			*dot = '\0';
		snprintf(line, 1024, "%s%s", try, ".blktrace.0");
		ret = stat(line, &st);
		if (ret == 0) {
			blktrace_to_dump(try);
			snprintf(line, 1024, "%s.%s", try, "dump");
			ret = stat(line, &st);
			if (ret == 0) {
				free(try);
				return strdup(line);
			}
		}
	}
	free(try);
	return NULL;
}
struct trace *open_trace(char *filename)
{
	int fd;
	char *p;
	struct stat st;
	int ret;
	struct trace *trace;
	char *found_filename;

	trace = calloc(1, sizeof(*trace));
	if (!trace) {
		fprintf(stderr, "unable to allocate memory for trace\n");
		return NULL;
	}

	found_filename = find_trace_file(filename);
	if (!found_filename) {
		fprintf(stderr, "Unable to find trace file %s\n", filename);
		goto fail;
	}
	free(filename);
	filename = found_filename;

	fd = open(filename, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "Unable to open trace file %s err %s\n", filename, strerror(errno));
		goto fail;
	}
	ret = fstat(fd, &st);
	if (ret < 0) {
		fprintf(stderr, "stat failed on %s err %s\n", filename, strerror(errno));
		goto fail_fd;
	}
	p = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (p == MAP_FAILED) {
		fprintf(stderr, "Unable to mmap trace file %s, err %s\n", filename, strerror(errno));
		goto fail_fd;
	}
	trace->fd = fd;
	trace->len = st.st_size;
	trace->start = p;
	trace->cur = p;
	trace->io = (struct blk_io_trace *)p;
	return trace;

fail_fd:
	close(fd);
fail:
	free(trace);
	return NULL;
}
static inline int tput_event(struct trace *trace)
{
	if (trace->found_completion)
		return __BLK_TA_COMPLETE;
	if (trace->found_issue)
		return __BLK_TA_ISSUE;
	if (trace->found_queue)
		return __BLK_TA_QUEUE;

	return __BLK_TA_COMPLETE;
}

static inline int io_event(struct trace *trace)
{
	if (trace->found_queue)
		return __BLK_TA_QUEUE;
	if (trace->found_issue)
		return __BLK_TA_ISSUE;
	if (trace->found_completion)
		return __BLK_TA_COMPLETE;

	return __BLK_TA_COMPLETE;
}

void add_tput(struct trace *trace, struct graph_line_data *gld)
{
	struct blk_io_trace *io = trace->io;
	int action = io->action & 0xffff;
	int seconds;

	if (io->action & BLK_TC_ACT(BLK_TC_NOTIFY))
		return;

	if (action != tput_event(trace))
		return;

	seconds = SECONDS(io->time);
	if (seconds > gld->seconds) {
		fprintf(stderr, "Bad record %d %d %d\n", seconds, gld->seconds, action);
		abort();
	}

	gld->data[seconds].sum += io->bytes;
	gld->data[seconds].count = 1;
	if (gld->data[seconds].sum > gld->max)
		gld->max = gld->data[seconds].sum;
}

void add_io(struct trace *trace, struct graph_dot_data *gdd_writes,
	    struct graph_dot_data *gdd_reads)
{
	struct blk_io_trace *io = trace->io;
	int action = io->action & 0xffff;
	u64 offset;

	if (io->action & BLK_TC_ACT(BLK_TC_NOTIFY))
		return;

	if (action != io_event(trace))
		return;

	offset = io->sector << 9;

	if (BLK_DATADIR(io->action) & BLK_TC_READ)
		set_gdd_bit(gdd_reads, offset, io->bytes, io->time);
	else if (BLK_DATADIR(io->action) & BLK_TC_WRITE)
		set_gdd_bit(gdd_writes, offset, io->bytes, io->time);
}

void add_pending_io(struct trace *trace, struct graph_line_data *gld)
{
	int ret;
	int seconds;
	struct blk_io_trace *io = trace->io;
	int action = io->action & 0xffff;
	double avg;

	if (io->action & BLK_TC_ACT(BLK_TC_NOTIFY))
		return;

	if (action != __BLK_TA_ISSUE)
		return;

	seconds = SECONDS(io->time);
	if (seconds > gld->seconds) {
		fprintf(stderr, "Bad record %d %d\n", seconds, gld->seconds);
		abort();
	}

	ret = hash_dispatched_io(trace->io);
	if (ret)
		return;

	ios_in_flight++;

	gld->data[seconds].sum += ios_in_flight;
	gld->data[seconds].count++;

	avg = (double)gld->data[seconds].sum / gld->data[seconds].count;
	if (gld->max < (u64)avg) {
		gld->max = avg;
	}
}

void add_completed_io(struct trace *trace,
		      struct graph_line_data *latency_gld)
{
	struct blk_io_trace *io = trace->io;
	int seconds;
	int action = io->action & 0xffff;
	struct pending_io *pio;
	double avg;
	u64 latency;

	if (io->action & BLK_TC_ACT(BLK_TC_NOTIFY))
		return;

	if (action != __BLK_TA_COMPLETE)
		return;

	seconds = SECONDS(io->time);

	pio = hash_completed_io(trace->io);
	if (!pio)
		return;

	if (ios_in_flight > 0)
		ios_in_flight--;
	if (io->time >= pio->dispatch_time) {
		latency = io->time - pio->dispatch_time;
		latency_gld->data[seconds].sum += latency;
		latency_gld->data[seconds].count++;
	}

	list_del(&pio->hash_list);
	free(pio);

	avg = (double)latency_gld->data[seconds].sum /
		latency_gld->data[seconds].count;
	if (latency_gld->max < (u64)avg) {
		latency_gld->max = avg;
	}
}

void add_iop(struct trace *trace, struct graph_line_data *gld)
{
	struct blk_io_trace *io = trace->io;
	int action = io->action & 0xffff;
	int seconds;

	if (io->action & BLK_TC_ACT(BLK_TC_NOTIFY))
		return;

	/* iops and tput use the same events */
	if (action != tput_event(trace))
		return;

	seconds = SECONDS(io->time);
	if (seconds > gld->seconds) {
		fprintf(stderr, "Bad record %d %d\n", seconds, gld->seconds);
		abort();
	}

	gld->data[seconds].sum += 1;
	gld->data[seconds].count = 1;
	if (gld->data[seconds].sum > gld->max)
		gld->max = gld->data[seconds].sum;
}

void check_record(struct trace *trace)
{
	struct blk_io_trace *io = trace->io;
	int action = io->action & 0xffff;

	if (!(io->action & BLK_TC_ACT(BLK_TC_NOTIFY))) {
		switch (action) {
		case __BLK_TA_COMPLETE:
			trace->found_completion = 1;
			break;
		case __BLK_TA_ISSUE:
			trace->found_issue = 1;
			break;
		case __BLK_TA_QUEUE:
			trace->found_queue = 1;
			break;
		};
	}
	handle_notify(trace);
}
