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
#include <string.h>
#include <unistd.h>
#include "globals.h"

char bt_timeline_version[] = "0.99";

char *devices, *exes, *input_name, *output_name, *seek_name;
char *d2c_name, *q2c_name, *per_io_name;
FILE *ranges_ofp, *avgs_ofp, *per_io_ofp;
int ifd, verbose, done, time_bounded;
double t_astart, t_aend;
unsigned long n_traces;
struct avgs_info all_avgs;
unsigned int n_devs;
time_t genesis, last_vtrace;
LIST_HEAD(all_devs);
LIST_HEAD(all_procs);
LIST_HEAD(free_ios);

double range_delta = 0.1;
__u64 last_q = (__u64)-1;

struct region_info all_regions = {
	.qranges = LIST_HEAD_INIT(all_regions.qranges),
	.cranges = LIST_HEAD_INIT(all_regions.cranges),
	.qr_cur = NULL,
	.cr_cur = NULL
};

int process(void);

int main(int argc, char *argv[])
{
	handle_args(argc, argv);

	init_dev_heads();
	iostat_init();
	if (process() || output_avgs(avgs_ofp) || output_ranges(ranges_ofp))
		return 1;

	return 0;
}

int process(void)
{
	int ret = 0;
	struct blk_io_trace *t;
	struct io *iop = io_alloc();

	genesis = last_vtrace = time(NULL);
	while (!done && !do_read(ifd, &iop->t, sizeof(struct blk_io_trace))) {
		t = convert_to_cpu(&iop->t);
		if (t->pdu_len > 0) {
			iop->pdu = malloc(t->pdu_len);
			if (do_read(ifd, iop->pdu, t->pdu_len)) {
				ret = 1;
				break;
			}
		}
		add_trace(iop);
		iop = io_alloc();
	}

	io_release(iop);
	do_retries();

	if (iostat_ofp) {
		fprintf(iostat_ofp, "\n");
		iostat_dump_stats(iostat_last_stamp, 1);
	}

	seek_clean();
	latency_clean();

	if (verbose) {
		double tps = (double)n_traces / (double)(time(NULL) - genesis);
		printf("%10lu traces @ %.1lf Ktps\n", n_traces, tps/1000.0);
	}

	return ret;
}
