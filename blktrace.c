/*
 * block queue tracing application
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
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <locale.h>
#include <signal.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/param.h>
#include <sys/statfs.h>
#include <sys/poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <sched.h>
#include <ctype.h>
#include <getopt.h>
#include <errno.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/sendfile.h>

#include "blktrace.h"
#include "barrier.h"

static char blktrace_version[] = "0.99.1";

/*
 * You may want to increase this even more, if you are logging at a high
 * rate and see skipped/missed events
 */
#define BUF_SIZE	(512 * 1024)
#define BUF_NR		(4)

#define OFILE_BUF	(128 * 1024)

#define DEBUGFS_TYPE	0x64626720

#define S_OPTS	"d:a:A:r:o:kw:Vb:n:D:lh:p:s"
static struct option l_opts[] = {
	{
		.name = "dev",
		.has_arg = required_argument,
		.flag = NULL,
		.val = 'd'
	},
	{
		.name = "act-mask",
		.has_arg = required_argument,
		.flag = NULL,
		.val = 'a'
	},
	{
		.name = "set-mask",
		.has_arg = required_argument,
		.flag = NULL,
		.val = 'A'
	},
	{
		.name = "relay",
		.has_arg = required_argument,
		.flag = NULL,
		.val = 'r'
	},
	{
		.name = "output",
		.has_arg = required_argument,
		.flag = NULL,
		.val = 'o'
	},
	{
		.name = "kill",
		.has_arg = no_argument,
		.flag = NULL,
		.val = 'k'
	},
	{
		.name = "stopwatch",
		.has_arg = required_argument,
		.flag = NULL,
		.val = 'w'
	},
	{
		.name = "version",
		.has_arg = no_argument,
		.flag = NULL,
		.val = 'V'
	},
	{
		.name = "buffer-size",
		.has_arg = required_argument,
		.flag = NULL,
		.val = 'b'
	},
	{
		.name = "num-sub-buffers",
		.has_arg = required_argument,
		.flag = NULL,
		.val = 'n'
	},
	{
		.name = "output-dir",
		.has_arg = required_argument,
		.flag = NULL,
		.val = 'D'
	},
	{
		.name = "listen",
		.has_arg = no_argument,
		.flag = NULL,
		.val = 'l'
	},
	{
		.name = "host",
		.has_arg = required_argument,
		.flag = NULL,
		.val = 'h'
	},
	{
		.name = "port",
		.has_arg = required_argument,
		.flag = NULL,
		.val = 'p'
	},
	{
		.name = "no-sendfile",
		.has_arg = no_argument,
		.flag = NULL,
		.val = 's'
	},
	{
		.name = NULL,
	}
};

struct tip_subbuf {
	void *buf;
	unsigned int len;
	unsigned int max_len;
};

#define FIFO_SIZE	(1024)	/* should be plenty big! */
#define CL_SIZE		(128)	/* cache line, any bigger? */

struct tip_subbuf_fifo {
	int tail __attribute__((aligned(CL_SIZE)));
	int head __attribute__((aligned(CL_SIZE)));
	struct tip_subbuf *q[FIFO_SIZE];
};

struct thread_information {
	int cpu;
	pthread_t thread;

	int fd;
	void *fd_buf;
	char fn[MAXPATHLEN + 64];

	FILE *ofile;
	char *ofile_buffer;
	off_t ofile_offset;
	int ofile_stdout;
	int ofile_mmap;

	int (*get_subbuf)(struct thread_information *, unsigned int);
	int (*flush_subbuf)(struct thread_information *, struct tip_subbuf *);
	int (*read_data)(struct thread_information *, void *, unsigned int);

	unsigned long events_processed;
	unsigned long long data_read;
	unsigned long long data_queued;
	struct device_information *device;

	int exited;

	/*
	 * piped fifo buffers
	 */
	struct tip_subbuf_fifo fifo;
	struct tip_subbuf *leftover_ts;

	/*
	 * mmap controlled output files
	 */
	unsigned long long fs_size;
	unsigned long long fs_max_size;
	unsigned long fs_off;
	void *fs_buf;
	unsigned long fs_buf_len;
};

struct device_information {
	int fd;
	char *path;
	char buts_name[32];
	volatile int trace_started;
	unsigned long drop_count;
	struct thread_information *threads;
	struct net_connection *nc;
};

static int ncpus;
static struct thread_information *thread_information;
static int ndevs;
static struct device_information *device_information;

/* command line option globals */
static char *debugfs_path;
static char *output_name;
static char *output_dir;
static int act_mask = ~0U;
static int kill_running_trace;
static unsigned long buf_size = BUF_SIZE;
static unsigned long buf_nr = BUF_NR;
static unsigned int page_size;

#define is_done()	(*(volatile int *)(&done))
static volatile int done;

#define is_trace_stopped()	(*(volatile int *)(&trace_stopped))
static volatile int trace_stopped;

#define is_stat_shown()	(*(volatile int *)(&stat_shown))
static volatile int stat_shown;

int data_is_native = -1;

static void exit_trace(int status);

#define dip_tracing(dip)	(*(volatile int *)(&(dip)->trace_started))
#define dip_set_tracing(dip, v)	((dip)->trace_started = (v))

#define __for_each_dip(__d, __di, __e, __i)	\
	for (__i = 0, __d = __di; __i < __e; __i++, __d++)

#define for_each_dip(__d, __i)		\
	__for_each_dip(__d, device_information, ndevs, __i)
#define for_each_nc_dip(__nc, __d, __i)		\
	__for_each_dip(__d, (__nc)->device_information, (__nc)->ndevs, __i)

