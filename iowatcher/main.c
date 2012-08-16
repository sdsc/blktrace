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
#include <math.h>
#include <getopt.h>

#include "plot.h"
#include "blkparse.h"
#include "list.h"
#include "tracers.h"
#include "mpstat.h"

LIST_HEAD(all_traces);

static int found_mpstat = 0;
static int cpu_color_index = 0;
static int color_index = 0;
char *colors[] = {
	"blue", "darkgreen",
	"red", "aqua",
	"orange", "darkviolet",
	"brown", "#00FF00",
	"yellow", "coral",
	"black", "darkred",
	"fuchsia", "crimson",
	NULL };

char *pick_color(void) {
	char *ret = colors[color_index];
	if (!ret) {
		color_index = 0;
		ret = colors[color_index];
	}
	color_index++;
	return ret;
}

char *pick_cpu_color(void) {
	char *ret = colors[cpu_color_index];
	if (!ret) {
		color_index = 0;
		ret = colors[cpu_color_index];
	}
	cpu_color_index++;
	return ret;
}

enum {
	IO_GRAPH_INDEX = 0,
	TPUT_GRAPH_INDEX,
	CPU_SYS_GRAPH_INDEX,
	CPU_IO_GRAPH_INDEX,
	CPU_IRQ_GRAPH_INDEX,
	CPU_SOFT_GRAPH_INDEX,
	CPU_USER_GRAPH_INDEX,
	LATENCY_GRAPH_INDEX,
	QUEUE_DEPTH_GRAPH_INDEX,
	IOPS_GRAPH_INDEX,
	TOTAL_GRAPHS
};

enum {
	MPSTAT_SYS = 0,
	MPSTAT_IRQ,
	MPSTAT_IO,
	MPSTAT_SOFT,
	MPSTAT_USER,
	MPSTAT_GRAPHS
};

static char *graphs_by_name[] = {
	"io",
	"tput",
	"cpu-sys",
	"cpu-io",
	"cpu-irq",
	"cpu-soft",
	"cpu-user",
	"latency",
	"queue-depth",
	"iops",
};

static int active_graphs[TOTAL_GRAPHS];
static int last_active_graph = IOPS_GRAPH_INDEX;

static int label_index = 0;
static int num_traces = 0;
static int longest_label = 0;

struct trace_file {
	struct list_head list;
	char *filename;
	char *label;
	struct trace *trace;
	int seconds;
	int stop_seconds;
	u64 max_offset;

	char *read_color;
	char *write_color;

	struct graph_line_data *tput_gld;
	struct graph_line_data *iop_gld;
	struct graph_line_data *latency_gld;
	struct graph_line_data *queue_depth_gld;
	struct graph_dot_data *gdd_writes;
	struct graph_dot_data *gdd_reads;

	int mpstat_seconds;
	int mpstat_stop_seconds;
	struct graph_line_data **mpstat_gld;
};

static void alloc_mpstat_gld(struct trace_file *tf)
{
	struct graph_line_data **ptr;

	if (tf->trace->mpstat_num_cpus == 0)
		return;

	ptr = calloc((tf->trace->mpstat_num_cpus + 1) * MPSTAT_GRAPHS,
		     sizeof(struct graph_line_data *));
	if (!ptr) {
		perror("Unable to allocate mpstat arrays\n");
		exit(1);
	}
	tf->mpstat_gld = ptr;
}

static void enable_all_graphs(void)
{
	int i;
	for (i = 0; i < TOTAL_GRAPHS; i++)
		active_graphs[i] = 1;
}

static void disable_all_graphs(void)
{
	int i;
	for (i = 0; i < TOTAL_GRAPHS; i++)
		active_graphs[i] = 0;
}

static int enable_one_graph(char *name)
{
	int i;
	for (i = 0; i < TOTAL_GRAPHS; i++) {
		if (strcmp(name, graphs_by_name[i]) == 0) {
			active_graphs[i] = 1;
			return 0;
		}
	}
	return -ENOENT;
}

static int disable_one_graph(char *name)
{
	int i;
	for (i = 0; i < TOTAL_GRAPHS; i++) {
		if (strcmp(name, graphs_by_name[i]) == 0) {
			active_graphs[i] = 0;
			return 0;
		}
	}
	return -ENOENT;
}

