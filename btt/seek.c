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

static struct file_info *all_files = NULL;

struct seek_bkt {
	long long sectors;
	int nseeks;
};

struct seeki {
	FILE *rfp, *wfp;
	struct seek_bkt *seek_bkts;
	int nseek_bkts;
	long long total_seeks;
	double total_sectors;
	long long last_start, last_end;
};

FILE *seek_open(__u32 device, char rw)
{
	FILE *fp;
	char *oname;
	int mjr, mnr;

	if (seek_name == NULL) return NULL;

	mjr = device >> MINORBITS;
	mnr = device & ((1 << MINORBITS) - 1);

	oname = malloc(strlen(seek_name)+32);
	sprintf(oname, "%s_%03d,%03d_%c.dat", seek_name, mjr, mnr, rw);
	if ((fp = fopen(oname, "w")) == NULL)
		perror(oname);
	else
		add_file(&all_files, fp, oname);

	return fp;
}

void seek_clean(void)
{
	clean_files(&all_files);
}

long long seek_dist(struct seeki *sip, struct io *iop)
{
	long long dist;
	long long start = BIT_START(iop), end = BIT_END(iop);

	/* Some overlap means no seek */
	if (((sip->last_start <= start) && (start <= sip->last_end)) ||
	    ((sip->last_start <= end) && (end <= sip->last_end)))
		dist = 0;
	else if (start > sip->last_end)
		dist = start - sip->last_end;
	else
		dist = start - sip->last_start;

	sip->last_start = start;
	sip->last_end = end;
	return dist;
}

void *seeki_init(__u32 device)
{
	struct seeki *sip = malloc(sizeof(struct seeki));

	sip->rfp = seek_open(device, 'r');
	sip->wfp = seek_open(device, 'w');
	sip->seek_bkts = NULL;
	sip->nseek_bkts = 0;
	sip->total_seeks = 0;
	sip->total_sectors = 0.0;
	sip->last_start = sip->last_end = 0;

	return sip;
}

void seeki_add(void *handle, struct io *iop)
{
	int left, mid, right;
	struct seek_bkt *bkt;
	struct seeki *sip = handle;
	long long dist = seek_dist(sip, iop);
	FILE *fp = IOP_READ(iop) ? sip->rfp : sip->wfp;

	if (fp)
		fprintf(fp, "%15.9lf %13lld\n", BIT_TIME(iop->t.time), dist);

	dist = llabs(dist);
	sip->total_seeks++;
	sip->total_sectors += dist;

	left = 0;
	right = sip->nseek_bkts-1;
	while (left <= right) {
		mid = (left+right)/2;
		bkt = &sip->seek_bkts[mid];
		if (dist == bkt->sectors) {
			bkt->nseeks++;
			return;
		}

		if (dist > bkt->sectors)
			left = mid + 1;
		else
			right = mid - 1;
	}

	sip->seek_bkts = realloc(sip->seek_bkts, 
			         (sip->nseek_bkts+1) * sizeof(struct seek_bkt));
	if (sip->nseek_bkts > left)
		memmove(&sip->seek_bkts[left+1], &sip->seek_bkts[left], 
			(sip->nseek_bkts - left) * sizeof(struct seek_bkt));
	(bkt = &sip->seek_bkts[left])->sectors = dist;
	bkt->nseeks = 1;
	sip->nseek_bkts++;
}

long long seeki_nseeks(void *handle)
{
	return ((struct seeki *)handle)->total_seeks;
}

double seeki_mean(void *handle)
{
	struct seeki *sip = handle;
	return sip->total_sectors / sip->total_seeks;
}

long long seeki_median(void *handle)
{
	int i;
	struct seek_bkt *p;
	struct seeki *sip = handle;
	long long sofar = 0, target = sip->total_seeks / 2;

	if (sip->total_seeks == 0) return 0;
	for (i = 0, p = sip->seek_bkts; i < sip->nseek_bkts; i++, p++)
		if ((sofar + p->nseeks) < target)
			sofar += p->nseeks;
		else
			break;

	return p->sectors;
}

int seeki_mode(void *handle, long long **modes_p, int *nseeks_p)
{
	int i;
	struct seek_bkt *p;
	int most_seeks = 0;
	struct seeki *sip = handle;
	int nmodes = 0;
	long long *modes = NULL;

	for (i = 0, p = sip->seek_bkts; i < sip->nseek_bkts; i++, p++)
		if ((modes == NULL) || (p->nseeks > most_seeks)) {
			most_seeks = p->nseeks;
			modes = realloc(modes, sizeof(long long));
			*modes = p->sectors;
			nmodes = 1;
		}
		else if (p->nseeks == most_seeks) {
			most_seeks = p->nseeks;
			modes = realloc(modes, (nmodes+1) * sizeof(long long));
			modes[nmodes++] = p->sectors;
		}

	*nseeks_p = most_seeks;
	*modes_p = modes;
	return nmodes;
}