#define __for_each_tip(__d, __t, __ncpus, __j)	\
	for (__j = 0, __t = (__d)->threads; __j < __ncpus; __j++, __t++)
#define for_each_tip(__d, __t, __j)	\
	__for_each_tip(__d, __t, ncpus, __j)

/*
 * networking stuff follows. we include a magic number so we know whether
 * to endianness convert or not
 */
struct blktrace_net_hdr {
	u32 magic;		/* same as trace magic */
	char buts_name[32];	/* trace name */
	u32 cpu;		/* for which cpu */
	u32 max_cpus;
	u32 len;		/* length of following trace data */
};

#define TRACE_NET_PORT		(8462)

enum {
	Net_none = 0,
	Net_server,
	Net_client,
};

/*
 * network cmd line params
 */
static char hostname[MAXHOSTNAMELEN];
static int net_port = TRACE_NET_PORT;
static int net_mode = 0;
static int net_use_sendfile = 1;

struct net_connection {
	int in_fd;
	time_t connect_time;
	struct in_addr cl_in_addr;
	struct device_information *device_information;
	int ndevs;
	int ncpus;
	int connection_index;
};

#define NET_MAX_CONNECTIONS	(1024)
static struct net_connection net_connections[NET_MAX_CONNECTIONS];
static int net_connects;
static int net_out_fd = -1;

static void handle_sigint(__attribute__((__unused__)) int sig)
{
	struct device_information *dip;
	int i;

	/*
	 * stop trace so we can reap currently produced data
	 */
	for_each_dip(dip, i) {
		if (dip->fd == -1)
			continue;
		if (ioctl(dip->fd, BLKTRACESTOP) < 0)
			perror("BLKTRACESTOP");
	}

	done = 1;
}

static int get_dropped_count(const char *buts_name)
{
	int fd;
	char tmp[MAXPATHLEN + 64];

	snprintf(tmp, sizeof(tmp), "%s/block/%s/dropped",
		 debugfs_path, buts_name);

	fd = open(tmp, O_RDONLY);
	if (fd < 0) {
		/*
		 * this may be ok, if the kernel doesn't support dropped counts
		 */
		if (errno == ENOENT)
			return 0;

		fprintf(stderr, "Couldn't open dropped file %s\n", tmp);
		return -1;
	}

	if (read(fd, tmp, sizeof(tmp)) < 0) {
		perror(tmp);
		close(fd);
		return -1;
	}

	close(fd);

	return atoi(tmp);
}

static int start_trace(struct device_information *dip)
{
	struct blk_user_trace_setup buts;

	memset(&buts, 0, sizeof(buts));
	buts.buf_size = buf_size;
	buts.buf_nr = buf_nr;
	buts.act_mask = act_mask;

	if (ioctl(dip->fd, BLKTRACESETUP, &buts) < 0) {
		perror("BLKTRACESETUP");
		return 1;
	}

	if (ioctl(dip->fd, BLKTRACESTART) < 0) {
		perror("BLKTRACESTART");
		return 1;
	}

	memcpy(dip->buts_name, buts.name, sizeof(dip->buts_name));
	dip_set_tracing(dip, 1);
	return 0;
}

static void stop_trace(struct device_information *dip)
{
	if (dip_tracing(dip) || kill_running_trace) {
		dip_set_tracing(dip, 0);

		/*
		 * should be stopped, just don't complain if it isn't
		 */
		ioctl(dip->fd, BLKTRACESTOP);

		if (ioctl(dip->fd, BLKTRACETEARDOWN) < 0)
			perror("BLKTRACETEARDOWN");

		close(dip->fd);
		dip->fd = -1;
	}
}

static void stop_all_traces(void)
{
	struct device_information *dip;
	int i;

	for_each_dip(dip, i) {
		dip->drop_count = get_dropped_count(dip->buts_name);
		stop_trace(dip);
	}
}

static void wait_for_data(struct thread_information *tip, int events)
{
	struct pollfd pfd = { .fd = tip->fd, .events = events };

	do {
		if (poll(&pfd, 1, 100) < 0) {
			perror("poll");
			break;
		}
		if (pfd.revents & events)
			break;
		if (tip->ofile_stdout)
			break;
	} while (!is_done());
}

static int read_data_file(struct thread_information *tip, void *buf,
			  unsigned int len)
{
	int ret = 0;

	do {
		wait_for_data(tip, POLLIN);

		ret = read(tip->fd, buf, len);
		if (!ret)
			continue;
		else if (ret > 0)
			return ret;
		else {
			if (errno != EAGAIN) {
				perror(tip->fn);
				fprintf(stderr,"Thread %d failed read of %s\n",
					tip->cpu, tip->fn);
				break;
			}
			continue;
		}
	} while (!is_done());

	return ret;

}

static int read_data_net(struct thread_information *tip, void *buf,
			 unsigned int len)
{
	struct net_connection *nc = tip->device->nc;
	unsigned int bytes_left = len;
	int ret = 0;

	do {
		ret = recv(nc->in_fd, buf, bytes_left, MSG_WAITALL);

		if (!ret)
			continue;
		else if (ret < 0) {
			if (errno != EAGAIN) {
				perror(tip->fn);
				fprintf(stderr, "server: failed read\n");
				return 0;
			}
			continue;
		} else {
			buf += ret;
			bytes_left -= ret;
		}
	} while (!is_done() && bytes_left);

	return len - bytes_left;
}

