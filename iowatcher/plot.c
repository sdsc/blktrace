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

#include "plot.h"

static int io_graph_scale = 8;
static int graph_width = 700;
static int graph_height = 250;
static int graph_circle_extra = 30;
static int graph_inner_x_margin = 2;
static int graph_inner_y_margin = 2;
static int graph_tick_len = 5;
static int graph_left_pad = 120;
static int tick_label_pad = 16;
static int tick_font_size = 15;
static char *font_family = "sans-serif";

/* this is the title for the whole page */
static int plot_title_height = 50;
static int plot_title_font_size = 25;

/* this is the label at the top of each plot */
static int plot_label_height = 60;
static int plot_label_font_size = 20;

/* label for each axis is slightly smaller */
static int axis_label_font_size = 16;

int legend_x_off = 45;
int legend_y_off = -10;
int legend_font_size = 15;
int legend_width = 80;

static int rolling_avg_secs = 0;

static int line_len = 1024;
static char line[1024];

static int final_height = 0;
static int final_width = 0;

struct graph_line_data *alloc_line_data(int seconds, int stop_seconds)
{
	int size = sizeof(struct graph_line_data) + (stop_seconds + 1) * sizeof(struct graph_line_pair);
	struct graph_line_data *gld;

	gld = calloc(1, size);
	if (!gld) {
		fprintf(stderr, "Unable to allocate memory for graph data\n");
		exit(1);
	}
	gld->seconds = seconds;
	gld->stop_seconds = stop_seconds;
	return gld;
}

void free_line_data(struct graph_line_data *gld)
{
	free(gld->label);
	free(gld);
}

struct graph_dot_data *alloc_dot_data(int seconds, u64 max_offset, int stop_seconds)
{
	int size;
	int arr_size;
	int rows = graph_height * io_graph_scale;
	int cols = graph_width;
	struct graph_dot_data *gdd;

	size = sizeof(struct graph_dot_data);

	/* the number of bits */
	arr_size = (rows + 1) * cols;

	/* the number of bytes */
	arr_size /= 8;

	gdd = calloc(1, size + arr_size);
	if (!gdd) {
		fprintf(stderr, "Unable to allocate memory for graph data\n");
		exit(1);
	}
	gdd->seconds = seconds;
	gdd->stop_seconds = stop_seconds;
	gdd->rows = rows;
	gdd->cols = cols;
	gdd->max_offset = max_offset;
	return gdd;
}

void free_dot_data(struct graph_dot_data *gdd)
{
	free(gdd);
}

void set_gdd_bit(struct graph_dot_data *gdd, u64 offset, double bytes, double time)
{
	double bytes_per_row = (double)gdd->max_offset / gdd->rows;

	double secs_per_col = (double)gdd->seconds / gdd->cols;
	double col;
	double row;
	int col_int;
	int row_int;
	int bit_index;
	int arr_index;
	int bit_mod;
	double mod = bytes_per_row;

	if (offset > gdd->max_offset)
		return;

	gdd->total_ios++;
	time = time / 1000000000.0;
	while (bytes > 0) {
		row = (double)offset / bytes_per_row;
		col = time / secs_per_col;

		col_int = floor(col);
		row_int = floor(row);
		bit_index = row_int * gdd->cols + col_int;
		arr_index = bit_index / 8;
		bit_mod = bit_index % 8;

		gdd->data[arr_index] |= 1 << bit_mod;
		offset += mod;
		bytes -= mod;
	}
}

void print_gdd(struct graph_dot_data *gdd)
{
	int col = 0;
	int row = 0;
	int arr_index;
	u64 val;
	int bit_index;
	int bit_mod;

	for (row = gdd->rows - 1; row >= 0; row--) {
		for (col = 0; col < gdd->cols; col++) {
			bit_index = row * gdd->cols + col;
			arr_index = bit_index / sizeof(unsigned long);
			bit_mod = bit_index % sizeof(unsigned long);

			val = gdd->data[arr_index];
			if (val & (1 << bit_mod))
				printf("*");
			else
				printf(" ");
		}
		printf("\n");
	}
}

