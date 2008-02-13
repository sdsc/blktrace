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
#include "globals.h"

struct bno_dump {
	FILE *rfp, *wfp, *cfp;
};

static struct file_info *bno_dump_files = NULL;

static FILE *bno_dump_open(__u32 device, char rwc)
{
	FILE *fp;
	char *oname;
	int mjr, mnr;

	mjr = device >> MINORBITS;
	mnr = device & ((1 << MINORBITS) - 1);

	oname = malloc(strlen(bno_dump_name) + 32);
	sprintf(oname, "%s_%03d,%03d_%c.dat", bno_dump_name, mjr, mnr, rwc);
	if ((fp = fopen(oname, "w")) == NULL)
		perror(oname);
	else
		add_file(&bno_dump_files, fp, oname);
	return fp;
}

void *bno_dump_init(__u32 device)
{
	struct bno_dump *bdp;

	if (bno_dump_name == NULL) return NULL;

	bdp = malloc(sizeof(*bdp));
	bdp->rfp = bno_dump_open(device, 'r');
	bdp->wfp = bno_dump_open(device, 'w');
	bdp->cfp = bno_dump_open(device, 'c');

	return bdp;
}

void bno_dump_exit(void *param)
{
	/*
	 * Associated files will be auto-cleaned by bno_dump_clean
	 */
	free(param);
}

static inline void bno_dump_write(FILE *fp, struct io *iop)
{
	fprintf(fp, "%15.9lf %lld %lld\n",
	        BIT_TIME(iop->t.time),
		(long long)BIT_START(iop), (long long)BIT_END(iop));
}

void bno_dump_add(void *handle, struct io *iop)
{
#	define RW_FP(bdp, iop)	(IOP_READ(iop) ? bdp->rfp : bdp->wfp)
	struct bno_dump *bdp = handle;

	if (bdp) {
		if (RW_FP(bdp, iop))
			bno_dump_write(RW_FP(bdp, iop), iop);
		if (bdp->cfp)
			bno_dump_write(bdp->cfp, iop);
	}
}

void bno_dump_clean(void)
{
	clean_files(&bno_dump_files);
}