static int last_graph(void)
{
	int i;
	for (i = TOTAL_GRAPHS - 1; i >= 0; i--) {
		if (active_graphs[i]) {
			return i;
		}
	}
	return -ENOENT;
}
static void add_trace_file(char *filename)
{
	struct trace_file *tf;

	tf = calloc(1, sizeof(*tf));
	if (!tf) {
		fprintf(stderr, "Unable to allocate memory\n");
		exit(1);
	}
	tf->label = "";
	tf->filename = strdup(filename);
	list_add_tail(&tf->list, &all_traces);
	tf->read_color = pick_color();
	tf->write_color = pick_color();
	num_traces++;
}

static void setup_trace_file_graphs(void)
{
	struct trace_file *tf;
	int i;

	list_for_each_entry(tf, &all_traces, list) {
		tf->tput_gld = alloc_line_data(tf->seconds, tf->stop_seconds);
		tf->latency_gld = alloc_line_data(tf->seconds, tf->stop_seconds);
		tf->queue_depth_gld = alloc_line_data(tf->seconds, tf->stop_seconds);
		tf->iop_gld = alloc_line_data(tf->seconds, tf->stop_seconds);
		tf->gdd_writes = alloc_dot_data(tf->seconds, tf->max_offset, tf->stop_seconds);
		tf->gdd_reads = alloc_dot_data(tf->seconds, tf->max_offset, tf->stop_seconds);

		if (tf->trace->mpstat_num_cpus == 0)
			continue;

		alloc_mpstat_gld(tf);
		for (i = 0; i < (tf->trace->mpstat_num_cpus + 1) * MPSTAT_GRAPHS; i++) {
			tf->mpstat_gld[i] =
				alloc_line_data(tf->mpstat_seconds,
						tf->mpstat_seconds);
			tf->mpstat_gld[i]->max = 100;
		}
	}
}

static void read_traces(void)
{
	struct trace_file *tf;
	struct trace *trace;
	u64 last_time;
	u64 ymin;
	u64 ymax;

	list_for_each_entry(tf, &all_traces, list) {
		trace = open_trace(tf->filename);
		if (!trace)
			exit(1);

		last_time = find_last_time(trace);
		tf->trace = trace;
		tf->seconds = SECONDS(last_time);
		tf->stop_seconds = SECONDS(last_time);
		tf->max_offset = find_highest_offset(trace);

		filter_outliers(trace, tf->max_offset, &ymin, &ymax);
		tf->max_offset = ymax;

		read_mpstat(trace, tf->filename);
		tf->mpstat_stop_seconds = trace->mpstat_seconds;
		tf->mpstat_seconds = trace->mpstat_seconds;
		if (tf->mpstat_seconds)
			found_mpstat = 1;
	}
}

static void read_trace_events(void)
{

	struct trace_file *tf;
	struct trace *trace;
	int ret;
	int i;
	int time;
	double user, sys, iowait, irq, soft;
	double max_user = 0, max_sys = 0, max_iowait = 0,
	       max_irq = 0, max_soft = 0;

	list_for_each_entry(tf, &all_traces, list) {
		trace = tf->trace;
		first_record(trace);
		while (1) {
			check_record(trace);
			add_tput(trace, tf->tput_gld);
			add_iop(trace, tf->iop_gld);
			add_io(trace, tf->gdd_writes, tf->gdd_reads);
			add_pending_io(trace, tf->queue_depth_gld);
			add_completed_io(trace, tf->latency_gld);
			ret = next_record(trace);
			if (ret)
				break;
		}
	}
	list_for_each_entry(tf, &all_traces, list) {
		trace = tf->trace;

		if (trace->mpstat_num_cpus == 0)
			continue;

		first_mpstat(trace);

		for (time = 0; time < tf->mpstat_stop_seconds; time ++) {
			for (i = 0; i < (trace->mpstat_num_cpus + 1) * MPSTAT_GRAPHS; i += MPSTAT_GRAPHS) {
				ret = read_mpstat_event(trace, &user, &sys,
							&iowait, &irq, &soft);
				if (ret)
					goto mpstat_done;
				if (next_mpstat_line(trace))
					goto mpstat_done;

				if (sys > max_sys)
					max_sys = sys;
				if (user > max_user)
					max_user = user;
				if (irq > max_irq)
					max_irq = irq;
				if (iowait > max_iowait)
					max_iowait = iowait;

				add_mpstat_gld(time, sys, tf->mpstat_gld[i + MPSTAT_SYS]);
				add_mpstat_gld(time, irq, tf->mpstat_gld[i + MPSTAT_IRQ]);
				add_mpstat_gld(time, soft, tf->mpstat_gld[i + MPSTAT_SOFT]);
				add_mpstat_gld(time, user, tf->mpstat_gld[i + MPSTAT_USER]);
				add_mpstat_gld(time, iowait, tf->mpstat_gld[i + MPSTAT_IO]);
			}
			if (next_mpstat(trace) == NULL)
				break;
		}
	}

mpstat_done:
	list_for_each_entry(tf, &all_traces, list) {
		trace = tf->trace;

		if (trace->mpstat_num_cpus == 0)
			continue;

		tf->mpstat_gld[MPSTAT_SYS]->max = max_sys;
		tf->mpstat_gld[MPSTAT_IRQ]->max = max_irq;
		tf->mpstat_gld[MPSTAT_SOFT]->max = max_soft;
		tf->mpstat_gld[MPSTAT_USER]->max = max_user;
		tf->mpstat_gld[MPSTAT_IO]->max = max_iowait;;
	}
	return;
}