static double rolling_avg(struct graph_line_pair *data, int index, int distance)
{
	double sum = 0;
	int start;

	if (distance < 0)
		distance = 1;
	if (distance > index) {
		start = 0;
	} else {
		start = index - distance;
	}
	distance = 0;
	while (start <= index) {
		double avg;

		if (data[start].count)
			avg = ((double)data[start].sum) / data[start].count;
		else
			avg= 0;

		sum += avg;
		distance++;
		start++;
	}
	return sum / distance;
}

void write_svg_header(int fd)
{
	char *spaces = "                                                    \n";
	char *header = "<svg  xmlns=\"http://www.w3.org/2000/svg\">\n";
	char *filter1 ="<filter id=\"shadow\">\n "
		"<feOffset result=\"offOut\" in=\"SourceAlpha\" dx=\"4\" dy=\"4\" />\n "
		"<feGaussianBlur result=\"blurOut\" in=\"offOut\" stdDeviation=\"2\" />\n "
		"<feBlend in=\"SourceGraphic\" in2=\"blurOut\" mode=\"normal\" />\n "
		"</filter>\n";
	char *filter2 ="<filter id=\"textshadow\" x=\"0\" y=\"0\" width=\"200%\" height=\"200%\">\n "
		"<feOffset result=\"offOut\" in=\"SourceAlpha\" dx=\"1\" dy=\"1\" />\n "
		"<feGaussianBlur result=\"blurOut\" in=\"offOut\" stdDeviation=\"1.5\" />\n "
		"<feBlend in=\"SourceGraphic\" in2=\"blurOut\" mode=\"normal\" />\n "
		"</filter>\n";
	char *filter3 ="<filter id=\"labelshadow\" x=\"0\" y=\"0\" width=\"200%\" height=\"200%\">\n "
		"<feOffset result=\"offOut\" in=\"SourceGraphic\" dx=\"3\" dy=\"3\" />\n "
		"<feColorMatrix result=\"matrixOut\" in=\"offOut\" type=\"matrix\" "
		"values=\"0.2 0 0 0 0 0 0.2 0 0 0 0 0 0.2 0 0 0 0 0 1 0\" /> "
		"<feGaussianBlur result=\"blurOut\" in=\"offOut\" stdDeviation=\"2\" />\n "
		"<feBlend in=\"SourceGraphic\" in2=\"blurOut\" mode=\"normal\" />\n "
		"</filter>\n";
	char *defs_start = "<defs>\n";
	char *defs_close = "</defs>\n";
	final_width = 0;
	final_height = 0;

	write(fd, header, strlen(header));
	/* write a bunch of spaces so we can stuff in the width and height later */
	write(fd, spaces, strlen(spaces));

	write(fd, defs_start, strlen(defs_start));
	write(fd, filter1, strlen(filter1));
	write(fd, filter2, strlen(filter2));
	write(fd, filter3, strlen(filter3));
	write(fd, defs_close, strlen(defs_close));
}

void write_drop_shadow(struct plot *plot)
{
	snprintf(line, line_len, "<rect x=\"0\" y=\"%d\" width=\"%d\" height=\"%d\" fill=\"white\"/>\n",
		 plot->start_y_offset, plot->total_width, 45);
	write(plot->fd, line, strlen(line));

	snprintf(line, line_len, "<path d=\"M %d %d h %d v %d h %d t %d %d V %d H %d Z\" "
		 "fill=\"white\" filter=\"url(#shadow)\"/>",
		0, plot->start_y_offset,
		plot->total_width - graph_left_pad / 2,
		-plot->total_height, 24, 1, 1,
		plot->start_y_offset + 10, 0);
	write(plot->fd, line, strlen(line));

	snprintf(line, line_len, "<path d=\"M %d %d H %d V %d h %d V %d H %d Z\" "
		 "fill=\"white\"/>",
		0, plot->start_y_offset - 15, /* start */
		plot->total_width - graph_left_pad / 2 - 10, /* hline over */
		plot->start_y_offset - plot->total_height, /* vline up */
		15, /*hline over */
		plot->start_y_offset, /* vline back down */
		0);
	write(plot->fd, line, strlen(line));

	plot->start_y_offset += 45;
}

/* svg y offset for the traditional 0,0 (bottom left corner) of the plot */
static int axis_y(void)
{
	return plot_label_height + graph_height + graph_inner_y_margin;
}

/* this gives you the correct pixel for a given offset from the bottom left y axis */
static double axis_y_off_double(double y)
{
	return plot_label_height + graph_height - y;
}

