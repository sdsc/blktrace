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
#include <stdio.h>
#include <stdlib.h>
#include <sched.h>
#include <ctype.h>
#include <getopt.h>
#include <sys/mman.h>

#include "blktrace.h"

#define BUF_SIZE	(128 *1024)
#define BUF_NR		(4)

#define RELAYFS_TYPE	0xF0B4A981

#define DECLARE_MASK_MAP(mask)          { BLK_TC_##mask, #mask, "BLK_TC_"#mask }
#define COMPARE_MASK_MAP(mmp, str)                                      \
        (!strcasecmp((mmp)->short_form, (str)) ||                      \
         !strcasecmp((mmp)->long_form, (str)))

#define VALID_SET(x)	((1 <= (x)) && ((x) < (1 << BLK_TC_SHIFT)))

struct mask_map {
	int mask;
	char *short_form;
	char *long_form;
};

struct mask_map mask_maps[] = {
	DECLARE_MASK_MAP(READ),
	DECLARE_MASK_MAP(WRITE),
	DECLARE_MASK_MAP(BARRIER),
	DECLARE_MASK_MAP(SYNC),
	DECLARE_MASK_MAP(QUEUE),
	DECLARE_MASK_MAP(REQUEUE),
	DECLARE_MASK_MAP(ISSUE),
	DECLARE_MASK_MAP(COMPLETE),
	DECLARE_MASK_MAP(FS),
	DECLARE_MASK_MAP(PC),
};

#define S_OPTS	"d:a:A:r:o:kw:"
static struct option l_opts[] = {
	{
		.name = "dev",
		.has_arg = 1,
		.flag = NULL,
		.val = 'd'
	},
	{
		.name = "act-mask",
		.has_arg = 1,
		.flag = NULL,
		.val = 'a'
	},
	{
		.name = "set-mask",
		.has_arg = 1,
		.flag = NULL,
		.val = 'A'
	},
	{
		.name = "relay",
		.has_arg = 1,
		.flag = NULL,
		.val = 'r'
	},
	{
		.name = "output",
		.has_arg = 1,
		.flag = NULL,
		.val = 'o'
	},
	{
		.name = "kill",
		.has_arg = 0,
		.flag = NULL,
		.val = 'k'
	},
	{
		.name = "stopwatch",
		.has_arg = 1,
		.flag = NULL,
		.val = 'w'
	},
	{
		.name = NULL,
		.has_arg = 0,
		.flag = NULL,
		.val = 0
	}
};

struct thread_information {
	int cpu;
	pthread_t thread;

	int fd;
	char fn[MAXPATHLEN + 64];
	void *buf;
	unsigned long buf_offset;
	unsigned int buf_subbuf;
	unsigned int sequence;

	pthread_mutex_t *fd_lock;
	int ofd;

	unsigned long events_processed;
	struct device_information *device;
};

struct device_information {
	int fd;
	char *path;
	char buts_name[32];
	int trace_started;
	struct thread_information *threads;
};

static int ncpus;
static struct thread_information *thread_information;
static int ndevs;
static struct device_information *device_information;

/* command line option globals */
static char *relay_path;
static char *output_name;
static int act_mask = ~0U;
static int kill_running_trace;
static int use_mmap;

#define is_done()	(*(volatile int *)(&done))
static volatile int done;

static pthread_mutex_t stdout_mutex = PTHREAD_MUTEX_INITIALIZER;

static void exit_trace(int status);

static int find_mask_map(char *string)
{
	int i;

	for (i = 0; i < sizeof(mask_maps)/sizeof(mask_maps[0]); i++)
		if (COMPARE_MASK_MAP(&mask_maps[i], string))
			return mask_maps[i].mask;

	return -1;
}

