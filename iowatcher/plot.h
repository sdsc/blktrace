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
 */
#ifndef __IOWATCH_PLOT__
#define __IOWATCH_PLOT__
#define MAX_TICKS 10

#include "list.h"

typedef __u64 u64;
typedef __u32 u32;
typedef __u16 u16;


struct plot {
	int fd;

	/* svg style y = 0 is the top of the graph */
	int start_y_offset;

	/* abs coords of the start of X start of the plot */
	int start_x_offset;

	int add_xlabel;
	int no_legend;

	/*
	 * these two are for anyone that wants
	 * to add a plot after this one, it tells
	 * them how much space we took up
	 */
	int total_height;
	int total_width;
	char **legend_lines;
	int legend_index;
	int num_legend_lines;
};

struct graph_line_pair {
	u64 count;
	u64 sum;
};

struct graph_line_data {
	/* total number of seconds in this graph */
	int seconds;

	int stop_seconds;

	/* Y max */
	u64 max;

	/* label for this graph */
	char *label;
	struct graph_line_pair data[];
};

struct graph_dot_data {
	u64 max_offset;
	u64 max_bank;
	u64 max_bank_offset;
	u64 total_ios;
	u64 total_bank_ios;

	int add_bank_ios;

	/* in pixels, number of rows in our bitmap */
	int rows;
	/* in pixels, number of cols in our bitmap */
	int cols;

	/* total number of seconds in this graph */
	int seconds;
	int stop_seconds;

	/* label for the legend */
	char *label;

	/* bitmap, one bit for each cell to light up */
	unsigned char data[];
};

struct plot_history {
	struct list_head list;
	double history_max;
	int history_len;
	int num_used;
	int col;
	char *color;
	double *history;
};

int svg_io_graph(struct plot *plot, struct graph_dot_data *gdd, char *color);
int svg_line_graph(struct plot *plot, struct graph_line_data *gld, char *color, int thresh1, int thresh2);
struct graph_line_data *alloc_line_data(int seconds, int stop_seconds);
void free_line_data(struct graph_line_data *gld);
struct graph_dot_data *alloc_dot_data(int seconds, u64 max_offset, int stop_seconds);
void free_dot_data(struct graph_dot_data *gdd);
void set_gdd_bit(struct graph_dot_data *gdd, u64 offset, double bytes, double time);
void print_gdd(struct graph_dot_data *gdd);
void write_svg_header(int fd);
struct plot *alloc_plot(void);
int close_plot(struct plot *plot);
int close_plot_no_height(struct plot *plot);
void setup_axis(struct plot *plot);
void set_xticks(struct plot *plot, int num_ticks, int first, int last);
void set_yticks(struct plot *plot, int num_ticks, int first, int last, char *units);
void set_plot_title(struct plot *plot, char *title);
void set_plot_label(struct plot *plot, char *label);
void set_xlabel(struct plot *plot, char *label);
void set_ylabel(struct plot *plot, char *label);
void scale_line_graph_bytes(u64 *max, char **units, u64 factor);
void scale_line_graph_time(u64 *max, char **units);
void write_drop_shadow(struct plot *plot);
void write_drop_shadow_line(struct plot *plot);
void svg_write_legend(struct plot *plot);
void svg_add_legend(struct plot *plot, char *text, char *extra, char *color);
void svg_alloc_legend(struct plot *plot, int num_lines);
void set_legend_width(int longest_str);
void set_rolling_avg(int rolling);
void svg_free_legend(struct plot *plot);
void set_io_graph_scale(int scale);
void set_plot_output(struct plot *plot, char *filename);
void set_graph_size(int width, int height);
void get_graph_size(int *width, int *height);
int svg_io_graph_movie(struct graph_dot_data *gdd, struct plot_history *ph, int col);
int svg_io_graph_movie_array(struct plot *plot, struct plot_history *ph);
void svg_write_time_line(struct plot *plot, int col);
void set_graph_height(int h);
void set_graph_width(int w);
int close_plot_file(struct plot *plot);
int svg_io_graph_movie_array_spindle(struct plot *plot, struct plot_history *ph);
void rewind_spindle_steps(int num);
void setup_axis_spindle(struct plot *plot);
int close_plot_col(struct plot *plot);
#endif