static int axis_y_off(int y)
{
	return axis_y_off_double(y);
}

/* svg x axis offset from 0 */
static int axis_x(void)
{
	return graph_left_pad;
}

/* the correct pixel for a given X offset */
static double axis_x_off_double(double x)
{
	return graph_left_pad + graph_inner_x_margin + x;
}

static int axis_x_off(int x)
{
	return (int)axis_x_off_double(x);
}


/*
 * this draws a backing rectangle for the plot and it
 * also creates a new svg element so our offsets can
 * be relative to this one plot.
 */
void setup_axis(struct plot *plot)
{
	int ret;
	int len;
	int fd = plot->fd;
	int bump_height = tick_font_size * 3 + axis_label_font_size;

	plot->total_width = axis_x_off(graph_width) + graph_left_pad / 2 + legend_width;
	plot->total_height = axis_y() + tick_label_pad + tick_font_size;

	if (plot->add_xlabel)
		plot->total_height += bump_height;

	/* backing rect */
	snprintf(line, line_len, "<rect x=\"0\" y=\"%d\" width=\"%d\" "
		 "height=\"%d\" fill=\"white\" stroke=\"none\"/>",
		plot->start_y_offset, plot->total_width + 40,
		plot->total_height + 20);
	len = strlen(line);
	write(fd, line, len);

	snprintf(line, line_len, "<rect x=\"15\" y=\"%d\" width=\"%d\" "
		 "filter=\"url(#shadow)\" "
		 "height=\"%d\" fill=\"white\" stroke=\"none\"/>",
		plot->start_y_offset, plot->total_width, plot->total_height);
	len = strlen(line);
	write(fd, line, len);
	plot->total_height += 20;

	if (plot->total_height + plot->start_y_offset > final_height)
		final_height = plot->total_height + plot->start_y_offset;
	if (plot->total_width + 40 > final_width)
		final_width = plot->total_width + 40;

	/* create an svg object for all our coords to be relative against */
	snprintf(line, line_len, "<svg x=\"%d\" y=\"%d\">\n", plot->start_x_offset, plot->start_y_offset);
	write(fd, line, strlen(line));

	snprintf(line, 1024, "<path d=\"M%d %d h %d V %d H %d Z\" stroke=\"black\" stroke-width=\"2\" fill=\"none\"/>\n",
		 axis_x(), axis_y(),
		 graph_width + graph_inner_x_margin * 2, axis_y_off(graph_height) - graph_inner_y_margin,
		 axis_x());
	len = strlen(line);
	ret = write(fd, line, len);
	if (ret != len) {
		fprintf(stderr, "failed to write svg axis\n");
		exit(1);
	}
}

/*
 * this draws a backing rectangle for the plot and it
 * also creates a new svg element so our offsets can
 * be relative to this one plot.
 */
void setup_axis_spindle(struct plot *plot)
{
	int len;
	int fd = plot->fd;
	int bump_height = tick_font_size * 3 + axis_label_font_size;

	plot->total_width = axis_x_off(graph_width) + graph_left_pad / 2 + legend_width;
	plot->total_height = axis_y() + tick_label_pad + tick_font_size;

	if (plot->add_xlabel)
		plot->total_height += bump_height;

	/* backing rect */
	snprintf(line, line_len, "<rect x=\"0\" y=\"%d\" width=\"%d\" "
		 "height=\"%d\" fill=\"white\" stroke=\"none\"/>",
		plot->start_y_offset, plot->total_width + 40,
		plot->total_height + 20);
	len = strlen(line);
	write(fd, line, len);

	snprintf(line, line_len, "<rect x=\"15\" y=\"%d\" width=\"%d\" "
		 "filter=\"url(#shadow)\" "
		 "height=\"%d\" fill=\"white\" stroke=\"none\"/>",
		plot->start_y_offset, plot->total_width, plot->total_height);
	len = strlen(line);
	write(fd, line, len);
	plot->total_height += 20;

	if (plot->total_height + plot->start_y_offset > final_height)
		final_height = plot->total_height + plot->start_y_offset;
	if (plot->total_width + 40 > final_width)
		final_width = plot->total_width + 40;

	/* create an svg object for all our coords to be relative against */
	snprintf(line, line_len, "<svg x=\"%d\" y=\"%d\">\n", plot->start_x_offset, plot->start_y_offset);
	write(fd, line, strlen(line));

}