static void set_trace_label(char *label)
{
	int cur = 0;
	struct trace_file *tf;
	int len = strlen(label);

	if (len > longest_label)
		longest_label = len;

	list_for_each_entry(tf, &all_traces, list) {
		if (cur == label_index) {
			tf->label = strdup(label);
			label_index++;
			break;
		}
		cur++;
	}
}

static char *graph_title = "";
static char *output_filename = "trace.svg";
static char *blktrace_device = NULL;
static char *blktrace_outfile = "trace";
static char *blktrace_dest_dir = ".";
static char *program_to_run = NULL;

static void set_blktrace_outfile(char *arg)
{
	char *s = strdup(arg);
	char *last_dot = strrchr(s, '.');

	if (last_dot) {
		if (strcmp(last_dot, ".dump") == 0)
			*last_dot = '\0';
	}
	blktrace_outfile = s;
}


char *option_string = "hT:t:o:l:r:O:N:d:p:";
static struct option long_options[] = {
	{"title", required_argument, 0, 'T'},
	{"trace", required_argument, 0, 't'},
	{"output", required_argument, 0, 'o'},
	{"label", required_argument, 0, 'l'},
	{"rolling", required_argument, 0, 'r'},
	{"no-graph", required_argument, 0, 'N'},
	{"only-graph", required_argument, 0, 'O'},
	{"device", required_argument, 0, 'd'},
	{"prog", required_argument, 0, 'p'},
	{"help", required_argument, 0, 'h'},
	{0, 0, 0, 0}
};

static void print_usage(void)
{
	fprintf(stderr, "iowatcher usage:\n"
		"\t-d (--device): device for blktrace to trace\n"
		"\t-t (--trace): trace file name (more than one allowed)\n"
		"\t-l (--label): trace label in the graph\n"
		"\t-o (--output): output file name (SVG only)\n"
		"\t-p (--prog): program to run while blktrace is run\n"
		"\t-r (--rolling): number of seconds in the rolling averge\n"
		"\t-T (--title): graph title\n"
		"\t-N (--no-graph): skip a single graph (io, tput, latency, queue_depth, iops)\n"
		"\t-O (--only-graph): add a single graph (io, tput, latency, queue_depth, iops)\n"
	       );
	exit(1);
}

static int parse_options(int ac, char **av)
{
	int c;
	int disabled = 0;

	while (1) {
		// int this_option_optind = optind ? optind : 1;
		int option_index = 0;

		c = getopt_long(ac, av, option_string,
				long_options, &option_index);

		if (c == -1)
			break;

		switch(c) {
		case 'h':
			print_usage();
			break;
		case 'T':
			graph_title = strdup(optarg);
			break;
		case 't':
			add_trace_file(optarg);
			set_blktrace_outfile(optarg);
			break;
		case 'o':
			output_filename = strdup(optarg);
			break;
		case 'l':
			set_trace_label(optarg);
			break;
		case 'r':
			set_rolling_avg(atoi(optarg));
			break;
		case 'O':
			if (!disabled) {
				disable_all_graphs();
				disabled = 1;
			}
			enable_one_graph(optarg);
			break;
		case 'N':
			disable_one_graph(optarg);
			break;
		case 'd':
			blktrace_device = strdup(optarg);
			break;
		case 'p':
			program_to_run = strdup(optarg);
			break;
		case '?':
			print_usage();
			break;
		default:
			break;
		}
	}
	return 0;
}