static int start_trace(struct device_information *dip)
{
	struct blk_user_trace_setup buts;

	memset(&buts, 0, sizeof(buts));
	buts.buf_size = BUF_SIZE;
	buts.buf_nr = BUF_NR;
	buts.act_mask = act_mask;

	if (ioctl(dip->fd, BLKSTARTTRACE, &buts) < 0) {
		perror("BLKSTARTTRACE");
		return 1;
	}

	memcpy(dip->buts_name, buts.name, sizeof(dip->buts_name));
	dip->trace_started = 1;
	return 0;
}

static void stop_trace(struct device_information *dip)
{
	if (dip->trace_started || kill_running_trace) {
		if (ioctl(dip->fd, BLKSTOPTRACE) < 0)
			perror("BLKSTOPTRACE");
		close(dip->fd);
		dip->trace_started = 0;
	}
}

static void stop_all_traces(void)
{
	struct device_information *dip;
	int i;

	for (dip = device_information, i = 0; i < ndevs; i++, dip++)
		stop_trace(dip);
}

static int get_data_read(struct thread_information *tip, void *buf, int len)
{
	char *p = buf;
	int ret, bytes_left = len;

	while (!is_done() && bytes_left > 0) {
		ret = read(tip->fd, p, bytes_left);
		if (ret == len)
			break;

		if (ret < 0) {
			perror(tip->fn);
			fprintf(stderr,"Thread %d failed read of %s\n",
				tip->cpu, tip->fn);
			exit_trace(1);
		} else if (ret > 0) {
			fprintf(stderr,"Thread %d misread %s %d,%d\n",
				tip->cpu, tip->fn, ret, len);
			exit_trace(1);
		} else {
			p += ret;
			bytes_left -= ret;
		}

		usleep(10000);
	}

	return 0;
}

static int get_data_mmap(struct thread_information *tip, void *buf, int len,
			 int check_magic)
{
	if (len > (BUF_SIZE * (tip->buf_subbuf + 1)) - tip->buf_offset) {
		tip->buf_subbuf++;
		if (tip->buf_subbuf == BUF_NR)
			tip->buf_subbuf = 0;

		tip->buf_offset = tip->buf_subbuf * BUF_SIZE;
	}

	while (!is_done()) {
		struct blk_io_trace *t = buf;

		memcpy(buf, tip->buf + tip->buf_offset, len);

		if (!check_magic)
			break;

		if (CHECK_MAGIC(t) && t->sequence >= tip->sequence) {
			tip->sequence = t->sequence;
			break;
		}

		usleep(10000);
	}

	tip->buf_offset += len;
	return 0;
}

static int get_data(struct thread_information *tip, void *buf, int len,
		    int check_magic)
{
	if (tip->buf)
		return get_data_mmap(tip, buf, len, check_magic);
	else
		return get_data_read(tip, buf, len);
}

static void *extract_data(struct thread_information *tip, char *ofn, int nb)
{
	unsigned char *buf;

	buf = malloc(nb);
	if (!get_data(tip, buf, nb, 0))
		return buf;

	free(buf);
	exit_trace(1);
	return NULL;
}

static inline void tip_fd_unlock(struct thread_information *tip)
{
	if (tip->fd_lock)
		pthread_mutex_unlock(tip->fd_lock);
}

static inline void tip_fd_lock(struct thread_information *tip)
{
	if (tip->fd_lock)
		pthread_mutex_lock(tip->fd_lock);
}

