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

#define S_OPTS	"d:D:e:hlmM:i:o:Vv"
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
		.name = "lvm",
		.has_arg = no_argument,
		.flag = NULL,
		.val = 'l'
	},
	{
		.name = "md",
		.has_arg = no_argument,
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
		.name = "input-file",
		.has_arg = required_argument,
		.flag = NULL,
		.val = 'i'
	},
	{
		.name = "output-file",
		.has_arg = required_argument,
		.flag = NULL,
		.val = 'o'
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
	"[ -e <exe,...>     | --exes=<exe,...>  ]\n" \
	"[ -h               | --help ]\n" \
	"[ -i <input name>  | --input-file=<input name> ]\n" \
	"(-l | -m)          | (--lvm | -md)\n" \
	"[ -o <output name> | --output-file=<output name> ]\n" \
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
	char *dev_map_fname = NULL;

	while ((c = getopt_long(argc, argv, S_OPTS, l_opts, NULL)) != -1) {
		switch (c) {
		case 'd':
			sscanf(optarg, "%lf", &range_delta);
			break;
		case 'D':
			devices = optarg;
			break;
		case 'e':
			exes = optarg;
			break;
		case 'h':
			usage(argv[0]);
			exit(0);
		case 'i':
			input_name = optarg;
			break;
		case 'l':
			is_lvm = 1;
			break;
		case 'm':
			is_lvm = 0;
			break;
		case 'M':
			dev_map_fname = optarg;
			break;
		case 'o':
			output_name = optarg;
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

	if (input_name == NULL || is_lvm < 0) {
		usage(argv[0]);
		exit(1);
	}

	ifd = open(input_name, O_RDONLY);
	if (ifd < 0) {
		perror(input_name);
		exit(1);
	}

	if (dev_map_fname && dev_map_read(dev_map_fname))
		exit(1);

	if (output_name == NULL)
		ranges_ofp = avgs_ofp = stdout;
	else {
		char *fname = malloc(sizeof(output_name) + 20);

		sprintf(fname, "%s.dat", output_name);
		ranges_ofp = fopen(fname, "w");
		if (ranges_ofp == NULL) {
			perror(fname);
			exit(1);
		}
		if (verbose)
			printf("Sending range data to %s\n", output_name);

		sprintf(fname, "%s.avg", output_name);
		avgs_ofp = fopen(fname, "w");
		if (avgs_ofp == NULL) {
			perror(fname);
			exit(1);
		}
		if (verbose)
			printf("Sending stats data to %s\n", output_name);

		free(fname);
	}
}