/* draw a plot title.  This should be done only once,
 * and it bumps the plot width/height numbers by
 * what it draws.
 *
 * Call this before setting up the first axis
 */
void set_plot_title(struct plot *plot, char *title)
{
	int len;
	int fd = plot->fd;

	plot->total_height = plot_title_height;
	plot->total_width = axis_x_off(graph_width) + graph_left_pad / 2 + legend_width;

	/* backing rect */
	snprintf(line, line_len, "<rect x=\"0\" y=\"%d\" width=\"%d\" height=\"%d\" fill=\"white\" stroke=\"none\"/>",
		plot->start_y_offset, plot->total_width + 40, plot_title_height + 20);
	len = strlen(line);
	write(fd, line, len);

	snprintf(line, line_len, "<text x=\"%d\" y=\"%d\" font-family=\"%s\" font-size=\"%d\" "
		 "font-weight=\"bold\" fill=\"black\" style=\"text-anchor: %s\">%s</text>\n",
		 axis_x_off(graph_width / 2),
		plot->start_y_offset + plot_title_height / 2,
		font_family, plot_title_font_size, "middle", title);
	plot->start_y_offset += plot_title_height;
	len = strlen(line);
	write(fd, line, len);
}

/*
 * create evenly spread out ticks along the xaxis.  if tick only is set
 * this just makes the ticks, otherwise it labels each tick as it goes
 */
void set_xticks(struct plot *plot, int num_ticks, int first, int last)
{
	int pixels_per_tick = graph_width / num_ticks;
	int step = (last - first) / num_ticks;
	int i;
	int tick_y = axis_y_off(graph_tick_len) + graph_inner_y_margin;
	int tick_x = axis_x();
	int tick_only = plot->add_xlabel == 0;

	int text_y = axis_y() + tick_label_pad;

	char *middle = "middle";
	char *start = "start";

	for (i = 0; i < num_ticks; i++) {
		char *anchor;
		if (i != 0) {
			snprintf(line, line_len, "<rect x=\"%d\" y=\"%d\" width=\"2\" height=\"%d\" style=\"stroke:none;fill:black;\"/>\n",
				tick_x, tick_y, graph_tick_len);
			write(plot->fd, line, strlen(line));
			anchor = middle;
		} else {
			anchor = start;
		}

		if (!tick_only) {
			snprintf(line, line_len, "<text x=\"%d\" y=\"%d\" font-family=\"%s\" font-size=\"%d\" "
				"fill=\"black\" style=\"text-anchor: %s\">%d</text>\n",
				tick_x, text_y, font_family, tick_font_size, anchor, step * i);
			write(plot->fd, line, strlen(line));
		}
		tick_x += pixels_per_tick;
	}

	if (!tick_only) {
		snprintf(line, line_len, "<text x=\"%d\" y=\"%d\" font-family=\"%s\" font-size=\"%d\" "
			"fill=\"black\" style=\"text-anchor: middle\">%d</text>\n",
			axis_x_off(graph_width - 2),
			text_y, font_family, tick_font_size, last);
		write(plot->fd, line, strlen(line));
	}
}

void set_ylabel(struct plot *plot, char *label)
{
	int len;
	int fd = plot->fd;

	snprintf(line, line_len, "<text x=\"%d\" y=\"%d\" font-family=\"%s\" "
		 "transform=\"rotate(-90 %d %d)\" font-weight=\"bold\" "
		 "font-size=\"%d\" fill=\"black\" style=\"text-anchor: %s\">%s</text>\n",
		 graph_left_pad / 2 - axis_label_font_size,
		 axis_y_off(graph_height / 2),
		 font_family,
		 graph_left_pad / 2 - axis_label_font_size,
		 (int)axis_y_off(graph_height / 2),
		 axis_label_font_size, "middle", label);
	len = strlen(line);
	write(fd, line, len);
}

void set_xlabel(struct plot *plot, char *label)
{
	int len;
	int fd = plot->fd;
	snprintf(line, line_len, "<text x=\"%d\" y=\"%d\" font-family=\"%s\" "
		 "font-weight=\"bold\" "
		 "font-size=\"%d\" fill=\"black\" style=\"text-anchor: %s\">%s</text>\n",
		 axis_x_off(graph_width / 2),
		 axis_y() + tick_font_size * 3 + axis_label_font_size / 2,
		 font_family,
		 axis_label_font_size, "middle", label);
	len = strlen(line);
	write(fd, line, len);

}