static inline struct tip_subbuf *
subbuf_fifo_dequeue(struct thread_information *tip)
{
	const int head = tip->fifo.head;
	const int next = (head + 1) & (FIFO_SIZE - 1);

	if (head != tip->fifo.tail) {
		struct tip_subbuf *ts = tip->fifo.q[head];

		store_barrier();
		tip->fifo.head = next;
		return ts;
	}

	return NULL;
}

static inline int subbuf_fifo_queue(struct thread_information *tip,
				    struct tip_subbuf *ts)
{
	const int tail = tip->fifo.tail;
	const int next = (tail + 1) & (FIFO_SIZE - 1);

	if (next != tip->fifo.head) {
		tip->fifo.q[tail] = ts;
		store_barrier();
		tip->fifo.tail = next;
		return 0;
	}

	fprintf(stderr, "fifo too small!\n");
	return 1;
}

/*
 * For file output, truncate and mmap the file appropriately
 */
static int mmap_subbuf(struct thread_information *tip, unsigned int maxlen)
{
	int ofd = fileno(tip->ofile);
	int ret;

	/*
	 * extend file, if we have to. use chunks of 16 subbuffers.
	 */
	if (tip->fs_off + buf_size > tip->fs_buf_len) {
		if (tip->fs_buf) {
			munlock(tip->fs_buf, tip->fs_buf_len);
			munmap(tip->fs_buf, tip->fs_buf_len);
			tip->fs_buf = NULL;
		}

		tip->fs_off = tip->fs_size & (page_size - 1);
		tip->fs_buf_len = (16 * buf_size) - tip->fs_off;
		tip->fs_max_size += tip->fs_buf_len;

		if (ftruncate(ofd, tip->fs_max_size) < 0) {
			perror("ftruncate");
			return -1;
		}

		tip->fs_buf = mmap(NULL, tip->fs_buf_len, PROT_WRITE,
				   MAP_SHARED, ofd, tip->fs_size - tip->fs_off);
		if (tip->fs_buf == MAP_FAILED) {
			perror("mmap");
			return -1;
		}
		mlock(tip->fs_buf, tip->fs_buf_len);
	}

	ret = tip->read_data(tip, tip->fs_buf + tip->fs_off, maxlen);
	if (ret >= 0) {
		tip->data_read += ret;
		tip->fs_size += ret;
		tip->fs_off += ret;
		return 0;
	}

	return -1;
}

/*
 * Use the copy approach for pipes and network
 */
static int get_subbuf(struct thread_information *tip, unsigned int maxlen)
{
	struct tip_subbuf *ts = malloc(sizeof(*ts));
	int ret;

	ts->buf = malloc(buf_size);
	ts->max_len = maxlen;

	ret = tip->read_data(tip, ts->buf, ts->max_len);
	if (ret > 0) {
		ts->len = ret;
		tip->data_read += ret;
		if (subbuf_fifo_queue(tip, ts))
			return -1;
	}

	return ret;
}

static int get_subbuf_sendfile(struct thread_information *tip,
			       unsigned int maxlen)
{
	struct tip_subbuf *ts;
	struct stat sb;
	unsigned int ready;

	wait_for_data(tip, POLLMSG);

	/*
	 * hack to get last data out, we can't use sendfile for that
	 */
	if (is_done())
		return get_subbuf(tip, maxlen);

	if (fstat(tip->fd, &sb) < 0) {
		perror("trace stat");
		return -1;
	}
	ready = sb.st_size - tip->data_queued;
	if (!ready) {
		usleep(1000);
		return 0;
	}

	ts = malloc(sizeof(*ts));
	ts->buf = NULL;
	ts->max_len = 0;
	ts->len = ready;
	tip->data_queued += ready;

	if (subbuf_fifo_queue(tip, ts))
		return -1;

	return ready;
}

static void close_thread(struct thread_information *tip)
{
	if (tip->fd != -1)
		close(tip->fd);
	if (tip->ofile)
		fclose(tip->ofile);
	if (tip->ofile_buffer)
		free(tip->ofile_buffer);
	if (tip->fd_buf)
		free(tip->fd_buf);

	tip->fd = -1;
	tip->ofile = NULL;
	tip->ofile_buffer = NULL;
	tip->fd_buf = NULL;
}

static void tip_ftrunc_final(struct thread_information *tip)
{
	/*
	 * truncate to right size and cleanup mmap
	 */
	if (tip->ofile_mmap && tip->ofile) {
		int ofd = fileno(tip->ofile);

		if (tip->fs_buf)
			munmap(tip->fs_buf, tip->fs_buf_len);

		ftruncate(ofd, tip->fs_size);
	}
}

static void *thread_main(void *arg)
{
	struct thread_information *tip = arg;
	pid_t pid = getpid();
	cpu_set_t cpu_mask;

	CPU_ZERO(&cpu_mask);
	CPU_SET((tip->cpu), &cpu_mask);

	if (sched_setaffinity(pid, sizeof(cpu_mask), &cpu_mask) == -1) {
		perror("sched_setaffinity");
		exit_trace(1);
	}

	snprintf(tip->fn, sizeof(tip->fn), "%s/block/%s/trace%d",
			debugfs_path, tip->device->buts_name, tip->cpu);
	tip->fd = open(tip->fn, O_RDONLY);
	if (tip->fd < 0) {
		perror(tip->fn);
		fprintf(stderr,"Thread %d failed open of %s\n", tip->cpu,
			tip->fn);
		exit_trace(1);
	}

	while (!is_done()) {
		if (tip->get_subbuf(tip, buf_size) < 0)
			break;
	}

	/*
	 * trace is stopped, pull data until we get a short read
	 */
	while (tip->get_subbuf(tip, buf_size) > 0)
		;

	tip_ftrunc_final(tip);
	tip->exited = 1;
	return NULL;
}

