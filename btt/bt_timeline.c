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

char *devices = NULL;
char *exes = NULL;
char *input_name = NULL;
char *output_name = NULL;
char *seek_name = NULL;
char *d2c_name = NULL;
char *q2c_name = NULL;
double range_delta = 0.1;
FILE *ranges_ofp, *avgs_ofp;
int verbose = 0;
int ifd;
unsigned long n_traces, n_io_allocs, n_io_frees;
struct avgs_info all_avgs;
__u64 last_q = (__u64)-1;
unsigned int pending_xs;

unsigned int n_devs;
LIST_HEAD(all_devs);
LIST_HEAD(all_ios);
LIST_HEAD(all_procs);
struct my_mem *free_ios = NULL;
struct my_mem *free_bits = NULL;

struct region_info all_regions = {
	.qranges = LIST_HEAD_INIT(all_regions.qranges),
	.cranges = LIST_HEAD_INIT(all_regions.cranges),
	.qr_cur = NULL,
	.cr_cur = NULL
};

char iop_map[] = { 'Q', 'X', 'A', 'M', 'I', 'D', 'C', 'Y' };

struct blk_io_trace *convert_to_cpu(struct blk_io_trace *t);
int process(void);

int main(int argc, char *argv[])
{
	handle_args(argc, argv);

	cy_init();
	iostat_init();
	if (process() || output_avgs(avgs_ofp) || output_ranges(ranges_ofp))
		return 1;

	return 0;
}

int process(void)
{
	int ret = 0;
	struct blk_io_trace *t;
	struct io *iop = IO_ZALLOC();

	while (!do_read(ifd, &iop->t, sizeof(iop->t))) {
		t = convert_to_cpu(&iop->t);
		if (t->pdu_len > 0) {
			iop->pdu = malloc(t->pdu_len);
			if (do_read(ifd, iop->pdu, t->pdu_len)) {
				free(iop->pdu);
				ret = 1;
				break;
			}
		}
		add_trace(iop);
		iop = IO_ZALLOC();
	}
	IO_FREE(iop);

	if (iostat_ofp) {
		fprintf(iostat_ofp, "\n");
		iostat_dump_stats(iostat_last_stamp, 1);
	}

	seek_clean();
	latency_clean();

	if (verbose)
		printf("\n%10lu traces, %10lu mallocs %1lu frees\n",
				       n_traces, n_io_allocs, n_io_frees);

	cy_shutdown();
	return ret;
}