/*
 * create evenly spread out ticks along the y axis.
 * The ticks are labeled as it goes
 */
void set_yticks(struct plot *plot, int num_ticks, int first, int last, char *units)
{
	int pixels_per_tick = graph_height / num_ticks;
	int step = (last - first) / num_ticks;
	int i;
	int tick_y = 0;
	int text_x = axis_x() - 6;
	int tick_x = axis_x();
	char *anchor = "end";

	for (i = 0; i < num_ticks; i++) {
		if (i != 0) {
			snprintf(line, line_len, "<line x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\" "
				 "style=\"stroke:lightgray;stroke-width:2;stroke-dasharray:9,12;\"/>\n",
				tick_x, axis_y_off(tick_y),
				axis_x_off(graph_width), axis_y_off(tick_y));
			write(plot->fd, line, strlen(line));
		}

		snprintf(line, line_len, "<text x=\"%d\" y=\"%d\" font-family=\"%s\" font-size=\"%d\" "
			 "fill=\"black\" style=\"text-anchor: %s\">%d%s</text>\n",
			text_x,
			axis_y_off(tick_y - tick_font_size / 2),
			font_family, tick_font_size, anchor, step * i, units);
		write(plot->fd, line, strlen(line));
		tick_y += pixels_per_tick;
	}
	snprintf(line, line_len, "<text x=\"%d\" y=\"%d\" font-family=\"%s\" font-size=\"%d\" "
		 "fill=\"black\" style=\"text-anchor: %s\">%d%s</text>\n",
		 text_x, axis_y_off(graph_height), font_family, tick_font_size, anchor, last, units);
	write(plot->fd, line, strlen(line));
}

void set_plot_label(struct plot *plot, char *label)
{
	int len;
	int fd = plot->fd;

	snprintf(line, line_len, "<text x=\"%d\" y=\"%d\" font-family=\"%s\" "
		 "font-size=\"%d\" fill=\"black\" style=\"text-anchor: %s\">%s</text>\n",
		 axis_x() + graph_width / 2,
		 plot_label_height / 2,
		font_family, plot_label_font_size, "middle", label);
	len = strlen(line);
	write(fd, line, len);
}

static void close_svg(int fd)
{
	char *close_line = "</svg>\n";

	write(fd, close_line, strlen(close_line));
}

int close_plot(struct plot *plot)
{
	close_svg(plot->fd);
	plot->start_y_offset += plot->total_height;
	plot->add_xlabel = 0;
	return 0;
}

struct plot *alloc_plot(void)
{
	struct plot *plot;
	plot = calloc(1, sizeof(*plot));
	if (!plot) {
		fprintf(stderr, "Unable to allocate memory %s\n", strerror(errno));
		exit(1);
	}
	plot->fd = 0;
	return plot;
}

int close_plot_file(struct plot *plot)
{
	int ret;
	ret = lseek(plot->fd, 0, SEEK_SET);
	if (ret == (off_t)-1) {
		perror("seek");
		exit(1);
	}
	final_width = ((final_width  + 1) / 2) * 2;
	final_height = ((final_height  + 1) / 2) * 2;
	snprintf(line, line_len, "<svg  xmlns=\"http://www.w3.org/2000/svg\" "
		 "width=\"%d\" height=\"%d\">\n",
		 final_width, final_height);
	write(plot->fd, line, strlen(line));
	close(plot->fd);
	plot->fd = 0;
	return 0;
}

void set_plot_output(struct plot *plot, char *filename)
{
	int fd;

	if (plot->fd)
		close_plot_file(plot);
	fd = open(filename, O_CREAT | O_TRUNC | O_WRONLY, 0600);
	if (fd < 0) {
		fprintf(stderr, "Unable to open output file %s err %s\n", filename, strerror(errno));
		exit(1);
	}
	plot->fd = fd;
	plot->start_y_offset = plot->start_x_offset = 0;
	write_svg_header(fd);
}