static int write_data_net(int fd, void *buf, unsigned int buf_len)
{
	unsigned int bytes_left = buf_len;
	int ret;

	while (bytes_left) {
		ret = send(fd, buf, bytes_left, 0);
		if (ret < 0) {
			perror("send");
			return 1;
		}

		buf += ret;
		bytes_left -= ret;
	}

	return 0;
}

static int net_send_header(struct thread_information *tip, unsigned int len)
{
	struct blktrace_net_hdr hdr;

	hdr.magic = BLK_IO_TRACE_MAGIC;
	strcpy(hdr.buts_name, tip->device->buts_name);
	hdr.cpu = tip->cpu;
	hdr.max_cpus = ncpus;
	hdr.len = len;

	return write_data_net(net_out_fd, &hdr, sizeof(hdr));
}

/*
 * send header with 0 length to signal end-of-run
 */
static void net_client_send_close(void)
{
	struct device_information *dip;
	struct blktrace_net_hdr hdr;
	int i;

	for_each_dip(dip, i) {
		hdr.magic = BLK_IO_TRACE_MAGIC;
		hdr.max_cpus = ncpus;
		hdr.len = 0;
		strcpy(hdr.buts_name, dip->buts_name);
		hdr.cpu = get_dropped_count(dip->buts_name);

		write_data_net(net_out_fd, &hdr, sizeof(hdr));
	}

}

static int flush_subbuf_net(struct thread_information *tip,
			    struct tip_subbuf *ts)
{
	if (net_send_header(tip, ts->len))
		return -1;
	if (write_data_net(net_out_fd, ts->buf, ts->len))
		return -1;

	free(ts->buf);
	free(ts);
	return 1;
}

static int net_sendfile(struct thread_information *tip, struct tip_subbuf *ts)
{
	int ret = sendfile(net_out_fd, tip->fd, NULL, ts->len);

	if (ret < 0) {
		perror("sendfile");
		return 1;
	} else if (ret < (int) ts->len) {
		fprintf(stderr, "short sendfile send (%d of %d)\n", ret, ts->len);
		return 1;
	}

	return 0;
}

static int flush_subbuf_sendfile(struct thread_information *tip,
				 struct tip_subbuf *ts)
{
	int ret = -1;

	/*
	 * currently we cannot use sendfile() on the last bytes read, as they
	 * may not be a full subbuffer. get_subbuf_sendfile() falls back to
	 * the read approach for those, so use send() to ship them out
	 */
	if (ts->buf)
		return flush_subbuf_net(tip, ts);
	
	if (net_send_header(tip, ts->len))
		goto err;
	if (net_sendfile(tip, ts))
		goto err;

	tip->data_read += ts->len;
	tip->ofile_offset += buf_size;
	ret = 1;
err:
	free(ts);
	return ret;
}

static int write_data(struct thread_information *tip, void *buf,
		      unsigned int buf_len)
{
	int ret;

	if (!buf_len)
		return 0;

	while (1) {
		ret = fwrite(buf, buf_len, 1, tip->ofile);
		if (ret == 1)
			break;

		if (ret < 0) {
			perror("write");
			return 1;
		}
	}

	if (tip->ofile_stdout)
		fflush(tip->ofile);

	return 0;
}

static int flush_subbuf_file(struct thread_information *tip,
			     struct tip_subbuf *ts)
{
	unsigned int offset = 0;
	struct blk_io_trace *t;
	int pdu_len, events = 0;

	/*
	 * surplus from last run
	 */
	if (tip->leftover_ts) {
		struct tip_subbuf *prev_ts = tip->leftover_ts;

		if (prev_ts->len + ts->len > prev_ts->max_len) {
			prev_ts->max_len += ts->len;
			prev_ts->buf = realloc(prev_ts->buf, prev_ts->max_len);
		}

		memcpy(prev_ts->buf + prev_ts->len, ts->buf, ts->len);
		prev_ts->len += ts->len;

		free(ts->buf);
		free(ts);

		ts = prev_ts;
		tip->leftover_ts = NULL;
	}

	while (offset + sizeof(*t) <= ts->len) {
		t = ts->buf + offset;

		if (verify_trace(t)) {
			write_data(tip, ts->buf, offset);
			return -1;
		}

		pdu_len = t->pdu_len;

		if (offset + sizeof(*t) + pdu_len > ts->len)
			break;

		offset += sizeof(*t) + pdu_len;
		tip->events_processed++;
		tip->data_read += sizeof(*t) + pdu_len;
		events++;
	}

	if (write_data(tip, ts->buf, offset))
		return -1;

	/*
	 * leftover bytes, save them for next time
	 */
	if (offset != ts->len) {
		tip->leftover_ts = ts;
		ts->len -= offset;
		memmove(ts->buf, ts->buf + offset, ts->len);
	} else {
		free(ts->buf);
		free(ts);
	}

	return events;
}

static int write_tip_events(struct thread_information *tip)
{
	struct tip_subbuf *ts = subbuf_fifo_dequeue(tip);

	if (ts)
		return tip->flush_subbuf(tip, ts);

	return 0;
}

/*
 * scans the tips we know and writes out the subbuffers we accumulate
 */
