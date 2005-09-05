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
#include <stdio.h>
#include <stdlib.h>
#include <sched.h>
#include <ctype.h>
#include <getopt.h>

#include "blktrace.h"

#define BUF_SIZE	(128 *1024)
#define BUF_NR		(4)

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

#define S_OPTS	"d:a:A:r:o:k"
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

	pthread_mutex_t *fd_lock;
	int ofd;

	unsigned long events_processed;
};

static char *relay_path;

#define is_done()	(*(volatile int *)(&done))
static volatile int done;

static int devfd, ncpus;
static struct thread_information *thread_information;
static char *buts_name_p;
static char *dev;
static char *output_name;
static int act_mask = ~0U;
static int trace_started;
static int kill_running_trace;

static pthread_mutex_t stdout_mutex = PTHREAD_MUTEX_INITIALIZER;

static int find_mask_map(char *string)
{
	int i;

	for (i = 0; i < sizeof(mask_maps)/sizeof(mask_maps[0]); i++)
		if (COMPARE_MASK_MAP(&mask_maps[i], string))
			return mask_maps[i].mask;

	return -1;
}

static int start_trace(char *dev)
{
	struct blk_user_trace_setup buts;

	memset(&buts, 0, sizeof(buts));
	buts.buf_size = BUF_SIZE;
	buts.buf_nr = BUF_NR;
	buts.act_mask = act_mask;

	if (ioctl(devfd, BLKSTARTTRACE, &buts) < 0) {
		perror("BLKSTARTTRACE");
		return 1;
	}

	trace_started = 1;
	buts_name_p = strdup(buts.name);
	return 0;
}

static void stop_trace(void)
{
	if (trace_started || kill_running_trace) {
		if (ioctl(devfd, BLKSTOPTRACE) < 0)
			perror("BLKSTOPTRACE");

		trace_started = 0;
	}
}

static void extract_data(struct thread_information *tip, char *ofn, int nb)
{
	int ret, bytes_left;
	unsigned char *buf, *p;

	buf = malloc(nb);
	p = buf;
	bytes_left = nb;
	while (bytes_left > 0) {
		ret = read(tip->fd, p, bytes_left);
		if (!ret)
			usleep(1000);
		else if (ret < 0) {
			perror(tip->fn);
			fprintf(stderr, "Thread %d extract_data %s failed\n",
				tip->cpu, tip->fn);
			free(buf);
			exit(1);
		} else {
			p += ret;
			bytes_left -= ret;
		}
	}

	ret = write(tip->ofd, buf, nb);
	if (ret != nb) {
		perror(ofn);
		fprintf(stderr,"Thread %d extract_data %s failed\n", tip->cpu, ofn);
		free(buf);
		exit(1);
	}

	free(buf);
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
	char dp[64];
	struct blk_io_trace t;
	pid_t pid = getpid();
	cpu_set_t cpu_mask;

	CPU_ZERO(&cpu_mask);
	CPU_SET((tip->cpu), &cpu_mask);

	if (sched_setaffinity(pid, sizeof(cpu_mask), &cpu_mask) == -1) {
		perror("sched_setaffinity");
		exit(1);
	}

	snprintf(tip->fn, sizeof(tip->fn),
		 "%s/block/%s/trace%d", relay_path, buts_name_p, tip->cpu);
	tip->fd = open(tip->fn, O_RDONLY);
	if (tip->fd < 0) {
		perror(tip->fn);
		fprintf(stderr,"Thread %d failed open of %s\n", tip->cpu,
			tip->fn);
		exit(1);
	}

	while (!is_done()) {
		ret = read(tip->fd, &t, sizeof(t));
		if (ret != sizeof(t)) {
			if (ret < 0) {
				perror(tip->fn);
				fprintf(stderr,"Thread %d failed read of %s\n",
					tip->cpu, tip->fn);
				exit(1);
			} else if (ret > 0) {
				fprintf(stderr,"Thread %d misread %s %d,%d\n",
					tip->cpu, tip->fn, ret, (int)sizeof(t));
				exit(1);
			} else {
				usleep(10000);
				continue;
			}
		}

		if (verify_trace(&t))
			exit(1);

		pdu_len = t.pdu_len;

		trace_to_be(&t);

		tip_fd_lock(tip);

		ret = write(tip->ofd, &t, sizeof(t));
		if (ret < 0) {
			fprintf(stderr,"Thread %d failed write\n", tip->cpu);
			tip_fd_unlock(tip);
			exit(1);
		}

		if (pdu_len)
			extract_data(tip, dp, pdu_len);

		tip_fd_unlock(tip);

		tip->events_processed++;
	}

	return NULL;
}

