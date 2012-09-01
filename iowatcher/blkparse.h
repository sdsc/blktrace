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
#ifndef __IOWATCH_BLKPARSE__
#define __IOWATCH_BLKPARSE__
#define MINORBITS 20
#define MINORMASK ((1 << MINORBITS) - 1)
#define SECONDS(x)              ((unsigned long long)(x) / 1000000000)
#define NANO_SECONDS(x)         ((unsigned long long)(x) % 1000000000)
#define DOUBLE_TO_NANO_ULL(d)   ((unsigned long long)((d) * 1000000000))
#define CHECK_MAGIC(t)          (((t)->magic & 0xffffff00) == BLK_IO_TRACE_MAGIC)

struct trace {
	int fd;
	u64 len;
	char *start;
	char *cur;
	struct blk_io_trace *io;
	u64 start_timestamp;
	struct timespec abs_start_time;

	/*
	 * flags for the things we find in the stream
	 * we prefer different events for different things
	 */
	int found_issue;
	int found_completion;
	int found_queue;

	char *mpstat_start;
	char *mpstat_cur;
	u64 mpstat_len;
	int mpstat_fd;
	int mpstat_seconds;
	int mpstat_num_cpus;
};

static inline unsigned int MAJOR(unsigned int dev)
{
	return dev >> MINORBITS;
}

static inline unsigned int MINOR(unsigned int dev)
{
	return dev & MINORMASK;
}

void init_io_hash_table(void);
struct trace *open_trace(char *filename);
u64 find_last_time(struct trace *trace);
void find_extreme_offsets(struct trace *trace, u64 *min_ret, u64 *max_ret,
			  u64 *max_bank_ret, u64 *max_offset_ret);
int filter_outliers(struct trace *trace, u64 min_offset, u64 max_offset,
		    u64 *yzoom_min, u64 *yzoom_max);
void add_iop(struct trace *trace, struct graph_line_data *gld);
void check_record(struct trace *trace);
void add_completed_io(struct trace *trace,
		      struct graph_line_data *latency_gld);
void add_io(struct trace *trace, struct graph_dot_data *gdd_writes,
	    struct graph_dot_data *gdd_reads);
void add_tput(struct trace *trace, struct graph_line_data *gld);
void add_pending_io(struct trace *trace, struct graph_line_data *gld);
int next_record(struct trace *trace);
void first_record(struct trace *trace);
#endif