static void get_and_write_events(void)
{
	struct device_information *dip;
	struct thread_information *tip;
	int i, j, events, ret, tips_running;

	while (!is_done()) {
		events = 0;

		for_each_dip(dip, i) {
			for_each_tip(dip, tip, j) {
				ret = write_tip_events(tip);
				if (ret > 0)
					events += ret;
			}
		}

		if (!events)
			usleep(100000);
	}

	/*
	 * reap stored events
	 */
	do {
		events = 0;
		tips_running = 0;
		for_each_dip(dip, i) {
			for_each_tip(dip, tip, j) {
				ret = write_tip_events(tip);
				if (ret > 0)
					events += ret;
				tips_running += !tip->exited;
			}
		}
		usleep(10);
	} while (events || tips_running);
}

static void wait_for_threads(void)
{
	/*
	 * for piped or network output, poll and fetch data for writeout.
	 * for files, we just wait around for trace threads to exit
	 */
	if ((output_name && !strcmp(output_name, "-")) ||
	    net_mode == Net_client)
		get_and_write_events();
	else {
		struct device_information *dip;
		struct thread_information *tip;
		int i, j, tips_running;

		do {
			tips_running = 0;
			usleep(100000);

			for_each_dip(dip, i)
				for_each_tip(dip, tip, j)
					tips_running += !tip->exited;
		} while (tips_running);
	}

	if (net_mode == Net_client)
		net_client_send_close();
}

static int fill_ofname(struct device_information *dip,
		       struct thread_information *tip, char *dst,
		       char *buts_name)
{
	struct stat sb;
	int len = 0;

	if (output_dir)
		len = sprintf(dst, "%s/", output_dir);
	else
		len = sprintf(dst, "./");

	if (net_mode == Net_server) {
		struct net_connection *nc = dip->nc;

		len += sprintf(dst + len, "%s-", inet_ntoa(nc->cl_in_addr));
		len += strftime(dst + len, 64, "%F-%T/", gmtime(&nc->connect_time));
	}

	if (stat(dst, &sb) < 0) {
		if (errno != ENOENT) {
			perror("stat");
			return 1;
		}
		if (mkdir(dst, 0755) < 0) {
			perror(dst);
			fprintf(stderr, "Can't make output dir\n");
			return 1;
		}
	}

	if (output_name)
		sprintf(dst + len, "%s.blktrace.%d", output_name, tip->cpu);
	else
		sprintf(dst + len, "%s.blktrace.%d", buts_name, tip->cpu);

	return 0;
}

static void fill_ops(struct thread_information *tip)
{
	/*
	 * setup ops
	 */
	if (net_mode == Net_client) {
		if (net_use_sendfile) {
			tip->get_subbuf = get_subbuf_sendfile;
			tip->flush_subbuf = flush_subbuf_sendfile;
		} else {
			tip->get_subbuf = get_subbuf;
			tip->flush_subbuf = flush_subbuf_net;
		}
	} else {
		if (tip->ofile_mmap)
			tip->get_subbuf = mmap_subbuf;
		else
			tip->get_subbuf = get_subbuf;

		tip->flush_subbuf = flush_subbuf_file;
	}
			
	if (net_mode == Net_server)
		tip->read_data = read_data_net;
	else
		tip->read_data = read_data_file;
}

static int tip_open_output(struct device_information *dip,
			   struct thread_information *tip)
{
	int pipeline = output_name && !strcmp(output_name, "-");
	int mode, vbuf_size;
	char op[128];

	if (net_mode == Net_client) {
		tip->ofile = NULL;
		tip->ofile_stdout = 0;
		tip->ofile_mmap = 0;
		goto done;
	} else if (pipeline) {
		tip->ofile = fdopen(STDOUT_FILENO, "w");
		tip->ofile_stdout = 1;
		tip->ofile_mmap = 0;
		mode = _IOLBF;
		vbuf_size = 512;
	} else {
		if (fill_ofname(dip, tip, op, dip->buts_name))
			return 1;
		tip->ofile = fopen(op, "w+");
		tip->ofile_stdout = 0;
		tip->ofile_mmap = 1;
		mode = _IOFBF;
		vbuf_size = OFILE_BUF;
	}

	if (tip->ofile == NULL) {
		perror(op);
		return 1;
	}

	tip->ofile_buffer = malloc(vbuf_size);
	if (setvbuf(tip->ofile, tip->ofile_buffer, mode, vbuf_size)) {
		perror("setvbuf");
		close_thread(tip);
		return 1;
	}

done:
	fill_ops(tip);
	return 0;
}

static int start_threads(struct device_information *dip)
{
	struct thread_information *tip;
	int j;

	for_each_tip(dip, tip, j) {
		tip->cpu = j;
		tip->device = dip;
		tip->events_processed = 0;
		tip->fd = -1;
		memset(&tip->fifo, 0, sizeof(tip->fifo));
		tip->leftover_ts = NULL;

		if (tip_open_output(dip, tip))
			return 1;

		if (pthread_create(&tip->thread, NULL, thread_main, tip)) {
			perror("pthread_create");
			close_thread(tip);
			return 1;
		}
	}

	return 0;
}

static void stop_threads(struct device_information *dip)
{
	struct thread_information *tip;
	unsigned long ret;
	int i;

	for_each_tip(dip, tip, i) {
		(void) pthread_join(tip->thread, (void *) &ret);
		close_thread(tip);
	}
}

static void stop_all_threads(void)
{
	struct device_information *dip;
	int i;

	for_each_dip(dip, i)
		stop_threads(dip);
}

static void stop_all_tracing(void)
{
	struct device_information *dip;
	int i;

	for_each_dip(dip, i)
		stop_trace(dip);
}

static void exit_trace(int status)
{
	if (!is_trace_stopped()) {
		trace_stopped = 1;
		stop_all_threads();
		stop_all_tracing();
	}

	exit(status);
}