static int start_threads(void)
{
	struct thread_information *tip;
	char op[64];
	int i;

	ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	if (ncpus < 0) {
		fprintf(stderr, "sysconf(_SC_NPROCESSORS_ONLN) failed\n");
		return 0;
	}

	thread_information = malloc(ncpus * sizeof(struct thread_information));
	for (i = 0, tip = thread_information; i < ncpus; i++, tip++) {
		tip->fd_lock = NULL;
		tip->cpu = i;
		tip->events_processed = 0;

		if (!strcmp(output_name, "-")) {
			tip->ofd = dup(STDOUT_FILENO);
			tip->fd_lock = &stdout_mutex;
		} else {
			sprintf(op, "%s_out.%d", output_name, tip->cpu);
			tip->ofd = open(op, O_CREAT|O_TRUNC|O_WRONLY, 0644);
		}

		if (tip->ofd < 0) {
			perror(op);
			return 0;
		}

		if (pthread_create(&tip->thread, NULL, extract, tip)) {
			perror( "pthread_create");
			return 0;
		}
	}

	return ncpus;
}

static void stop_threads(void)
{
	struct thread_information *tip = thread_information;
	int i;

	for (i = 0; i < ncpus; i++, tip++) {
		int ret;

		if (pthread_join(tip->thread, (void *) &ret))
			perror("thread_join");

		close(tip->fd);
		close(tip->ofd);
	}
}

static void show_stats(void)
{
	int i;
	struct thread_information *tip;
	unsigned long events_processed = 0;

	if (!strcmp(output_name, "-"))
		return;

	for (i = 0, tip = thread_information; i < ncpus; i++, tip++) {
		printf("CPU%3d: %20ld events\n",
		       tip->cpu, tip->events_processed);
		events_processed += tip->events_processed;
	}

	printf("Total:  %20ld events\n", events_processed);
}

static void handle_sigint(int sig)
{
	done = 1;
}

int main(int argc, char *argv[])
{
	static char default_relay_path[] = "/relay";
	struct stat st;
	int i, c;
	int act_mask_tmp = 0;

	while ((c = getopt_long(argc, argv, S_OPTS, l_opts, NULL)) >= 0) {
		switch (c) {
		case 'a':
			i = find_mask_map(optarg);
			if (i < 0) {
				fprintf(stderr,"Invalid action mask %s\n", 
					optarg);
				return 4;
			}
			act_mask_tmp |= i;
			break;

		case 'A':
			if ((sscanf(optarg, "%x", &i) != 1) || !VALID_SET(i)) {
				fprintf(stderr,
					"Invalid set action mask %s/0x%x\n", 
					optarg, i);
				return 4;
			}
			act_mask_tmp = i;
			break;

		case 'd':
			dev = strdup(optarg);
			break;

		case 'r':
			relay_path = optarg;
			break;

		case 'o':
			output_name = strdup(optarg);
			break;
		case 'k':
			kill_running_trace = 1;
			break;

		default:
			fprintf(stderr,"Usage: %s -d <dev> "
				       "[-a <trace> [-a <trace>]]\n", argv[0]);
			return 4;
		}
	}

	if ((dev == NULL) || (optind < argc)) {
		fprintf(stderr,"Usage: %s -d <dev> "
			       "[-a <trace> [-a <trace>]]\n", argv[0]);
		return 4;
	}

	if (!relay_path)
		relay_path = default_relay_path;

	if (act_mask_tmp != 0)
		act_mask = act_mask_tmp;

	if (stat(relay_path, &st) < 0) {
		fprintf(stderr,"%s does not appear to be mounted\n",
			relay_path);
		return 2;
	}

	devfd = open(dev, O_RDONLY);
	if (devfd < 0) {
		perror(dev);
		return 3;
	}

	if (kill_running_trace) {
		stop_trace();
		exit(0);
	}

	if (start_trace(dev)) {
		close(devfd);
		fprintf(stderr, "Failed to start trace on %s\n", dev);
		return 4;
	}

	setlocale(LC_NUMERIC, "en_US");

	if (!output_name)
		output_name = strdup(buts_name_p);

	i = start_threads();
	if (!i) {
		fprintf(stderr, "Failed to start worker threads\n");
		stop_trace();
		return 5;
	}

	signal(SIGINT, handle_sigint);
	signal(SIGHUP, handle_sigint);
	signal(SIGTERM, handle_sigint);

	atexit(stop_trace);

	while (!is_done())
		sleep(1);

	stop_threads();
	stop_trace();
	show_stats();
	close(devfd);

	return 0;
}