char *byte_unit_names[] = { "", "K", "M", "G", "T", "P", "E", "Z", "Y", "unobtainium" };
int MAX_BYTE_UNIT_SCALE = 9;

char *time_unit_names[] = { "n", "u", "m", "s" };
int MAX_TIME_UNIT_SCALE = 3;

void scale_line_graph_bytes(u64 *max, char **units, u64 factor)
{
	int scale = 0;
	u64 val = *max;
	u64 div = 1;
	while (val > factor * 64) {
		val /= factor;
		scale++;
		div *= factor;
	}
	*units = byte_unit_names[scale];
	if (scale == 0)
		return;

	if (scale > MAX_BYTE_UNIT_SCALE)
		scale = MAX_BYTE_UNIT_SCALE;

	*max /= div;
}

void scale_line_graph_time(u64 *max, char **units)
{
	int scale = 0;
	u64 val = *max;
	u64 div = 1;
	while (val > 1000 * 10) {
		val /= 1000;
		scale++;
		div *= 1000;
		if (scale == MAX_TIME_UNIT_SCALE)
			break;
	}
	*units = time_unit_names[scale];
	if (scale == 0)
		return;

	*max /= div;
}

int svg_line_graph(struct plot *plot, struct graph_line_data *gld, char *color, int thresh1, int thresh2)
{
	int i;
	double val;
	double avg;
	int rolling;
	int fd = plot->fd;
	char *start = "<path d=\"";
	double yscale = ((double)gld->max) / graph_height;
	double xscale = (double)(gld->seconds - 1) / graph_width;
	char c = 'M';
	double x;
	int printed_header = 0;
	int printed_lines = 0;

	if (thresh1 && thresh2)
		rolling = 0;
	else if (rolling_avg_secs)
		rolling = rolling_avg_secs;
	else
		rolling = gld->stop_seconds / 25;

	for (i = 0; i < gld->stop_seconds; i++) {
		avg = rolling_avg(gld->data, i, rolling);
		if (yscale == 0)
			val = 0;
		else
			val = avg / yscale;

		if (val > graph_height)
			val = graph_height;
		if (val < 0)
			val = 0;

		x = (double)i / xscale;
		if (!thresh1 && !thresh2) {

			if (!printed_header) {
				write(fd, start, strlen(start));
				printed_header = 1;
			}

			/* in full line mode, everything in the graph is connected */
			snprintf(line, line_len, "%c %d %d ", c, axis_x_off(x), axis_y_off(val));
			c = 'L';
			write(fd, line, strlen(line));
			printed_lines = 1;
		} else if (avg > thresh1 || avg > thresh2) {
			int len = 10;
			if (!printed_header) {
				write(fd, start, strlen(start));
				printed_header = 1;
			}

			/* otherwise, we just print a bar up there to show this one data point */
			if (i >= gld->stop_seconds - 2)
				len = -10;

			/*
			 * we don't use the rolling averages here to show high
			 * points in the data
			 */
			snprintf(line, line_len, "M %d %d h %d ", axis_x_off(x),
				 axis_y_off(val), len);
			write(fd, line, strlen(line));
			printed_lines = 1;
		}

	}
	if (printed_lines) {
		snprintf(line, line_len, "\" fill=\"none\" stroke=\"%s\" stroke-width=\"2\"/>\n", color);
		write(fd, line, strlen(line));
	}

	return 0;
}

void svg_write_time_line(struct plot *plot, int col)
{
	snprintf(line, line_len, "<line x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\" "
				 "style=\"stroke:black;stroke-width:2;\"/>\n",
				 axis_x_off(col), axis_y_off(0),
				 axis_x_off(col), axis_y_off(graph_height));
	write(plot->fd, line, strlen(line));
}

static int svg_add_io(int fd, double row, double col, double width, double height, char *color)
{
	float rx = 0;

	snprintf(line, line_len, "<rect x=\"%.2f\" y=\"%.2f\" width=\"%.1f\" height=\"%.1f\" "
		 "rx=\"%.2f\" style=\"stroke:none;fill:%s;stroke-width:0\"/>\n",
		 axis_x_off_double(col), axis_y_off_double(row), width, height, rx, color);
	return write(fd, line, strlen(line));
}

