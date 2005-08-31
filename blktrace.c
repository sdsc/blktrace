/*
 * block queue tracing application
 *
 * TODO (in no particular order):
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
#include <ctype.h>
#include <getopt.h>

#include "blktrace.h"

#define BUF_SIZE	(128 *1024)
#define BUF_NR		(4)

#define DECLARE_MASK_MAP(mask)          { BLK_TC_##mask, #mask, "BLK_TC_"#mask }
#define COMPARE_MASK_MAP(mmp, str)                                      \
        (!strcmp(mmp->short_form, toupper(str)) ||                      \
         !strcmp(mmp->long_form, toupper(str)))

#define VALID_SET(x)	((1 <= (x)) && ((x) < (1 << BLK_TC_SHIFT)))

struct mask_map {
	int mask;
	char *short_form;
	char *long_form;
};

struct mask_map mask_maps[] = {
        DECLARE_MASK_MAP( READ     ),
        DECLARE_MASK_MAP( WRITE    ),
        DECLARE_MASK_MAP( BARRIER  ),
        DECLARE_MASK_MAP( SYNC     ),
        DECLARE_MASK_MAP( QUEUE    ),
        DECLARE_MASK_MAP( REQUEUE  ),
        DECLARE_MASK_MAP( ISSUE    ),
        DECLARE_MASK_MAP( COMPLETE ),
        DECLARE_MASK_MAP( FS       ),
        DECLARE_MASK_MAP( PC       ),
};

#define S_OPTS	"d:a:A:"
struct option l_opts[] = {
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
		.name = NULL,
		.has_arg = 0,
		.flag = NULL,
		.val = 0
	}
};

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
static char *dev;
static int act_mask = ~0;

inline int compare_mask_map(struct mask_map *mmp, char *string)
{
        int i;
        char *s, *ustring = strdup(string);

        for (i = 0, s = ustring; i < strlen(ustring); i++, s++)
                *s = toupper(*s);

        return !strcmp(mmp->short_form, ustring) ||
               !strcmp(mmp->long_form, ustring);
}

int find_mask_map(char *string)
{
        int i;

        for (i = 0; i < sizeof(mask_maps)/sizeof(mask_maps[0]); i++)
                if (compare_mask_map(&mask_maps[i], string))
                        return mask_maps[i].mask;
	return -1;
}

static int start_trace(char *dev)
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
	buts.act_mask = act_mask;

	printf("Starting trace on %s\n", dev);
	if (ioctl(devfd, BLKSTARTTRACE, &buts) < 0) {
		perror("BLKSTARTTRACE");
		return 1;
	}

	buts_name_p = strdup(buts.name);
	return 0;
}

static void stop_trace(void)
{
	if (ioctl(devfd, BLKSTOPTRACE) < 0)
		perror("BLKSTOPTRACE");

	close(devfd);
}

static void extract_data(int cpu, char *ifn, int ifd, char *ofn, int ofd,
			 int nb)
{
	int ret, bytes_left;
	unsigned char *buf, *p;

	buf = malloc(nb);
	p = buf;
	bytes_left = nb;
	while (bytes_left > 0) {
		ret = read(ifd, p, bytes_left);
		if (!ret)
			usleep(1000);
		else if (ret < 0) {
			perror(ifn);
			fprintf(stderr, "Thread %d extract_data %s failed\n",
				cpu, ifn);
			free(buf);
			exit(1);
		} else {
			p += ret;
			bytes_left -= ret;
		}
	}

	ret = write(ofd, buf, nb);
	if (ret != nb) {
		perror(ofn);
		fprintf(stderr,"Thread %d extract_data %s failed\n", cpu, ofn);
		free(buf);
		exit(1);
	}

	free(buf);
}

static void *extract(void *arg)
{
	struct thread_information *tip = arg;
	int tracefd, ret, ofd, pdu_len;
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
				fprintf(stderr,"Thread %d misread %s %d,%d\n",
					tip->cpu, ip, ret, (int)sizeof(t));
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

		ret = write(ofd, &t, sizeof(t));
		if (ret < 0) {
			perror(op);
			fprintf(stderr,"Thread %d failed write of %s\n", 
				tip->cpu, op);
			exit(1);
		}

		if (pdu_len)
			extract_data(tip->cpu, ip, tracefd, dp, ofd, pdu_len);

		tip->events_processed++;
	}

	return NULL;
}

static int start_threads(void)
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

static void stop_threads(void)
{
	struct thread_information *tip = thread_information;
	int i;

	for (i = 0; i < ncpus; i++, tip++) {
		int ret;

		if (pthread_join(tip->thread, (void *) &ret))
			perror("thread_join");
	}
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

	if (act_mask_tmp != 0) {
		act_mask = act_mask_tmp;
		printf("Tracing 0x%04x: ", act_mask);
		for (i = 0; i < BLK_TC_SHIFT; i++)
			if (act_mask & (1 << i))
				printf("%s ", mask_maps[i].short_form);
		printf("\n");
	}

	if (stat(relay_path, &st) < 0) {
		fprintf(stderr,"%s does not appear to be mounted\n",
			relay_path);
		return 2;
	}

	if (start_trace(dev)) {
		fprintf(stderr, "Failed to start trace on %s\n", dev);
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

	stop_threads();
	stop_trace();
	close(devfd);
	show_stats();

	return 0;
}

