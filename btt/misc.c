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

#define INLINE_DECLARE
#include "globals.h"

int data_is_native = -1;

struct blk_io_trace *convert_to_cpu(struct blk_io_trace *t)
{
	if (data_is_native == -1)
		check_data_endianness(t->magic);

	trace_to_cpu(t);

	ASSERT(CHECK_MAGIC(t));
	ASSERT((t->magic & 0xff) == SUPPORTED_VERSION);

	return t;
}

int in_devices(struct blk_io_trace *t)
{
	int i;
	unsigned int mjr, mnr;
	char *p = devices;

	if (p == NULL) return 1;	/* Allow anything */

	for (;;) {
		i = sscanf(p, "%u,%u;", &mjr, &mnr);
		ASSERT(i == 2);

		if ((mjr == MAJOR(t->device) && (mnr == MINOR(t->device))))
			return 1;

		p = strchr(p, ';');
		if (!p)
			break;
		p++;
	}

	return 0;
}

unsigned int do_read(int ifd, void *buf, int len)
{
	int n;

	n = read(ifd, buf, len);
	if (n < 0) {
		perror(input_name);
		return 1;
	}
	else if (0 < n && n < len) {
		fprintf(stderr,"Short read on %s\n", input_name);
		return 1;
	}
	else if (n == 0) /* EOF */
		return 1;

	return 0;
}
