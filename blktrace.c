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
#include <errno.h>

#include "blktrace.h"

static char blktrace_version[] = "0.99";

/*
 * You may want to increase this even more, if you are logging at a high
 * rate and see skipped/missed events
 */
#define BUF_SIZE	(512 * 1024)
#define BUF_NR		(4)

#define OFILE_BUF	(128 * 1024)

#define RELAYFS_TYPE	0xF0B4A981

#define S_OPTS	"d:a:A:r:o:kw:Vb:n:D:"
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
		.name = NULL,
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
	FILE *ofile;
	char *ofile_buffer;

	volatile int closed;

	unsigned long events_processed;
	struct device_information *device;
};

struct device_information {
	int fd;
	char *path;
	char buts_name[32];
	volatile int trace_started;
	struct thread_information *threads;
};

static int ncpus;
static struct thread_information *thread_information;
static int ndevs;
static struct device_information *device_information;

/* command line option globals */
static char *relay_path;
static char *output_name;
static char *output_dir;
static int act_mask = ~0U;
static int kill_running_trace;
static unsigned int buf_size = BUF_SIZE;
static unsigned int buf_nr = BUF_NR;

#define is_done()	(*(volatile int *)(&done))
static volatile int done;

static pthread_mutex_t stdout_mutex = PTHREAD_MUTEX_INITIALIZER;

static void exit_trace(int status);

#define tip_closed(tip)		(*(volatile int *)(&(tip)->closed))
#define set_tip_closed(tip)	((tip)->closed = 1)

#define dip_tracing(dip)	(*(volatile int *)(&(dip)->trace_started))
#define dip_set_tracing(dip, v)	((dip)->trace_started = (v))

#define __for_each_dip(__d, __i, __e)	\
	for (__i = 0, __d = device_information; __i < __e; __i++, __d++)

#define for_each_dip(__d, __i)	__for_each_dip(__d, __i, ndevs)
#define for_each_tip(__d, __t, __i)	\
	for (__i = 0, __t = (__d)->threads; __i < ncpus; __i++, __t++)

static int start_trace(struct device_information *dip)
{
	struct blk_user_trace_setup buts;

	memset(&buts, 0, sizeof(buts));
	buts.buf_size = buf_size;
	buts.buf_nr = buf_nr;
	buts.act_mask = act_mask;

	if (ioctl(dip->fd, BLKSTARTTRACE, &buts) < 0) {
		perror("BLKSTARTTRACE");
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

		if (ioctl(dip->fd, BLKSTOPTRACE) < 0)
			perror("BLKSTOPTRACE");

		close(dip->fd);
		dip->fd = -1;
	}
}

static void stop_all_traces(void)
{
	struct device_information *dip;
	int i;

	for_each_dip(dip, i)
		stop_trace(dip);
}

static int read_data(struct thread_information *tip, void *buf, int len)
{
	char *p = buf;
	int ret, bytes_left = len;

	while (!is_done() && bytes_left > 0) {
		ret = read(tip->fd, p, bytes_left);
		if (ret == bytes_left)
			return 0;

		if (ret < 0) {
			perror(tip->fn);
			fprintf(stderr,"Thread %d failed read of %s\n",
				tip->cpu, tip->fn);
			break;
		} else if (ret > 0) {
			p += ret;
			bytes_left -= ret;
		} else
			usleep(1000);
	}

	return -1;
}

static int write_data(FILE *file, void *buf, unsigned int buf_len)
{
	int ret, bytes_left;
	char *p = buf;

	bytes_left = buf_len;
	while (bytes_left > 0) {
		ret = fwrite(p, bytes_left, 1, file);
		if (ret == 1)
			break;

		if (ret < 0) {
			perror("write");
			return 1;
		}
	}

	return 0;
}

static void *extract_data(struct thread_information *tip, int nb)
{
	unsigned char *buf;

	buf = malloc(nb);
	if (!read_data(tip, buf, nb))
		return buf;

	free(buf);
	return NULL;
}

/*
 * trace may start inside 'bit' or may need to be gotten further on
 */
