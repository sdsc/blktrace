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

#define S_OPTS	"d:D:e:hi:I:l:M:o:p:q:s:S:t:T:Vv"
static struct option l_opts[] = {
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
		.name = NULL,
	}
};

static char usage_str[] = \
	"\n[ -d <seconds>     | --range-delta=<seconds> ]\n" \
	"[ -D <dev;...>     | --devices=<dev;...> ]\n" \
	"[ -e <exe,...>     | --exes=<exe,...>  ]\n" \
	"[ -h               | --help ]\n" \
	"[ -i <input name>  | --input-file=<input name> ]\n" \
	"[ -I <output name> | --iostat=<output name> ]\n" \
	"[ -l <output name> | --d2c-latencies=<output name> ]\n" \
	"[ -M <dev map>     | --dev-maps=<dev map>\n" \
	"[ -o <output name> | --output-file=<output name> ]\n" \
	"[ -p <output name> | --per-io-dump=<output name> ]\n" \
	"[ -q <output name> | --q2c-latencies=<output name> ]\n" \
	"[ -s <output name> | --seeks=<output name> ]\n" \
	"[ -S <interval>    | --iostat-interval=<interval> ]\n" \
	"[ -t <sec>         | --time-start=<sec> ]\n" \
	"[ -T <sec>         | --time-end=<sec> ]\n" \
	"[ -V               | --version ]\n" \
	"[ -v               | --verbose ]\n\n";

static void usage(char *prog)
{
	fprintf(stderr, "Usage: %s %s %s", prog, bt_timeline_version,
		usage_str);
}

void handle_args(int argc, char *argv[])
{
	int c;

	while ((c = getopt_long(argc, argv, S_OPTS, l_opts, NULL)) != -1) {
		switch (c) {
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
		case 'v':
			verbose = 1;
			break;
		case 'V':
			printf("%s version %s\n", argv[0], bt_timeline_version);
			exit(0);
		default:
			usage(argv[0]);
			exit(1);
		}
	}

	if (input_name == NULL) {
		usage(argv[0]);
		exit(1);
	}

	setup_ifile(input_name);

	if (output_name == NULL)
		ranges_ofp = avgs_ofp = stdout;
	else {
		char *fname = malloc(strlen(output_name) + 32);

		sprintf(fname, "%s.dat", output_name);
		ranges_ofp = fopen(fname, "w");
		if (ranges_ofp == NULL) {
			perror(fname);
			exit(1);
		}
		if (verbose)
			printf("Sending range data to %s.dat\n", output_name);

		sprintf(fname, "%s.avg", output_name);
		avgs_ofp = fopen(fname, "w");
		if (avgs_ofp == NULL) {
			perror(fname);
			exit(1);
		}
		if (verbose)
			printf("Sending stats data to %s.avg\n", output_name);

		free(fname);
	}

	if (iostat_name != NULL) {
		iostat_ofp = fopen(iostat_name, "w");
		if (iostat_ofp == NULL) {
			perror(iostat_name);
			exit(1);
		}
		setbuffer(iostat_ofp, malloc(64 * 1024), 64 * 1024);
	}

	if (per_io_name != NULL) {
		per_io_ofp = fopen(per_io_name, "w");
		if (per_io_ofp == NULL) {
			perror(per_io_name);
			exit(1);
		}
		setbuffer(per_io_ofp, malloc(64 * 1024), 64 * 1024);
	}
}
