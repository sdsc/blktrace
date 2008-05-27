/*
 * blktrace output analysis: generate a timeline & gather statistics
 *
 * Copyright (C) 2006 Alan D. Brunelle <Alan.Brunelle@hp.com>
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

#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "globals.h"

#define SETBUFFER_SIZE	(64 * 1024)

#define S_OPTS	"aAB:d:D:e:hi:I:l:m:M:o:p:q:s:S:t:T:u:VvX"
static struct option l_opts[] = {
	{
		.name = "seek-absolute",
		.has_arg = no_argument,
		.flag = NULL,
		.val = 'a'
	},
	{
		.name = "all-data",
		.has_arg = no_argument,
		.flag = NULL,
		.val = 'A'
	},
	{
		.name = "dump-blocknos",
		.has_arg = required_argument,
		.flag = NULL,
		.val = 'B'
	},
	{
		.name = "range-delta",
		.has_arg = required_argument,
		.flag = NULL,
		.val = 'd'
	},
	{
		.name = "devices",
		.has_arg = required_argument,
		.flag = NULL,
		.val = 'D'
	},
	{
		.name = "exes",
		.has_arg = required_argument,
		.flag = NULL,
		.val = 'e'
	},
	{
		.name = "help",
		.has_arg = no_argument,
		.flag = NULL,
		.val = 'h'
	},
	{
		.name = "input-file",
		.has_arg = required_argument,
		.flag = NULL,
		.val = 'i'
	},
	{
		.name = "iostat",
		.has_arg = required_argument,
		.flag = NULL,
		.val = 'I'
	},
	{
		.name = "d2c-latencies",
		.has_arg = required_argument,
		.flag = NULL,
		.val = 'l'
	},
	{
		.name = "seeks-per-second",
		.has_arg = required_argument,
		.flag = NULL,
		.val = 'm'
	},
	{
		.name = "dev-maps",
		.has_arg = required_argument,
		.flag = NULL,
		.val = 'M'
	},
	{
		.name = "output-file",
		.has_arg = required_argument,
		.flag = NULL,
		.val = 'o'
	},
	{
		.name = "per-io-dump",
		.has_arg = required_argument,
		.flag = NULL,
		.val = 'p'
	},
	{
		.name = "q2c-latencies",
		.has_arg = required_argument,
		.flag = NULL,
		.val = 'q'
	},
	{
		.name = "seeks",
		.has_arg = required_argument,
		.flag = NULL,
		.val = 's'
	},
	{
		.name = "iostat-interval",
		.has_arg = required_argument,
		.flag = NULL,
		.val = 'S'
	},
	{
		.name = "time-start",
		.has_arg = required_argument,
		.flag = NULL,
		.val = 't'
	},
	{
		.name = "time-end",
		.has_arg = required_argument,
		.flag = NULL,
		.val = 'T'
	},
	{
		.name = "unplug-hist",
		.has_arg = required_argument,
		.flag = NULL,
		.val = 'u'
	},
	{
		.name = "version",
		.has_arg = no_argument,
		.flag = NULL,
		.val = 'V'
	},
	{
		.name = "verbose",
		.has_arg = no_argument,
		.flag = NULL,
		.val = 'v'
	},
	{
		.name = "easy-parse-avgs",
		.has_arg = no_argument,
		.flag = NULL,
		.val = 'X'
	},
	{
		.name = NULL,
	}
};

static char usage_str[] = \
	"\n[ -a               | --seek-absolute ]\n" \
	"[ -A               | --all-data ]\n" \
	"[ -B <output name> | --dump-blocknos=<output name> ]\n" \
	"[ -d <seconds>     | --range-delta=<seconds> ]\n" \
	"[ -D <dev;...>     | --devices=<dev;...> ]\n" \
	"[ -e <exe,...>     | --exes=<exe,...>  ]\n" \
	"[ -h               | --help ]\n" \
	"[ -i <input name>  | --input-file=<input name> ]\n" \
	"[ -I <output name> | --iostat=<output name> ]\n" \
	"[ -l <output name> | --d2c-latencies=<output name> ]\n" \
	"[ -m <output name> | --seeks-per-second=<output name> ]\n" \
	"[ -M <dev map>     | --dev-maps=<dev map>\n" \
	"[ -o <output name> | --output-file=<output name> ]\n" \
	"[ -p <output name> | --per-io-dump=<output name> ]\n" \
	"[ -q <output name> | --q2c-latencies=<output name> ]\n" \
	"[ -s <output name> | --seeks=<output name> ]\n" \
	"[ -S <interval>    | --iostat-interval=<interval> ]\n" \
	"[ -t <sec>         | --time-start=<sec> ]\n" \
	"[ -T <sec>         | --time-end=<sec> ]\n" \
	"[ -u <output name> | --unplug-hist=<output name> ]\n" \
	"[ -V               | --version ]\n" \
	"[ -v               | --verbose ]\n" \
	"[ -X               | --easy-parse-avgs ]\n" \
	"\n";

static struct file_info *arg_files = NULL;

static void usage(char *prog)
{
	fprintf(stderr, "Usage: %s %s %s", prog, bt_timeline_version,
		usage_str);
}

static FILE *setup_ofile(char *fname)
{
	if (fname) {
		char *buf;
		FILE *ofp = fopen(fname, "w");

		if (!ofp) {
			perror(fname);
			exit(1);
		}

		buf = malloc(SETBUFFER_SIZE);

		setbuffer(ofp, buf, SETBUFFER_SIZE);
		add_file(&arg_files, ofp, fname);
		add_buf(buf);
		return ofp;
	}

	return NULL;
}

void clean_args(void)
{
	clean_files(&arg_files);
}

void handle_args(int argc, char *argv[])
{
	int c;

	while ((c = getopt_long(argc, argv, S_OPTS, l_opts, NULL)) != -1) {
		switch (c) {
		case 'a':
			seek_absolute = 1;
			break;
		case 'A':
			output_all_data = 1;
			break;
		case 'B':
			bno_dump_name = strdup(optarg);
			break;
		case 'd':
			sscanf(optarg, "%lf", &range_delta);
			break;
		case 'D':
			devices = strdup(optarg);
			break;
		case 'e':
			exes = strdup(optarg);
			break;
		case 'h':
			usage(argv[0]);
			exit(0);
		case 'i':
			input_name = strdup(optarg);
			break;
		case 'l':
			d2c_name = strdup(optarg);
			break;
		case 'I':
			iostat_name = strdup(optarg);
			break;
		case 'm':
			sps_name = strdup(optarg);
			break;
		case 'M':
			if (dev_map_read(optarg))
				exit(1);
			break;
		case 'o':
			output_name = strdup(optarg);
			break;
		case 'p':
			per_io_name = strdup(optarg);
			break;
		case 'q':
			q2c_name = strdup(optarg);
			break;
		case 's':
			seek_name = strdup(optarg);
			break;
		case 'S': {
			unsigned int interval;
			sscanf(optarg, "%u", &interval);
			iostat_interval = (__u64)interval * 1000000000LL;
			break;
		}
		case 't':
			sscanf(optarg, "%lf", &t_astart);
			time_bounded = 1;
			break;
		case 'T':
			sscanf(optarg, "%lf", &t_aend);
			time_bounded = 1;
			break;
		case 'u':
			unplug_hist_name = strdup(optarg);
			break;
		case 'v':
			verbose = 1;
			break;
		case 'V':
			printf("%s version %s\n", argv[0], bt_timeline_version);
			exit(0);
		case 'X':
			easy_parse_avgs++;
			break;
		default:
			usage(argv[0]);
			exit(1);
		}
	}

	if (input_name == NULL) {
		usage(argv[0]);
		exit(1);
	}

	if (sps_name && !seek_name) {
		fprintf(stderr, "FATAL: -m option requires -s options\n");
		exit(1);
	}

	setup_ifile(input_name);

	if (output_name == NULL) {
		ranges_ofp = avgs_ofp = msgs_ofp = stdout;
		easy_parse_avgs = 0;
	}
	else {
		char *fname = malloc(strlen(output_name) + 32);

		sprintf(fname, "%s.dat", output_name);
		ranges_ofp = fopen(fname, "w");
		if (ranges_ofp == NULL) {
			perror(fname);
			exit(1);
		}
		if (verbose)
			printf("Sending range data to %s\n", fname);

		sprintf(fname, "%s.avg", output_name);
		avgs_ofp = fopen(fname, "w");
		if (avgs_ofp == NULL) {
			perror(fname);
			exit(1);
		}
		if (verbose)
			printf("Sending stats data to %s\n", fname);

		sprintf(fname, "%s.msg", output_name);
		msgs_ofp = fopen(fname, "w");
		if (msgs_ofp == NULL) {
			perror(fname);
			exit(1);
		}
		if (verbose)
			printf("Sending K messages to %s\n", fname);

		if (easy_parse_avgs) {
			sprintf(fname, "%s.xvg", output_name);
			xavgs_ofp = fopen(fname, "w");
			if (avgs_ofp == NULL) {
				perror(fname);
				exit(1);
			}
			if (verbose)
				printf("Sending X stats data to %s\n", fname);
		}

		free(fname);
	}

	iostat_ofp = setup_ofile(iostat_name);
	per_io_ofp = setup_ofile(per_io_name);
}
