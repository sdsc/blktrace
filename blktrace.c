/*
 * block queue tracing application
 *
 * TODO (in no particular order):
 *	- Add ability to specify capture mask instead of logging all events
 *	- Add option for relayfs mount point
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
#include <stdio.h>
#include <stdlib.h>
#include <sched.h>

#include "blktrace.h"

#define BUF_SIZE	(128 *1024)
#define BUF_NR		(4)

struct thread_information {
	int cpu;
	pthread_t thread;
	unsigned long events_processed;
};

static char relay_path[] = "/relay/";

#define is_done()	(*(volatile int *)(&done))
static volatile int done;

static int devfd, ncpus;
static struct thread_information *thread_information;
static char *buts_name_p;

int start_trace(char *dev)
{
	struct blk_user_trace_setup buts;

	devfd = open(dev, O_RDONLY);
	if (devfd < 0) {
		perror(dev);
		return 1;
	}

	memset(&buts, sizeof(buts), 0);
	buts.buf_size = BUF_SIZE;
	buts.buf_nr = BUF_NR;

	printf("Starting trace on %s\n", dev);
	if (ioctl(devfd, BLKSTARTTRACE, &buts) < 0) {
		perror("BLKSTARTTRACE");
		return 1;
	}

	buts_name_p = strdup(buts.name);
	return 0;
}

void stop_trace(void)
{
	if (ioctl(devfd, BLKSTOPTRACE) < 0)
		perror("BLKSTOPTRACE");

	close(devfd);
}

inline int verify_trace(struct blk_io_trace *t)
{
	if (!CHECK_MAGIC(t)) {
		fprintf(stderr, "bad trace magic %x\n", t->magic);
		return 1;
	}
	if ((t->magic & 0xff) != SUPPORTED_VERSION) {
		fprintf(stderr, "unsupported trace version %x\n", 
			t->magic & 0xff);
		return 1;
	}

	return 0;
}

void extract_data(int cpu, char *ifn, int ifd, char *ofn, int ofd, int nb)
{
	int ret, bytes_left;
	unsigned char buf[nb], *p;

	p = buf;
	bytes_left = nb;
	while (bytes_left > 0) {
		ret = read(ifd, p, bytes_left);
		if (ret < 0) {
			perror(ifn);
			fprintf(stderr, "Thread %d extract_data %s failed\n",
				cpu, ifn);
			exit(1);
		} else if (ret == 0)
			usleep(1000);
		else {
			p += ret;
			bytes_left -= ret;
		}
	}

	ret = write(ofd, buf, nb);
	if (ret != nb) {
		perror(ofn);
		fprintf(stderr,"Thread %d extract_data %s failed\n", cpu, ofn);
		exit(1);
	}
}

void *extract(void *arg)
{
	struct thread_information *tip = arg;
	int tracefd, ret, ofd, dfd;
	char ip[64], op[64], dp[64];
	struct blk_io_trace t;
	pid_t pid = getpid();
	cpu_set_t cpu_mask;

	CPU_ZERO(&cpu_mask);
	CPU_SET(tip->cpu, &cpu_mask);

	if (sched_setaffinity(pid, sizeof(cpu_mask), &cpu_mask) == -1) {
		perror("sched_setaffinity");
		exit(1);
	}

	sprintf(op, "%s_out.%d", buts_name_p, tip->cpu);
	ofd = open(op, O_CREAT|O_TRUNC|O_WRONLY, 0644);
	if (ofd < 0) {
		perror(op);
		fprintf(stderr,"Thread %d failed creat of %s\n", tip->cpu, op);
		exit(1);
	}

	sprintf(dp, "%s_dat.%d", buts_name_p, tip->cpu);
	dfd = open(dp, O_CREAT|O_TRUNC|O_WRONLY, 0644);
	if (dfd < 0) {
		perror(dp);
		fprintf(stderr,"Thread %d failed creat of %s\n", tip->cpu, dp);
		exit(1);
	}

	sprintf(ip, "%s%s%d", relay_path, buts_name_p, tip->cpu);
	tracefd = open(ip, O_RDONLY);
	if (tracefd < 0) {
		perror(ip);
		fprintf(stderr,"Thread %d failed open of %s\n", tip->cpu, ip);
		exit(1);
	}

	while (!is_done()) {
		ret = read(tracefd, &t, sizeof(t));
		if (ret != sizeof(t)) {
			if (ret < 0) {
				perror(ip);
				fprintf(stderr,"Thread %d failed read of %s\n",
					tip->cpu, ip);
				exit(1);
			} else if (ret > 0) {
				fprintf(stderr,"Thread %d misread %s %d,%ld\n",
					tip->cpu, ip, ret, sizeof(t));
				exit(1);
			} else {
				usleep(10000);
				continue;
			}
		}

		if (verify_trace(&t))
			exit(1);

		switch (t.action & 0xffff) {
		case __BLK_TA_ISSUE:
		case __BLK_TA_COMPLETE:
			if (!t.pdu_len)
				break;
			else if (t.pdu_len > 64) {
				fprintf(stderr, 
					"Thread %d Payload too large %d\n", 
					tip->cpu, t.pdu_len);
				exit(1);
			}
			extract_data(tip->cpu, ip, tracefd, dp, dfd, t.pdu_len);
			break;
		}

		/* version is verified, stuff with CPU number now */
		t.magic = (t.magic & ~0xff) | tip->cpu;
		ret = write(ofd, &t, sizeof(t));
		if (ret < 0) {
			perror(op);
			fprintf(stderr,"Thread %d failed write of %s\n", 
				tip->cpu, op);
			exit(1);
		}

		tip->events_processed++;
	}

	return NULL;
}