static void compare_max_tf(struct trace_file *tf, int *seconds, u64 *max_offset)
{
	if (tf->seconds > *seconds)
		*seconds = tf->seconds;
	if (tf->max_offset > *max_offset)
		*max_offset = tf->max_offset;
}

static void set_all_max_tf(int seconds, u64 max_offset)
{
	struct trace_file *tf;

	list_for_each_entry(tf, &all_traces, list) {
		tf->seconds = seconds;
		tf->max_offset = max_offset;
	}
}

static void plot_io(struct plot *plot, int seconds, u64 max_offset)
{
	struct trace_file *tf;

	if (active_graphs[IO_GRAPH_INDEX] == 0)
		return;

	plot->add_xlabel = last_active_graph == IO_GRAPH_INDEX;
	setup_axis(plot);

	svg_alloc_legend(plot, num_traces * 2);

	set_plot_label(plot, "Device IO");
	set_ylabel(plot, "Offset (MB)");
	set_yticks(plot, 4, 0, max_offset / (1024 * 1024), "");
	set_xticks(plot, 9, 0, seconds);

	list_for_each_entry(tf, &all_traces, list) {
		char *label = tf->label;

		if (!label)
			label = "";
		svg_io_graph(plot, tf->gdd_reads, tf->read_color);
		if (tf->gdd_reads->total_ios)
			svg_add_legend(plot, label, " Reads", tf->read_color);

		svg_io_graph(plot, tf->gdd_writes, tf->write_color);
		if (tf->gdd_writes->total_ios) {
			svg_add_legend(plot, label, " Writes", tf->write_color);
		}
	}
	if (plot->add_xlabel)
		set_xlabel(plot, "Time (seconds)");
	svg_write_legend(plot);
	close_plot(plot);
}

static void plot_tput(struct plot *plot, int seconds)
{
	struct trace_file *tf;
	char *units;
	char line[128];
	u64 max = 0;

	if (active_graphs[TPUT_GRAPH_INDEX] == 0)
		return;

	if (num_traces > 1)
		svg_alloc_legend(plot, num_traces);
	list_for_each_entry(tf, &all_traces, list) {
		if (tf->tput_gld->max > max)
			max = tf->tput_gld->max;
	}
	list_for_each_entry(tf, &all_traces, list)
		tf->tput_gld->max = max;

	plot->add_xlabel = last_active_graph == TPUT_GRAPH_INDEX;
	setup_axis(plot);
	set_plot_label(plot, "Throughput");

	tf = list_entry(all_traces.next, struct trace_file, list);

	scale_line_graph_bytes(&max, &units, 1024);
	sprintf(line, "%sB/s", units);
	set_ylabel(plot, line);
	set_yticks(plot, 4, 0, max, "");
	set_xticks(plot, 9, 0, seconds);

	list_for_each_entry(tf, &all_traces, list) {
		svg_line_graph(plot, tf->tput_gld, tf->read_color);
		if (num_traces > 1)
			svg_add_legend(plot, tf->label, "", tf->read_color);
	}

	if (plot->add_xlabel)
		set_xlabel(plot, "Time (seconds)");
	if (num_traces > 1)
		svg_write_legend(plot);
	close_plot(plot);
}