static int resize_devices(char *path)
{
	int size = (ndevs + 1) * sizeof(struct device_information);

	device_information = realloc(device_information, size);
	if (!device_information) {
		fprintf(stderr, "Out of memory, device %s (%d)\n", path, size);
		return 1;
	}
	device_information[ndevs].path = path;
	ndevs++;
	return 0;
}

static int open_devices(void)
{
	struct device_information *dip;
	int i;

	for_each_dip(dip, i) {
		dip->fd = open(dip->path, O_RDONLY | O_NONBLOCK);
		if (dip->fd < 0) {
			perror(dip->path);
			return 1;
		}
	}

	return 0;
}

static int start_devices(void)
{
	struct device_information *dip;
	int i, j, size;

	size = ncpus * sizeof(struct thread_information);
	thread_information = malloc(size * ndevs);
	if (!thread_information) {
		fprintf(stderr, "Out of memory, threads (%d)\n", size * ndevs);
		return 1;
	}

	for_each_dip(dip, i) {
		if (start_trace(dip)) {
			close(dip->fd);
			fprintf(stderr, "Failed to start trace on %s\n",
				dip->path);
			break;
		}
	}

	if (i != ndevs) {
		__for_each_dip(dip, device_information, i, j)
			stop_trace(dip);

		return 1;
	}

	for_each_dip(dip, i) {
		dip->threads = thread_information + (i * ncpus);
		if (start_threads(dip)) {
			fprintf(stderr, "Failed to start worker threads\n");
			break;
		}
	}

	if (i != ndevs) {
		__for_each_dip(dip, device_information, i, j)
			stop_threads(dip);
		for_each_dip(dip, i)
			stop_trace(dip);

		return 1;
	}

	return 0;
}

static void show_stats(struct device_information *dips, int ndips, int cpus)
{
	struct device_information *dip;
	struct thread_information *tip;
	unsigned long long events_processed, data_read;
	unsigned long total_drops;
	int i, j, no_stdout = 0;

	if (is_stat_shown())
		return;

	if (output_name && !strcmp(output_name, "-"))
		no_stdout = 1;

	stat_shown = 1;

	total_drops = 0;
	__for_each_dip(dip, dips, ndips, i) {
		if (!no_stdout)
			printf("Device: %s\n", dip->path);
		events_processed = 0;
		data_read = 0;
		__for_each_tip(dip, tip, cpus, j) {
			if (!no_stdout)
				printf("  CPU%3d: %20lu events, %8llu KiB data\n",
			       		tip->cpu, tip->events_processed,
					(tip->data_read + 1023) >> 10);
			events_processed += tip->events_processed;
			data_read += tip->data_read;
		}
		total_drops += dip->drop_count;
		if (!no_stdout)
			printf("  Total:  %20llu events (dropped %lu), %8llu KiB data\n",
					events_processed, dip->drop_count,
					(data_read + 1023) >> 10);
	}

	if (total_drops)
		fprintf(stderr, "You have dropped events, consider using a larger buffer size (-b)\n");
}

static struct device_information *net_get_dip(struct net_connection *nc,
					      char *buts_name)
{
	struct device_information *dip;
	int i;

	for (i = 0; i < nc->ndevs; i++) {
		dip = &nc->device_information[i];

		if (!strcmp(dip->buts_name, buts_name))
			return dip;
	}

	nc->device_information = realloc(nc->device_information, (nc->ndevs + 1) * sizeof(*dip));
	dip = &nc->device_information[nc->ndevs];
	memset(dip, 0, sizeof(*dip));
	dip->fd = -1;
	dip->nc = nc;
	strcpy(dip->buts_name, buts_name);
	dip->path = strdup(buts_name);
	dip->trace_started = 1;
	nc->ndevs++;
	dip->threads = malloc(nc->ncpus * sizeof(struct thread_information));
	memset(dip->threads, 0, nc->ncpus * sizeof(struct thread_information));

	/*
	 * open all files
	 */
	for (i = 0; i < nc->ncpus; i++) {
		struct thread_information *tip = &dip->threads[i];

		tip->cpu = i;
		tip->device = dip;
		tip->fd = -1;

		if (tip_open_output(dip, tip))
			return NULL;
	}

	return dip;
}

static struct thread_information *net_get_tip(struct net_connection *nc,
					      struct blktrace_net_hdr *bnh)
{
	struct device_information *dip;

	nc->ncpus = bnh->max_cpus;
	dip = net_get_dip(nc, bnh->buts_name);
	if (!dip->trace_started) {
		fprintf(stderr, "Events for closed devices %s\n", dip->buts_name);
		return NULL;
	}

	return &dip->threads[bnh->cpu];
}

static int net_get_header(struct net_connection *nc,
			  struct blktrace_net_hdr *bnh)
{
	int fl = fcntl(nc->in_fd, F_GETFL);
	int bytes_left, ret;
	void *p = bnh;

	fcntl(nc->in_fd, F_SETFL, fl | O_NONBLOCK);
	bytes_left = sizeof(*bnh);
	while (bytes_left && !is_done()) {
		ret = recv(nc->in_fd, p, bytes_left, MSG_WAITALL);
		if (ret < 0) {
			if (errno != EAGAIN) {
				perror("recv header");
				return 1;
			}
			usleep(1000);
			continue;
		} else if (!ret) {
			usleep(1000);
			continue;
		} else {
			p += ret;
			bytes_left -= ret;
		}
	}
	fcntl(nc->in_fd, F_SETFL, fl & ~O_NONBLOCK);
	return bytes_left;
}

/*
 * finalize a net client: truncate files, show stats, cleanup, etc
 */
