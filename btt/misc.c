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
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/resource.h>

#define INLINE_DECLARE
#include "globals.h"

int in_devices(struct blk_io_trace *t)
{
	int i;
	unsigned int mjr, mnr;
	char *p = devices;

	if (p == NULL) return 1;	/* Allow anything */

	for (;;) {
		i = sscanf(p, "%u,%u;", &mjr, &mnr);
		if ((mjr == MAJOR(t->device) && (mnr == MINOR(t->device))))
			return 1;

		p = strchr(p, ';');
		if (!p)
			break;
		p++;
	}

	return 0;
}

void add_file(struct file_info **fipp, FILE *fp, char *oname)
{
	struct file_info *fip = malloc(sizeof(*fip));

	fip->ofp = fp;
	fip->oname = oname;
	fip->next = *fipp;
	*fipp = fip;
}

void clean_files(struct file_info **fipp)
{
	struct stat buf;
	struct file_info *fip;

	while ((fip = *fipp) != NULL) {
		*fipp = fip->next;

		fclose(fip->ofp);
		if (!stat(fip->oname, &buf) && (buf.st_size == 0))
			unlink(fip->oname);

		free(fip->oname);
		free(fip);
	}
}

struct buf_info {
	struct buf_info *next;
	void *buf;
} *all_bufs;
void add_buf(void *buf)
{
	struct buf_info *bip = malloc(sizeof(*bip));

	bip->buf = buf;
	bip->next = all_bufs;
	all_bufs = bip;
}

void clean_bufs(void)
{
	struct buf_info *bip;

	while ((bip = all_bufs) != NULL) {
		all_bufs = bip->next;
		free(bip->buf);
		free(bip);
	}
}

char *make_dev_hdr(char *pad, size_t len, struct d_info *dip, int add_parens)
{
	if (dip->map == NULL) {
		if (add_parens)
			snprintf(pad, len, "(%3d,%3d)",
				 MAJOR(dip->device), MINOR(dip->device));
		else
			snprintf(pad, len, "%d,%d",
				 MAJOR(dip->device), MINOR(dip->device));
	}
	else
		snprintf(pad, len, "%s", dip->map->device);

	return pad;
}

/*
 * Due to the N(devs) parts of a lot of the output features provided
 * by btt, it will fail opens on large(ish) systems. Here we try to
 * keep bumping our open file limits, and if those fail, we return NULL.
 *
 * Root users will probably be OK with this, others...
 */
FILE *my_fopen(const char *path, const char *mode)
{
	FILE *fp;

	fp = fopen(path, mode);
	while (fp == NULL) {
		if (errno == ENFILE || errno == EMFILE) {
			struct rlimit rlim;

			if (getrlimit(RLIMIT_NOFILE, &rlim) < 0) {
				perror("get: RLIMIT_NOFILE");
				return NULL;
			}

			rlim.rlim_cur++;
			if (rlim.rlim_cur >= rlim.rlim_max)
				rlim.rlim_max++;

			if (setrlimit(RLIMIT_NOFILE, &rlim) < 0) {
				perror("set: RLIMIT_NOFILE");
				return NULL;
			}
		}
		else {
			perror(path);
			return NULL;
		}

		fp = fopen(path, mode);
	}

	return fp;
}

void dbg_ping(void) {}