static void plot_cpu(struct plot *plot, int seconds, char *label,
		     int active_index, int gld_index)
{
	struct trace_file *tf;
	char line[128];
	int max = 0;
	int i;
	int gld_i;
	char *color;
	double avg = 0;
	int ymax;
	int plotted = 0;

	if (active_graphs[active_index] == 0)
		return;

	list_for_each_entry(tf, &all_traces, list) {
		if (tf->trace->mpstat_num_cpus > max)
			max = tf->trace->mpstat_num_cpus;
	}
	if (max == 0)
		return;

	tf = list_entry(all_traces.next, struct trace_file, list);

	ymax = tf->mpstat_gld[gld_index]->max;
	if (ymax == 0)
		return;

	svg_alloc_legend(plot, num_traces * max);

	plot->add_xlabel = last_active_graph == active_index;
	setup_axis(plot);
	set_plot_label(plot, label);

	seconds = tf->mpstat_seconds;

	set_yticks(plot, 4, 0, tf->mpstat_gld[gld_index]->max, "");
	set_ylabel(plot, "Percent");
	set_xticks(plot, 9, 0, seconds);

	cpu_color_index = 0;
	list_for_each_entry(tf, &all_traces, list) {
		for (i = 0; i < tf->mpstat_gld[0]->stop_seconds; i++) {
			avg += tf->mpstat_gld[gld_index]->data[i].sum;
		}
		avg /= tf->mpstat_gld[gld_index]->stop_seconds;
		color = pick_cpu_color();
		svg_line_graph(plot, tf->mpstat_gld[0], color);
		svg_add_legend(plot, tf->label, " avg", color);

		for (i = 1; i < tf->trace->mpstat_num_cpus + 1; i++) {
			struct graph_line_data *gld = tf->mpstat_gld[i * MPSTAT_GRAPHS + gld_index];
			double this_avg = 0;

			for (gld_i = 0; gld_i < gld->stop_seconds; gld_i++)
				this_avg += gld->data[i].sum;

			this_avg /= gld->stop_seconds;

			for (gld_i = 0; gld_i < gld->stop_seconds; gld_i++) {
				if (this_avg > avg + 30 ||
				    gld->data[gld_i].sum > 95) {
					color = pick_cpu_color();
					svg_line_graph(plot, gld, color);
					snprintf(line, 128, " CPU %d\n", i - 1);
					svg_add_legend(plot, tf->label, line, color);
					plotted++;
					break;
				}

			}
		}
	}

	if (plot->add_xlabel)
		set_xlabel(plot, "Time (seconds)");

	if (plot->legend_index <= 8)
		svg_write_legend(plot);
	else
		svg_free_legend(plot);
	close_plot(plot);
}

static void plot_latency(struct plot *plot, int seconds)
{
	struct trace_file *tf;
	char *units;
	char line[128];
	u64 max = 0;

	if (active_graphs[LATENCY_GRAPH_INDEX] == 0)
		return;

	if (num_traces > 1)
		svg_alloc_legend(plot, num_traces);
	list_for_each_entry(tf, &all_traces, list) {
		if (tf->latency_gld->max > max)
			max = tf->latency_gld->max;
	}
	list_for_each_entry(tf, &all_traces, list)
		tf->latency_gld->max = max;

	plot->add_xlabel = last_active_graph == TPUT_GRAPH_INDEX;
	setup_axis(plot);
	set_plot_label(plot, "IO Latency");

	tf = list_entry(all_traces.next, struct trace_file, list);

	scale_line_graph_time(&max, &units);
	sprintf(line, "latency (%ss)", units);
	set_ylabel(plot, line);
	set_yticks(plot, 4, 0, max, "");
	set_xticks(plot, 9, 0, seconds);

	list_for_each_entry(tf, &all_traces, list) {
		svg_line_graph(plot, tf->latency_gld, tf->read_color);
		if (num_traces > 1)
			svg_add_legend(plot, tf->label, "", tf->read_color);
	}

	if (plot->add_xlabel)
		set_xlabel(plot, "Time (seconds)");
	if (num_traces > 1)
		svg_write_legend(plot);
	close_plot(plot);
}

static void plot_queue_depth(struct plot *plot, int seconds)
{
	struct trace_file *tf;

	if (active_graphs[QUEUE_DEPTH_GRAPH_INDEX] == 0)
		return;

	plot->add_xlabel = last_active_graph == QUEUE_DEPTH_GRAPH_INDEX;

	setup_axis(plot);
	set_plot_label(plot, "Queue Depth");
	if (num_traces > 1)
		svg_alloc_legend(plot, num_traces);

	tf = list_entry(all_traces.next, struct trace_file, list);
	set_ylabel(plot, "Pending IO");
	set_yticks(plot, 4, 0, tf->queue_depth_gld->max, "");
	set_xticks(plot, 9, 0, seconds);

	list_for_each_entry(tf, &all_traces, list) {
		svg_line_graph(plot, tf->queue_depth_gld, tf->read_color);
		if (num_traces > 1)
			svg_add_legend(plot, tf->label, "", tf->read_color);
	}

	if (plot->add_xlabel)
		set_xlabel(plot, "Time (seconds)");
	if (num_traces > 1)
		svg_write_legend(plot);
	close_plot(plot);
}