int start_threads(void)
{
	struct thread_information *tip;
	int i;

	ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	if (ncpus < 0) {
		fprintf(stderr, "sysconf(_SC_NPROCESSORS_ONLN) failed\n");
		return 1;
	}
	printf("Processors online: %d\n", ncpus);

	thread_information = malloc(ncpus * sizeof(struct thread_information));
	for (i = 0, tip = thread_information; i < ncpus; i++, tip++) {
		tip->cpu = i;
		tip->events_processed = 0;

		if (pthread_create(&tip->thread, NULL, extract, tip)) {
			perror( "pthread_create");
			return 0;
		}
	}

	return ncpus;
}

void show_stats(void)
{
	int i;
	struct thread_information *tip;
	unsigned long events_processed = 0;

	for (i = 0, tip = thread_information; i < ncpus; i++, tip++) {
		printf("CPU%3d: %20ld events\n",
		       tip->cpu, tip->events_processed);
		events_processed += tip->events_processed;
	}

	printf("Total:  %20ld events\n", events_processed);
}

void handle_sigint(int sig)
{
	printf("exiting on signal %d\n", sig);
	done = 1;
}

int main(int argc, char *argv[])
{
	struct thread_information *tip;
	struct stat st;
	int i;

	if (argc < 2) {
		fprintf(stderr, "Usage: %s <dev>\n", argv[0]);
		return 1;
	}

	if (stat(relay_path, &st) < 0) {
		fprintf(stderr,"%s does not appear to be mounted\n", 
			relay_path);
		return 2;
	}

	if (start_trace(argv[1])) {
		fprintf(stderr, "Failed to start trace\n");
		stop_trace();
		return 3;
	}

	setlocale(LC_NUMERIC, "en_US");

	i = start_threads();
	if (!i) {
		fprintf(stderr, "Failed to start worker threads\n");
		stop_trace();
		return 4;
	}

	printf("Threads started  : %d\n", i);

	signal(SIGINT, handle_sigint);
	signal(SIGHUP, handle_sigint);
	signal(SIGTERM, handle_sigint);

	while (!is_done())
		sleep(1);

	for (i = 0, tip = thread_information; i < ncpus; i++, tip++) {
		int ret;

		if (pthread_join(tip->thread, (void *) &ret))
			perror("thread_join");
	}

	stop_trace();
	close(devfd);
	show_stats();

	return 0;
}