static int get_event_slow(struct thread_information *tip,
			  struct blk_io_trace *bit)
{
	const int inc = sizeof(__u32);
	struct blk_io_trace foo;
	unsigned int offset;
	void *p;

	/*
	 * check is trace is inside
	 */
	offset = 0;
	p = bit;
	while (offset < sizeof(*bit)) {
		p += inc;
		offset += inc;

		memcpy(&foo, p, inc);

		if (CHECK_MAGIC(&foo))
			break;
	}

	/*
	 * part trace found inside, read the rest
	 */
	if (offset < sizeof(*bit)) {
		int good_bytes = sizeof(*bit) - offset;

		memmove(bit, p, good_bytes);
		p = (void *) bit + good_bytes;

		return read_data(tip, p, offset);
	}

	/*
	 * nothing found, keep looking for start of trace
	 */
	do {
		if (read_data(tip, bit, sizeof(bit->magic)))
			return -1;
	} while (!CHECK_MAGIC(bit));

	/*
	 * now get the rest of it
	 */
	p = &bit->sequence;
	if (!read_data(tip, p, sizeof(*bit) - inc))
		return -1;

	return 0;
}

/*
 * Sometimes relayfs screws us a little, if an event crosses a sub buffer
 * boundary. So keep looking forward in the trace data until an event
 * is found
 */
static int get_event(struct thread_information *tip, struct blk_io_trace *bit)
{
	/*
	 * optimize for the common fast case, a full trace read that
	 * succeeds
	 */
	if (read_data(tip, bit, sizeof(*bit)))
		return -1;

	if (CHECK_MAGIC(bit))
		return 0;

	/*
	 * ok that didn't work, the event may start somewhere inside the
	 * trace itself
	 */
	return get_event_slow(tip, bit);
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
	int pdu_len;
	char *pdu_data;
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

	pdu_data = NULL;
	while (!is_done()) {
		if (get_event(tip, &t))
			break;

		if (verify_trace(&t))
			break;

		pdu_len = t.pdu_len;

		trace_to_be(&t);

		if (pdu_len) {
			pdu_data = extract_data(tip, pdu_len);
			if (!pdu_data)
				break;
		}

		/*
		 * now we have both trace and payload, get a lock on the
		 * output descriptor and send it off
		 */
		tip_fd_lock(tip);

		if (write_data(tip->ofile, &t, sizeof(t))) {
			tip_fd_unlock(tip);
			break;
		}

		if (pdu_data && write_data(tip->ofile, pdu_data, pdu_len)) {
			tip_fd_unlock(tip);
			break;
		}

		tip_fd_unlock(tip);

		if (pdu_data) {
			free(pdu_data);
			pdu_data = NULL;
		}

		tip->events_processed++;
	}

	exit_trace(1);
	return NULL;
}

static void close_thread(struct thread_information *tip)
{
	if (tip_closed(tip))
		return;

	set_tip_closed(tip);

	if (tip->fd != -1)
		close(tip->fd);
	if (tip->ofile)
		fclose(tip->ofile);
	if (tip->ofile_buffer)
		free(tip->ofile_buffer);

	tip->fd = -1;
	tip->ofile = NULL;
	tip->ofile_buffer = NULL;
}

