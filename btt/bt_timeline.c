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
#include <sys/time.h>
#include <time.h>
#include "globals.h"

char bt_timeline_version[] = "0.99.1";

char *devices, *exes, *input_name, *output_name, *seek_name, *bno_dump_name;
char *d2c_name, *q2c_name, *per_io_name, *unplug_hist_name;
FILE *ranges_ofp, *avgs_ofp, *per_io_ofp;
int verbose, done, time_bounded, output_all_data, seek_absolute;
double t_astart, t_aend;
unsigned long n_traces;
struct avgs_info all_avgs;
unsigned int n_devs;
time_t genesis, last_vtrace;
LIST_HEAD(all_devs);
LIST_HEAD(all_procs);
LIST_HEAD(free_ios);
LIST_HEAD(free_bilinks);
LIST_HEAD(rmhd);
LIST_HEAD(retries);
__u64 q_histo[N_HIST_BKTS], d_histo[N_HIST_BKTS];

double range_delta = 0.1;
__u64 last_q = (__u64)-1;
__u64 next_retry_check = 0;

struct region_info all_regions = {
	.qranges = LIST_HEAD_INIT(all_regions.qranges),
	.cranges = LIST_HEAD_INIT(all_regions.cranges),
};

#if defined(DEBUG)
	int rb_tree_size;
#endif

#if defined(COUNT_IOS)
unsigned long nios_reused, nios_alloced, nios_freed;
LIST_HEAD(cios);
#endif

int process(void);

int main(int argc, char *argv[])
{
	handle_args(argc, argv);

	init_dev_heads();
	iostat_init();
	if (process() || output_avgs(avgs_ofp) || output_ranges(ranges_ofp))
		return 1;

	if (iostat_ofp) {
		fprintf(iostat_ofp, "\n");
		iostat_dump_stats(iostat_last_stamp, 1);
	}

	if (ranges_ofp != stdout) 
		fclose(ranges_ofp);
	if (avgs_ofp != stdout) 
		fclose(avgs_ofp);

	seek_clean();
	latency_clean();
	bno_dump_clean();
	dev_map_exit();
	dip_exit();
	pip_exit();
	io_free_all();
	bilink_free_all();
	region_exit(&all_regions);

	free(input_name);
	if (output_name) free(output_name);

	clean_bufs();

	return 0;
}

static inline double tv2dbl(struct timeval *tv)
{
	return (double)tv->tv_sec + (((double)tv->tv_usec) / (1000.0 * 1000.0));
}

int process(void)
{
	int ret = 0;
	struct io *iop = io_alloc();
	struct timeval tvs, tve;

	genesis = last_vtrace = time(NULL);
	gettimeofday(&tvs, NULL);
	while (!done && next_trace(&iop->t, &iop->pdu)) {
		add_trace(iop);
		iop = io_alloc();
	}

	io_release(iop);
	do_retries(0);
	gettimeofday(&tve, NULL);

	if (verbose) {
		double tps, dt_input = tv2dbl(&tve) - tv2dbl(&tvs);
		
		tps = (double)n_traces / dt_input;
		printf("\r                                        "
		       "                                        \r");
		printf("%10lu traces @ %.1lf Ktps in %.6lf seconds\n",
			n_traces, tps/1000.0,
			dt_input);

#		if defined(DEBUG)
			printf("\ttree = |%d|\n", rb_tree_size);
			if (rb_tree_size > 0)
				dump_rb_trees();
#		endif

#		if defined(COUNT_IOS)
			{
				struct io *_iop;
				struct list_head *_p;
				FILE *_ofp = fopen("cios.txt", "w");
				printf("(%ld + %ld) = %ld - %ld = %ld\n", 
					nios_alloced, nios_reused, 
					nios_alloced + nios_reused, 
					nios_freed, 
					(nios_alloced + nios_reused) 
								- nios_freed);

				__list_for_each(_p, &cios) {
					_iop = list_entry(_p, struct io, 
								cio_head);
					__dump_iop(_ofp, _iop, 0);
				}
				fclose(_ofp);
			}
#		endif
	}

	return ret;
}