static void net_client_done(struct net_connection *nc)
{
	struct device_information *dip;
	struct thread_information *tip;
	struct net_connection *last_nc;
	int i, j;

	for_each_nc_dip(nc, dip, i)
		__for_each_tip(dip, tip, nc->ncpus, j)
			tip_ftrunc_final(tip);

	show_stats(nc->device_information, nc->ndevs, nc->ncpus);

	/*
	 * cleanup for next run
	 */
	for_each_nc_dip(nc, dip, i) {
		__for_each_tip(dip, tip, nc->ncpus, j) {
			if (tip->ofile)
				fclose(tip->ofile);
		}

		free(dip->threads);
		free(dip->path);
	}

	free(nc->device_information);
	nc->device_information = NULL;
	nc->ncpus = nc->ndevs = 0;

	close(nc->in_fd);
	nc->in_fd = -1;

	net_connects--;

	/*
	 * now put last entry where this one was, a little nasty since we
	 * need to adjust dip->nc as well
	 */
	if (nc->connection_index != net_connects) {
		last_nc = &net_connections[net_connects];
		*nc = *last_nc;
		for_each_nc_dip(nc, dip, i)
			dip->nc = nc;
	}

	stat_shown = 0;
}

/*
 * handle incoming events from a net client
 */
static int net_client_data(struct net_connection *nc)
{
	struct thread_information *tip;
	struct blktrace_net_hdr bnh;

	if (net_get_header(nc, &bnh))
		return 1;

	if (data_is_native == -1 && check_data_endianness(bnh.magic)) {
		fprintf(stderr, "server: received data is bad\n");
		return 1;
	}

	if (!data_is_native) {
		bnh.magic = be32_to_cpu(bnh.magic);
		bnh.cpu = be32_to_cpu(bnh.cpu);
		bnh.len = be32_to_cpu(bnh.len);
	}

	if ((bnh.magic & 0xffffff00) != BLK_IO_TRACE_MAGIC) {
		fprintf(stderr, "server: bad data magic\n");
		return 1;
	}

	/*
	 * len == 0 means that the other end signalled end-of-run
	 */
	if (!bnh.len) {
		/*
		 * overload cpu count with dropped events
		 */
		struct device_information *dip;

		dip = net_get_dip(nc, bnh.buts_name);
		dip->drop_count = bnh.cpu;
		dip->trace_started = 0;

		printf("server: end of run for %s\n", dip->buts_name);
		net_client_done(nc);
		return 0;
	}

	tip = net_get_tip(nc, &bnh);
	if (!tip)
		return 1;

	if (mmap_subbuf(tip, bnh.len))
		return 1;

	return 0;
}

static void net_add_connection(int listen_fd, struct sockaddr_in *addr)
{
	socklen_t socklen = sizeof(*addr);
	struct net_connection *nc;

	if (net_connects == NET_MAX_CONNECTIONS) {
		fprintf(stderr, "server: no more connections allowed\n");
		return;
	}

	nc = &net_connections[net_connects];
	memset(nc, 0, sizeof(*nc));

	nc->in_fd = accept(listen_fd, (struct sockaddr *) addr, &socklen);
	if (nc->in_fd < 0) {
		perror("accept");
		return;
	}

	printf("server: connection from %s\n", inet_ntoa(addr->sin_addr));
	time(&nc->connect_time);
	nc->connection_index = net_connects;
	nc->cl_in_addr = addr->sin_addr;
	net_connects++;
}

/*
 * event driven loop, handle new incoming connections and data from
 * existing connections
 */
static void net_server_handle_connections(int listen_fd,
					  struct sockaddr_in *addr)
{
	struct pollfd pfds[NET_MAX_CONNECTIONS + 1];
	int i, events;

	printf("server: waiting for connections...\n");

	while (!is_done()) {
		/*
		 * the zero entry is for incoming connections, remaining
		 * entries for clients
		 */
		pfds[0].fd = listen_fd;
		pfds[0].events = POLLIN;
		for (i = 0; i < net_connects; i++) {
			pfds[i + 1].fd = net_connections[i].in_fd;
			pfds[i + 1].events = POLLIN;
		}

		events = poll(pfds, 1 + net_connects, -1);
		if (events < 0) {
			if (errno == EINTR)
				continue;

			perror("poll");
			break;
		} else if (!events)
			continue;

		if (pfds[0].revents & POLLIN) {
			net_add_connection(listen_fd, addr);
			events--;
		}

		for (i = 0; events && i < net_connects; i++) {
			if (pfds[i + 1].revents & POLLIN) {
				net_client_data(&net_connections[i]);
				events--;
			}
		}
	}
}

/*
 * Start here when we are in server mode - just fetch data from the network
 * and dump to files
 */
static int net_server(void)
{
	struct sockaddr_in addr;
	int fd, opt;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("server: socket");
		return 1;
	}

	opt = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
		perror("setsockopt");
		return 1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(net_port);

	if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		perror("bind");
		return 1;
	}

	if (listen(fd, 1) < 0) {
		perror("listen");
		return 1;
	}

	net_server_handle_connections(fd, &addr);
	return 0;
}

/*
 * Setup outgoing network connection where we will transmit data
 */