static int start_threads(struct device_information *dip)
{
	struct thread_information *tip;
	char op[64];
	int j, pipeline = output_name && !strcmp(output_name, "-");
	int len, mode;

	for_each_tip(dip, tip, j) {
		tip->cpu = j;
		tip->device = dip;
		tip->fd_lock = NULL;
		tip->events_processed = 0;

		if (pipeline) {
			tip->ofile = fdopen(STDOUT_FILENO, "w");
			tip->fd_lock = &stdout_mutex;
			mode = _IOLBF;
			buf_size = 512;
		} else {
			len = 0;

			if (output_dir)
				len = sprintf(op, "%s/", output_dir);

			if (output_name) {
				sprintf(op + len, "%s.blktrace.%d", output_name,
					tip->cpu);
			} else {
				sprintf(op + len, "%s.blktrace.%d",
					dip->buts_name, tip->cpu);
			}
			tip->ofile = fopen(op, "w");
			mode = _IOFBF;
			buf_size = OFILE_BUF;
		}

		if (tip->ofile == NULL) {
			perror(op);
			return 1;
		}

		tip->ofile_buffer = malloc(buf_size);
		if (setvbuf(tip->ofile, tip->ofile_buffer, mode, buf_size)) {
			perror("setvbuf");
			close_thread(tip);
			return 1;
		}

		if (pthread_create(&tip->thread, NULL, extract, tip)) {
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
	long ret;
	int i;

	for_each_tip(dip, tip, i) {
		if (pthread_join(tip->thread, (void *) &ret))
			perror("thread_join");

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
	struct thread_information *tip;
	int i, j;

	for_each_dip(dip, i) {
		for_each_tip(dip, tip, j)
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
		__for_each_dip(dip, j, i)
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
		__for_each_dip(dip, j, i)
			stop_threads(dip);
		for_each_dip(dip, i)
			stop_trace(dip);

		return 1;
	}

	return 0;
}

static int get_dropped_count(const char *buts_name)
{
	int fd;
	char tmp[MAXPATHLEN + 64];

	snprintf(tmp, sizeof(tmp), "%s/block/%s/dropped",
		 relay_path, buts_name);

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

static void show_stats(void)
{
	int i, j, dropped, total_drops, no_stdout = 0;
	struct device_information *dip;
	struct thread_information *tip;
	unsigned long long events_processed;

	if (output_name && !strcmp(output_name, "-"))
		no_stdout = 1;

	total_drops = 0;
	for_each_dip(dip, i) {
		if (!no_stdout)
			printf("Device: %s\n", dip->path);
		events_processed = 0;
		for_each_tip(dip, tip, j) {
			if (!no_stdout)
				printf("  CPU%3d: %20ld events\n",
			       		tip->cpu, tip->events_processed);
			events_processed += tip->events_processed;
		}
		dropped = get_dropped_count(dip->buts_name);
		total_drops += dropped;
		if (!no_stdout)
			printf("  Total:  %20lld events (dropped %d)\n",
					events_processed, dropped);
	}

	if (total_drops)
		fprintf(stderr, "You have dropped events, consider using a larger buffer size (-b)\n");
}

static char usage_str[] = \
	"-d <dev> [ -r relay path ] [ -o <output> ] [-k ] [ -w time ]\n" \
	"[ -a action ] [ -A action mask ] [ -v ]\n\n" \
	"\t-d Use specified device. May also be given last after options\n" \
	"\t-r Path to mounted relayfs, defaults to /relay\n" \
	"\t-o File(s) to send output to\n" \
	"\t-D Directory to prepend to output file names\n" \
	"\t-k Kill a running trace\n" \
	"\t-w Stop after defined time, in seconds\n" \
	"\t-a Only trace specified actions. See documentation\n" \
	"\t-A Give trace mask as a single value. See documentation\n" \
	"\t-b Sub buffer size in KiB\n" \
	"\t-n Number of sub buffers\n" \
	"\t-v Print program version info\n\n";

static void show_usage(char *program)
{
	fprintf(stderr, "Usage: %s %s %s",program, blktrace_version, usage_str);
}

static void handle_sigint(__attribute__((__unused__)) int sig)
{
	done = 1;
	show_stats();
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
		case 'V':
			printf("%s version %s\n", argv[0], blktrace_version);
			return 0;
		case 'b':
			buf_size = atoi(optarg);
			if (buf_size <= 0 || buf_size > 16*1024) {
				fprintf(stderr,
					"Invalid buffer size (%d)\n", buf_size);
				return 1;
			}
			buf_size <<= 10;
			break;
		case 'n':
			buf_nr = atoi(optarg);
			if (buf_nr <= 0) {
				fprintf(stderr,
					"Invalid buffer nr (%d)\n", buf_nr);
				return 1;
			}
			break;
		case 'D':
			output_dir = optarg;
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
	} else if (st.f_type != (long) RELAYFS_TYPE) {
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

