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
#include <signal.h>
#include <sys/wait.h>

#include "plot.h"
#include "blkparse.h"
#include "list.h"

static int line_len = 1024;
static char line[1024];

static pid_t blktrace_pid = 0;
static pid_t mpstat_pid = 0;

char *blktrace_args[17 + MAX_DEVICES_PER_TRACE * 2] = {
	"blktrace",
	"-b", "8192",
	"-a", "queue",
	"-a", "complete",
	"-a", "issue",
	"-a", "notify",
	"-D", ".",
	NULL,
};

char *mpstat_args[] = {
	"mpstat",
	"-P", "ALL", "1",
	NULL,
};

#define DEST_DIR_INDEX 12
#define LAST_ARG 13

int stop_tracer(pid_t *tracer_pid)
{
	int ret;
	pid_t pid = *tracer_pid;
	pid_t pid_ret;
	int status = 0;

	if (pid == 0)
		return 0;

	*tracer_pid = 0;
	ret = kill(pid, SIGTERM);
	if (ret) {
		fprintf(stderr, "failed to stop tracer pid %lu error %s\n",
			(unsigned long)pid, strerror(errno));
		return -errno;
	}
	pid_ret = waitpid(pid, &status, WUNTRACED);
	if (pid_ret == pid && WIFEXITED(status) == 0) {
		fprintf(stderr, "blktrace returns error %d\n", WEXITSTATUS(status));
	}
	return 0;
}


void stop_all_tracers(void)
{
	stop_tracer(&blktrace_pid);
	stop_tracer(&mpstat_pid);
}

void sig_handler_for_quit(int val)
{
	fprintf(stderr, "iowatcher exiting with %d, stopping tracers\n", val);
	stop_all_tracers();
}


int start_blktrace(char **devices, int num_devices, char *trace_name, char *dest)
{
	pid_t pid;
	int ret;
	char **arg = blktrace_args;
	int i;
	int arg_index;

	fprintf(stderr, "running blktrace");
	if (!trace_name)
		trace_name = "trace";

	arg_index = LAST_ARG;
	for (i = 0; i < num_devices; i++) {
		blktrace_args[arg_index++] = "-d";
		blktrace_args[arg_index++] = devices[i];
	}

	/*
	 * single device traces use -o and are put into
	 * the dest dir if provided
	 */
	if (num_devices == 1) {
		blktrace_args[arg_index++] = "-o";
		blktrace_args[arg_index++] = trace_name;
		if (dest)
			blktrace_args[DEST_DIR_INDEX] = dest;
	} else {
		/*
		 * multi device traces are put into a dest
		 * dir based on the trace name
		 */
		blktrace_args[DEST_DIR_INDEX] = trace_name;
	}

	blktrace_args[arg_index] = NULL;

	while(*arg) {
		fprintf(stderr, " %s", *arg);
		arg++;
	}
	fprintf(stderr, "\n");


	pid = fork();
	if (pid == 0) {
		ret = execvp("blktrace", blktrace_args);
		if (ret) {
			fprintf(stderr, "failed to exec blktrace error %s\n", strerror(errno));
			exit(errno);
		}

	} else {
		blktrace_pid = pid;
		signal(SIGTERM, sig_handler_for_quit);
		signal(SIGINT, sig_handler_for_quit);
	}
	return 0;
}

int run_program(char *str)
{
	int ret;

	fprintf(stderr, "running program %s\n", str);
	ret = system(str);
	if (ret == -1) {
		fprintf(stderr, "failed to run program %s error %s\n", str, strerror(errno));
		stop_all_tracers();
		return -errno;
	}
	stop_all_tracers();
	return 0;
}

int wait_for_tracers(void)
{
	int status = 0;
	if (blktrace_pid != 0) {
		waitpid(blktrace_pid, &status, WUNTRACED);
		blktrace_pid = 0;
	}
	if (mpstat_pid != 0) {
		waitpid(mpstat_pid, &status, WUNTRACED);
		mpstat_pid = 0;
	}
	return 0;
}

int blktrace_to_dump(char *trace_name)
{
	snprintf(line, line_len, "blkparse -O -i %s -d '%s.%s'",
		trace_name, trace_name, "dump");

	system(line);
	return 0;
}

int start_mpstat(char *trace_name)
{
	int fd;
	pid_t pid;

	snprintf(line, line_len, "%s.mpstat", trace_name);

	fd = open(line, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) {
		fprintf(stderr, "unable to open %s for writing err %s\n",
			line, strerror(errno));
		exit(1);
	}
	pid = fork();
	if (pid == 0) {
		int ret;

		close(1);
		ret = dup2(fd, 1);
		if (ret < 0) {
			fprintf(stderr, "failed to setup output file for mpstat\n");
			exit(1);
		}
		ret = execvp("mpstat", mpstat_args);
		if (ret < 0) {
			fprintf(stderr, "failed to exec mpstat err %s\n",
				strerror(ret));
			exit(1);
		}
	}
	mpstat_pid = pid;
	return 0;
}