int svg_io_graph_movie_array(struct plot *plot, struct plot_history *ph)
{
	double cell_index;
	double movie_row;
	double movie_col;
	int i;

	for (i = 0; i < ph->num_used; i++) {
		cell_index = ph->history[i];
		movie_row = floor(cell_index / graph_width);
		movie_col = cell_index - movie_row * graph_width;
		svg_add_io(plot->fd, movie_row, movie_col, 4, 4, ph->color);
	}
	return 0;
}

static float spindle_steps = 0;

void rewind_spindle_steps(int num)
{
	spindle_steps -= num * 0.01;
}

int svg_io_graph_movie_array_spindle(struct plot *plot, struct plot_history *ph)
{
	double cell_index;
	int i;
	int num_circles = 0;
	double cells_per_circle;
	double circle_num;
	double degrees_per_cell;
	double rot;
	double center_x;
	double center_y;
	double graph_width_extra = graph_width + graph_circle_extra;
	double graph_height_extra = graph_height + graph_circle_extra;
	double radius;;

	if (graph_width_extra > graph_height_extra)
		graph_width_extra = graph_height_extra;

	if (graph_width_extra < graph_height_extra)
		graph_height_extra = graph_width_extra;

	radius = graph_width_extra;

	center_x = axis_x_off_double(graph_width_extra / 2);
	center_y = axis_y_off_double(graph_height_extra / 2);

	snprintf(line, line_len, "<g transform=\"rotate(%.4f, %.2f, %.2f)\"> "
		 "<circle cx=\"%.2f\" cy=\"%.2f\" "
		 "stroke=\"black\" stroke-width=\"6\" "
		 "r=\"%.2f\" fill=\"none\"/>\n",
		 -spindle_steps * 1.2, center_x, center_y, center_x, center_y, graph_width_extra / 2);
	write(plot->fd, line, strlen(line));
	snprintf(line, line_len, "<circle cx=\"%.2f\" cy=\"%.2f\" "
		"stroke=\"none\" fill=\"red\" r=\"%.2f\"/>\n</g>\n",
		axis_x_off_double(graph_width_extra), center_y, 4.5);
	write(plot->fd, line, strlen(line));
	spindle_steps += 0.01;

	radius = floor(radius / 2);
	num_circles = radius / 4 - 3;
	cells_per_circle = ph->history_max / num_circles;
	degrees_per_cell = 360 / cells_per_circle;

	for (i = 0; i < ph->num_used; i++) {
		cell_index = ph->history[i];
		circle_num = floor(cell_index / cells_per_circle);
		rot = cell_index - circle_num * cells_per_circle;
		circle_num = num_circles - circle_num;
		radius = circle_num * 4;

		rot = rot * degrees_per_cell;
		rot -= spindle_steps;
		snprintf(line, line_len, "<path transform=\"rotate(%.4f, %.2f, %.2f)\" "
			 "d=\"M %.2f %.2f a %.2f %.2f 0 0 1 0 5\" "
			 "stroke=\"%s\" stroke-width=\"4\"/>\n",
			 rot, center_x, center_y,
			 axis_x_off_double(graph_width_extra / 2 + radius) + 8, center_y,
			 radius, radius, ph->color);

		write(plot->fd, line, strlen(line));
	}
	return 0;
}

static int add_plot_history(struct plot_history *ph, double val)
{
	if (ph->num_used == ph->history_len) {
		ph->history = realloc(ph->history,
				      (ph->history_len + 4096) * sizeof(double));
		if (!ph->history) {
			perror("Unable to allocate memory");
			exit(1);
		}
		ph->history_len += 4096;
	}
	ph->history[ph->num_used++] = val;
	return 0;
}

int svg_io_graph_movie(struct graph_dot_data *gdd, struct plot_history *ph, int col)
{
	int row = 0;
	int arr_index;
	unsigned char val;
	int bit_index;
	int bit_mod;
	double blocks_per_row = gdd->max_offset / gdd->rows;
	double movie_blocks_per_cell = gdd->max_offset / (graph_width * graph_height);
	double cell_index;
	int margin_orig = graph_inner_y_margin;

	graph_inner_y_margin += 5;
	ph->history_max = gdd->max_offset / movie_blocks_per_cell;

	for (row = gdd->rows - 1; row >= 0; row--) {
		bit_index = row * gdd->cols + col;
		arr_index = bit_index / 8;
		bit_mod = bit_index % 8;

		if (arr_index < 0)
			continue;
		val = gdd->data[arr_index];
		if (val & (1 << bit_mod)) {
			/* in bytes, linear offset from the start of the drive */
			cell_index = (double)row * blocks_per_row;

			/* a cell number in the graph */
			cell_index /= movie_blocks_per_cell;

			add_plot_history(ph, cell_index);
		}
	}
	graph_inner_y_margin = margin_orig;
	return 0;
}