static void *extract(void *arg)
{
	struct thread_information *tip = arg;
	int ret, pdu_len;
	char dp[64], *pdu_data;
	struct blk_io_trace t;
	pid_t pid = getpid();
	cpu_set_t cpu_mask;

	CPU_ZERO(&cpu_mask);
	CPU_SET((tip->cpu), &cpu_mask);

	if (sched_setaffinity(pid, sizeof(cpu_mask), &cpu_mask) == -1) {
		perror("sched_setaffinity");
		exit_trace(1);
	}

	snprintf(tip->fn, sizeof(tip->fn), "%s/block/%s/trace%d",
			relay_path, tip->device->buts_name, tip->cpu);
	tip->fd = open(tip->fn, O_RDONLY);
	if (tip->fd < 0) {
		perror(tip->fn);
		fprintf(stderr,"Thread %d failed open of %s\n", tip->cpu,
			tip->fn);
		exit_trace(1);
	}

	if (use_mmap) {
		tip->buf = mmap(NULL, BUF_SIZE * BUF_NR, PROT_READ,
					MAP_PRIVATE | MAP_POPULATE, tip->fd, 0);
		if (tip->buf == MAP_FAILED) {
			perror("mmap");
			exit_trace(1);
		}
	}

	pdu_data = NULL;
	while (!is_done()) {
		if (get_data(tip, &t, sizeof(t), 1))
			break;

		if (verify_trace(&t))
			exit_trace(1);

		pdu_len = t.pdu_len;

		trace_to_be(&t);

		if (pdu_len)
			pdu_data = extract_data(tip, dp, pdu_len);

		/*
		 * now we have both trace and payload, get a lock on the
		 * output descriptor and send it off
		 */
		tip_fd_lock(tip);

		ret = write(tip->ofd, &t, sizeof(t));
		if (ret < 0) {
			fprintf(stderr,"Thread %d failed write\n", tip->cpu);
			tip_fd_unlock(tip);
			exit_trace(1);
		}

		if (pdu_data) {
			ret = write(tip->ofd, pdu_data, pdu_len);
			if (ret != pdu_len) {
				perror("write pdu data");
				tip_fd_unlock(tip);
				exit_trace(1);
			}

			free(pdu_data);
			pdu_data = NULL;
		}

		tip_fd_unlock(tip);
		tip->events_processed++;
	}

	return NULL;
}

static int start_threads(struct device_information *dip)
{
	struct thread_information *tip;
	char op[64];
	int j, pipeline = output_name && !strcmp(output_name, "-");

	for (tip = dip->threads, j = 0; j < ncpus; j++, tip++) {
		tip->cpu = j;
		tip->device = dip;
		tip->fd_lock = NULL;
		tip->events_processed = 0;

		if (pipeline) {
			tip->ofd = dup(STDOUT_FILENO);
			tip->fd_lock = &stdout_mutex;
		} else {
			if (output_name)
				sprintf(op, "%s_%s_out.%d", output_name,
					dip->buts_name, tip->cpu);
			else
				sprintf(op, "%s_out.%d",
					dip->buts_name, tip->cpu);
			tip->ofd = open(op, O_CREAT|O_TRUNC|O_WRONLY, 0644);
		}

		if (tip->ofd < 0) {
			perror(op);
			return 1;
		}

		if (pthread_create(&tip->thread, NULL, extract, tip)) {
			perror("pthread_create");
			close(tip->ofd);
			return 1;
		}
	}

	return 0;
}

static void close_thread(struct thread_information *tip)
{
	if (tip->buf)
		munmap(tip->buf, BUF_SIZE * BUF_NR);

	if (tip->fd != -1)
		close(tip->fd);
	if (tip->ofd != -1)
		close(tip->ofd);

	tip->fd = tip->ofd = -1;
}

static void stop_threads(struct device_information *dip)
{
	struct thread_information *tip;
	long ret;
	int j;

	for (tip = dip->threads, j = 0; j < ncpus; j++, tip++) {
		if (pthread_join(tip->thread, (void *) &ret))
			perror("thread_join");
		close_thread(tip);
	}
}

static void stop_all_threads(void)
{
	struct device_information *dip;
	int i;

	for (dip = device_information, i = 0; i < ndevs; i++, dip++)
		stop_threads(dip);
}

static void stop_all_tracing(void)
{
	struct device_information *dip;
	struct thread_information *tip;
	int i, j;

	for (dip = device_information, i = 0; i < ndevs; i++, dip++) {
		for (tip = dip->threads, j = 0; j < ncpus; j++, tip++)
			close_thread(tip);
		stop_trace(dip);
	}
}