static void plot_iops(struct plot *plot, int seconds)
{
	struct trace_file *tf;
	char *units;
	u64 max = 0;

	if (active_graphs[IOPS_GRAPH_INDEX] == 0)
		return;

	list_for_each_entry(tf, &all_traces, list) {
		if (tf->iop_gld->max > max)
			max = tf->iop_gld->max;
	}

	list_for_each_entry(tf, &all_traces, list)
		tf->iop_gld->max = max;


	plot->add_xlabel = last_active_graph == IOPS_GRAPH_INDEX;
	setup_axis(plot);
	set_plot_label(plot, "IOPs");
	if (num_traces > 1)
		svg_alloc_legend(plot, num_traces);

	tf = list_entry(all_traces.next, struct trace_file, list);

	scale_line_graph_bytes(&max, &units, 1000);
	set_ylabel(plot, "IO/s");

	set_yticks(plot, 4, 0, max, units);
	set_xticks(plot, 9, 0, seconds);

	list_for_each_entry(tf, &all_traces, list) {
		svg_line_graph(plot, tf->iop_gld, tf->read_color);
		if (num_traces > 1)
			svg_add_legend(plot, tf->label, "", tf->read_color);
	}

	if (plot->add_xlabel)
		set_xlabel(plot, "Time (seconds)");
	if (num_traces > 1)
		svg_write_legend(plot);

	close_plot(plot);
}

int main(int ac, char **av)
{
	struct plot *plot;
	int seconds = 0;
	u64 max_offset = 0;
	int fd;
	struct trace_file *tf;
	int ret;

	init_io_hash_table();

	enable_all_graphs();

	parse_options(ac, av);

	last_active_graph = last_graph();

	if (list_empty(&all_traces)) {
		fprintf(stderr, "No traces found, exiting\n");
		exit(1);
	}

	if (blktrace_device) {
		ret = start_blktrace(blktrace_device, blktrace_outfile,
				     blktrace_dest_dir);
		if (ret) {
			fprintf(stderr, "exiting due to blktrace failure\n");
			exit(1);
		}
		start_mpstat(blktrace_outfile);
		if (program_to_run) {
			ret = run_program(program_to_run);
			if (ret) {
				fprintf(stderr, "failed to run %s\n",
					program_to_run);
				exit(1);
			}
			wait_for_tracers();
			blktrace_to_dump(blktrace_outfile);
		} else {
			/* no program specified, just wait for
			 * blktrace to exit
			 */
			wait_for_tracers();
		}
	}

	/* step one, read all the traces */
	read_traces();

	/* step two, find the maxes for time and offset */
	list_for_each_entry(tf, &all_traces, list)
		compare_max_tf(tf, &seconds, &max_offset);

	/* push the max we found into all the tfs */
	set_all_max_tf(seconds, max_offset);

	/* alloc graphing structs for all the traces */
	setup_trace_file_graphs();

	/* run through all the traces and read their events */
	read_trace_events();

	fd = open(output_filename, O_CREAT | O_TRUNC | O_WRONLY, 0600);
	if (fd < 0) {
		fprintf(stderr, "Unable to open output file %s %s\n",
			output_filename, strerror(errno));
		exit(1);
	}

	write_svg_header(fd);
	plot = alloc_plot(fd);

	if (active_graphs[IO_GRAPH_INDEX] || found_mpstat)
		set_legend_width(longest_label + strlen("writes"));
	else if (num_traces > 1)
		set_legend_width(longest_label);
	else
		set_legend_width(0);

	set_plot_title(plot, graph_title);

	plot_io(plot, seconds, max_offset);
	plot_tput(plot, seconds);
	plot_cpu(plot, seconds, "CPU IO Wait Time",
		 CPU_IO_GRAPH_INDEX, MPSTAT_IO);
	plot_cpu(plot, seconds, "CPU System Time",
		 CPU_SYS_GRAPH_INDEX, MPSTAT_SYS);
	plot_cpu(plot, seconds, "CPU IRQ Time",
		 CPU_IRQ_GRAPH_INDEX, MPSTAT_IRQ);
	plot_cpu(plot, seconds, "CPU SoftIRQ Time",
		 CPU_SOFT_GRAPH_INDEX, MPSTAT_SOFT);
	plot_cpu(plot, seconds, "CPU User Time",
		 CPU_USER_GRAPH_INDEX, MPSTAT_USER);

	plot_latency(plot, seconds);
	plot_queue_depth(plot, seconds);
	plot_iops(plot, seconds);

	/* once for all */
	close_plot(plot);
	close(fd);
	return 0;
}
