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
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

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

void dbg_ping(void) {}