static void exit_trace(int status)
{
	stop_all_tracing();
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

	for (dip = device_information, i = 0; i < ndevs; i++, dip++) {
		dip->fd = open(dip->path, O_RDONLY);
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

	for (dip = device_information, i = 0; i < ndevs; i++, dip++) {
		if (start_trace(dip)) {
			close(dip->fd);
			fprintf(stderr, "Failed to start trace on %s\n",
				dip->path);
			break;
		}
	}
	if (i != ndevs) {
		for (dip = device_information, j = 0; j < i; j++, dip++)
			stop_trace(dip);
		return 1;
	}

	for (dip = device_information, i = 0; i < ndevs; i++, dip++) {
		dip->threads = thread_information + (i * ncpus);
		if (start_threads(dip)) {
			fprintf(stderr, "Failed to start worker threads\n");
			break;
		}
	}
	if (i != ndevs) {
		for (dip = device_information, j = 0; j < i; j++, dip++)
			stop_threads(dip);
		for (dip = device_information, i = 0; i < ndevs; i++, dip++)
			stop_trace(dip);
		return 1;
	}

	return 0;
}

static void show_stats(void)
{
	int i, j;
	struct device_information *dip;
	struct thread_information *tip;
	unsigned long long events_processed;
  
	if (output_name && !strcmp(output_name, "-"))
		return;

	for (dip = device_information, i = 0; i < ndevs; i++, dip++) {
		printf("Device: %s\n", dip->path);
		events_processed = 0;
		for (tip = dip->threads, j = 0; j < ncpus; j++, tip++) {
			printf("  CPU%3d: %20ld events\n",
			       tip->cpu, tip->events_processed);
			events_processed += tip->events_processed;
		}
		printf("  Total:  %20lld events\n", events_processed);
	}
}
  
static void show_usage(char *program)
{
	fprintf(stderr,"Usage: %s [-d <dev>] "
		       "[-a <trace> [-a <trace>]] <dev>\n",
		program);
}

static void handle_sigint(int sig)
{
	done = 1;
}

int main(int argc, char *argv[])
{
	static char default_relay_path[] = "/relay";
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
			if ((sscanf(optarg, "%x", &i) != 1) || !VALID_SET(i)) {
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
			relay_path = optarg;
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

		default:
			show_usage(argv[0]);
			return 1;
		}
	}

	while (optind < argc) {
		if (resize_devices(argv[optind++]) != 0)
			return 1;
	}

	if (ndevs == 0) {
		show_usage(argv[0]);
		return 1;
	}

	if (!relay_path)
		relay_path = default_relay_path;

	if (act_mask_tmp != 0)
		act_mask = act_mask_tmp;

	if (statfs(relay_path, &st) < 0) {
		perror("statfs");
		fprintf(stderr,"%s does not appear to be a valid path\n",
			relay_path);
		return 1;
	} else if (st.f_type != RELAYFS_TYPE) {
		fprintf(stderr,"%s does not appear to be a relay filesystem\n",
			relay_path);
		return 1;
	}

	if (open_devices() != 0)
		return 1;

	if (kill_running_trace) {
		stop_all_traces();
		return 0;
	}

	setlocale(LC_NUMERIC, "en_US");

	ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	if (ncpus < 0) {
		fprintf(stderr, "sysconf(_SC_NPROCESSORS_ONLN) failed\n");
		return 1;
	}

	if (start_devices() != 0)
		return 1;

	signal(SIGINT, handle_sigint);
	signal(SIGHUP, handle_sigint);
	signal(SIGTERM, handle_sigint);
	signal(SIGALRM, handle_sigint);

	atexit(stop_all_tracing);

	if (stop_watch)
		alarm(stop_watch);

	while (!is_done())
		sleep(1);

	stop_all_threads();
	stop_all_traces();
	show_stats();

	return 0;
}