static int net_setup_client(void)
{
	struct sockaddr_in addr;
	int fd;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("client: socket");
		return 1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(net_port);

	if (inet_aton(hostname, &addr.sin_addr) != 1) {
		struct hostent *hent = gethostbyname(hostname);
		if (!hent) {
			perror("gethostbyname");
			return 1;
		}

		memcpy(&addr.sin_addr, hent->h_addr, 4);
		strcpy(hostname, hent->h_name);
	}

	printf("blktrace: connecting to %s\n", hostname);

	if (connect(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		perror("client: connect");
		return 1;
	}

	printf("blktrace: connected!\n");
	net_out_fd = fd;
	return 0;
}

static char usage_str[] = \
	"-d <dev> [ -r debugfs path ] [ -o <output> ] [-k ] [ -w time ]\n" \
	"[ -a action ] [ -A action mask ] [ -v ]\n\n" \
	"\t-d Use specified device. May also be given last after options\n" \
	"\t-r Path to mounted debugfs, defaults to /debug\n" \
	"\t-o File(s) to send output to\n" \
	"\t-D Directory to prepend to output file names\n" \
	"\t-k Kill a running trace\n" \
	"\t-w Stop after defined time, in seconds\n" \
	"\t-a Only trace specified actions. See documentation\n" \
	"\t-A Give trace mask as a single value. See documentation\n" \
	"\t-b Sub buffer size in KiB\n" \
	"\t-n Number of sub buffers\n" \
	"\t-l Run in network listen mode (blktrace server)\n" \
	"\t-h Run in network client mode, connecting to the given host\n" \
	"\t-p Network port to use (default 8462)\n" \
	"\t-s Make the network client use sendfile() to transfer data\n" \
	"\t-V Print program version info\n\n";

static void show_usage(char *program)
{
	fprintf(stderr, "Usage: %s %s %s",program, blktrace_version, usage_str);
}

int main(int argc, char *argv[])
{
	static char default_debugfs_path[] = "/debug";
	struct statfs st;
	int i, c;
	int stop_watch = 0;
	int act_mask_tmp = 0;

	while ((c = getopt_long(argc, argv, S_OPTS, l_opts, NULL)) >= 0) {
		switch (c) {
		case 'a':
			i = find_mask_map(optarg);
			if (i < 0) {
				fprintf(stderr,"Invalid action mask %s\n",
					optarg);
				return 1;
			}
			act_mask_tmp |= i;
			break;

		case 'A':
			if ((sscanf(optarg, "%x", &i) != 1) || 
							!valid_act_opt(i)) {
				fprintf(stderr,
					"Invalid set action mask %s/0x%x\n",
					optarg, i);
				return 1;
			}
			act_mask_tmp = i;
			break;

		case 'd':
			if (resize_devices(optarg) != 0)
				return 1;
			break;

		case 'r':
			debugfs_path = optarg;
			break;

		case 'o':
			output_name = optarg;
			break;
		case 'k':
			kill_running_trace = 1;
			break;
		case 'w':
			stop_watch = atoi(optarg);
			if (stop_watch <= 0) {
				fprintf(stderr,
					"Invalid stopwatch value (%d secs)\n",
					stop_watch);
				return 1;
			}
			break;
		case 'V':
			printf("%s version %s\n", argv[0], blktrace_version);
			return 0;
		case 'b':
			buf_size = strtoul(optarg, NULL, 10);
			if (buf_size <= 0 || buf_size > 16*1024) {
				fprintf(stderr,
					"Invalid buffer size (%lu)\n",buf_size);
				return 1;
			}
			buf_size <<= 10;
			break;
		case 'n':
			buf_nr = strtoul(optarg, NULL, 10);
			if (buf_nr <= 0) {
				fprintf(stderr,
					"Invalid buffer nr (%lu)\n", buf_nr);
				return 1;
			}
			break;
		case 'D':
			output_dir = optarg;
			break;
		case 'h':
			net_mode = Net_client;
			strcpy(hostname, optarg);
			break;
		case 'l':
			net_mode = Net_server;
			break;
		case 'p':
			net_port = atoi(optarg);
			break;
		case 's':
			net_use_sendfile = 0;
			break;
		default:
			show_usage(argv[0]);
			return 1;
		}
	}

	setlocale(LC_NUMERIC, "en_US");

	page_size = getpagesize();

	if (net_mode == Net_server)
		return net_server();

	while (optind < argc) {
		if (resize_devices(argv[optind++]) != 0)
			return 1;
	}

	if (ndevs == 0) {
		show_usage(argv[0]);
		return 1;
	}

	if (act_mask_tmp != 0)
		act_mask = act_mask_tmp;

	if (!debugfs_path)
		debugfs_path = default_debugfs_path;

	if (statfs(debugfs_path, &st) < 0) {
		perror("statfs");
		fprintf(stderr,"%s does not appear to be a valid path\n",
			debugfs_path);
		return 1;
	} else if (st.f_type != (long) DEBUGFS_TYPE) {
		fprintf(stderr,"%s does not appear to be a debug filesystem\n",
			debugfs_path);
		return 1;
	}

	if (open_devices() != 0)
		return 1;

	if (kill_running_trace) {
		stop_all_traces();
		return 0;
	}

	ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	if (ncpus < 0) {
		fprintf(stderr, "sysconf(_SC_NPROCESSORS_ONLN) failed\n");
		return 1;
	}

	signal(SIGINT, handle_sigint);
	signal(SIGHUP, handle_sigint);
	signal(SIGTERM, handle_sigint);
	signal(SIGALRM, handle_sigint);

	if (net_mode == Net_client && net_setup_client())
		return 1;

	if (start_devices() != 0)
		return 1;

	atexit(stop_all_tracing);

	if (stop_watch)
		alarm(stop_watch);

	wait_for_threads();

	if (!is_trace_stopped()) {
		trace_stopped = 1;
		stop_all_threads();
		stop_all_traces();
	}

	show_stats(device_information, ndevs, ncpus);

	return 0;
}