int svg_io_graph(struct plot *plot, struct graph_dot_data *gdd, char *color)
{
	int fd = plot->fd;;
	int col = 0;
	int row = 0;
	int arr_index;
	unsigned char val;
	int bit_index;
	int bit_mod;

	for (row = gdd->rows - 1; row >= 0; row--) {
		for (col = 0; col < gdd->cols; col++) {
			bit_index = row * gdd->cols + col;
			arr_index = bit_index / 8;
			bit_mod = bit_index % 8;

			if (arr_index < 0)
				continue;
			val = gdd->data[arr_index];
			if (val & (1 << bit_mod))
				svg_add_io(fd, floor(row / io_graph_scale), col, 1.5, 1.5, color);
		}
	}
	return 0;
}

void svg_alloc_legend(struct plot *plot, int num_lines)
{
	char **lines = calloc(num_lines, sizeof(char *));
	plot->legend_index = 0;
	plot->legend_lines = lines;
	plot->num_legend_lines = num_lines;
}

void svg_free_legend(struct plot *plot)
{
	int i;
	for (i = 0; i < plot->legend_index; i++)
		free(plot->legend_lines[i]);
	free(plot->legend_lines);
	plot->legend_lines = NULL;
	plot->legend_index = 0;
}

void svg_write_legend(struct plot *plot)
{
	int legend_line_x = axis_x_off(graph_width) + legend_x_off;
	int legend_line_y = axis_y_off(graph_height) + legend_y_off;
	int i;

	if (plot->legend_index == 0)
		return;

	snprintf(line, line_len, "<rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\" "
		 "fill=\"white\" filter=\"url(#shadow)\"/>\n",
		 legend_line_x - 15,
		 legend_line_y - 12,
		 legend_width,
		 plot->legend_index * legend_font_size + legend_font_size / 2 + 12);

	write(plot->fd, line, strlen(line));
	for (i = 0; i < plot->legend_index; i++) {
		write(plot->fd, plot->legend_lines[i],
		      strlen(plot->legend_lines[i]));
		free(plot->legend_lines[i]);
	}
	free(plot->legend_lines);
	plot->legend_lines = NULL;
	plot->legend_index = 0;
}

void svg_add_legend(struct plot *plot, char *text, char *extra, char *color)
{
	int legend_line_x = axis_x_off(graph_width) + legend_x_off;
	int legend_line_y = axis_y_off(graph_height) + legend_y_off;

	if (!text && (!extra || strlen(extra) == 0))
		return;

	legend_line_y += plot->legend_index * legend_font_size + legend_font_size / 2;
	snprintf(line, line_len, "<path d=\"M %d %d h 8\" stroke=\"%s\" stroke-width=\"8\" "
		 "filter=\"url(#labelshadow)\"/> "
		 "<text x=\"%d\" y=\"%d\" font-family=\"%s\" font-size=\"%d\" "
		 "fill=\"black\" style=\"text-anchor: left\">%s%s</text>\n",
		 legend_line_x, legend_line_y,
		 color, legend_line_x + 13,
		 legend_line_y + 4, font_family, legend_font_size,
		 text, extra);

	plot->legend_lines[plot->legend_index++] = strdup(line);
}

void set_legend_width(int longest_str)
{
	if (longest_str)
		legend_width = longest_str * (legend_font_size * 3 / 4) + 25;
	else
		legend_width = 0;
}

void set_rolling_avg(int rolling)
{
	rolling_avg_secs = rolling;
}

void set_io_graph_scale(int scale)
{
	io_graph_scale = scale;
}

void set_graph_size(int width, int height)
{
	graph_width = width;
	graph_height = height;
}

void get_graph_size(int *width, int *height)
{
	*width = graph_width;
	*height = graph_height;
}

void set_graph_height(int h)
{
	graph_height = h;
}
void set_graph_width(int w)
{
	graph_width = w;
}
