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
#include "globals.h"

typedef struct avg_info *ai_dip_t;
ai_dip_t dip_q2q_avg(struct d_info *dip) { return &dip->avgs.q2q; }
ai_dip_t dip_q2c_avg(struct d_info *dip) { return &dip->avgs.q2c; }
ai_dip_t dip_q2a_avg(struct d_info *dip) { return &dip->avgs.q2a; }
ai_dip_t dip_q2i_avg(struct d_info *dip) { return &dip->avgs.q2i; }
ai_dip_t dip_i2d_avg(struct d_info *dip) { return &dip->avgs.i2d; }
ai_dip_t dip_d2c_avg(struct d_info *dip) { return &dip->avgs.d2c; }

typedef struct avg_info *ai_pip_t;
ai_pip_t pip_q2q_avg(struct p_info *pip) { return &pip->avgs.q2q; }
ai_pip_t pip_q2c_avg(struct p_info *pip) { return &pip->avgs.q2c; }
ai_pip_t pip_q2a_avg(struct p_info *pip) { return &pip->avgs.q2a; }
ai_pip_t pip_q2i_avg(struct p_info *pip) { return &pip->avgs.q2i; }
ai_pip_t pip_i2d_avg(struct p_info *pip) { return &pip->avgs.i2d; }
ai_pip_t pip_d2c_avg(struct p_info *pip) { return &pip->avgs.d2c; }

void output_section_hdr(FILE *ofp, char *hdr)
{
	fprintf(ofp, "==================== ");
	fprintf(ofp, hdr);
	fprintf(ofp, " ====================\n\n");
}

void output_hdr(FILE *ofp, char *hdr)
{
	fprintf(ofp, "%15s %13s %13s %13s %11s\n",
	        hdr, "MIN", "AVG", "MAX", "N" );
	fprintf(ofp, "--------------- ------------- ------------- ------------- -----------\n");
}

void __output_avg(FILE *ofp, char *hdr, struct avg_info *ap)
{
	if (ap->n > 0) {
		ap->avg = BIT_TIME(ap->total) / (double)ap->n;
		fprintf(ofp, "%-15s %13.9f %13.9f %13.9f %11d\n", hdr,
			BIT_TIME(ap->min), ap->avg, BIT_TIME(ap->max), ap->n);
	}
}

void output_hdr2(FILE *ofp, char*hdr)
{
	fprintf(ofp, "%15s %13s %13s %13s %13s %13s %13s\n", hdr, "Q2Q", "Q2A", "Q2I", "I2D", "D2C", "Q2C");
	fprintf(ofp, "--------------- ------------- ------------- ------------- ------------- ------------- -------------\n");
}

static inline char *avg2string(struct avg_info *ap, char *string)
{
	if (ap->n > 0)
		sprintf(string, "%13.9f", ap->avg);
	else
		sprintf(string, " ");
	return string;
}

void __output_avg2(FILE *ofp, char *hdr, struct avgs_info *ap)
{
	char c1[16], c2[16], c3[16], c4[16], c5[16], c6[16];

	if (ap->q2q.n > 0 || ap->q2a.n > 0 || ap->q2i.n > 0 ||
			ap->i2d.n > 0 || ap->d2c.n > 0 || ap->q2c.n > 0) {
		fprintf(ofp, "%-15s %13s %13s %13s %13s %13s %13s\n", hdr,
			avg2string(&ap->q2q,c1), avg2string(&ap->q2a,c2),
			avg2string(&ap->q2i,c3), avg2string(&ap->i2d,c4),
			avg2string(&ap->d2c,c5), avg2string(&ap->q2c,c6));
	}
}

void __pip_output_avg2(struct p_info *pip, void *arg)
{
	__output_avg2((FILE *)arg, pip->name, &pip->avgs);
}

void __dip_output_avg2(struct d_info *dip, void *arg)
{
	char dev_info[15];
	__output_avg2((FILE *)arg, make_dev_hdr(dev_info, 15, dip), &dip->avgs);
}

char *make_dev_hdr(char *pad, size_t len, struct d_info *dip)
{
	if (dip->map == NULL)
		snprintf(pad, len, "(%3d,%3d)", 
			 MAJOR(dip->device), MINOR(dip->device));
	else
		snprintf(pad, len, "%s", dip->map->device);

	return pad;
}

struct __oda {
	FILE *ofp;
	ai_dip_t (*func)(struct d_info *);
};
void __output_dip_avg(struct d_info *dip, void *arg)
{
	struct __oda *odap = arg;
	ai_dip_t ap = odap->func(dip);
	if (ap->n > 0) {
		char dev_info[15];
		ap->avg = BIT_TIME(ap->total) / (double)ap->n;
		__output_avg(odap->ofp, make_dev_hdr(dev_info, 15, dip), ap);
	}
}

void output_dip_avg(FILE *ofp, char *hdr, ai_dip_t (*func)(struct d_info *))
{
	struct __oda oda = { .ofp = ofp, .func = func};
	output_hdr(ofp, hdr);
	dip_foreach_out(__output_dip_avg, &oda);
	fprintf(ofp, "\n");
}

int n_merges = 0;
struct {
	unsigned long long nq, nd, blkmin, blkmax, total;
} merge_data;
void __output_dip_merge_ratio(struct d_info *dip, void *arg)
{
	double blks_avg;
	char scratch[15];
	double ratio, q2c_n = dip->avgs.q2c.n, d2c_n = dip->n_ds;

	if (q2c_n > 0.0 && d2c_n > 0.0) {
		ratio = q2c_n / d2c_n;
		blks_avg = (double)dip->avgs.blks.total / d2c_n;
		fprintf((FILE *)arg, 
			"%10s | %8llu %8llu %7.1lf | %8llu %8llu %8llu %8llu\n",
			make_dev_hdr(scratch, 15, dip),
			(unsigned long long)dip->avgs.q2c.n,
			(unsigned long long)dip->n_ds,
			ratio,
			(unsigned long long)dip->avgs.blks.min,
			(unsigned long long)blks_avg,
			(unsigned long long)dip->avgs.blks.max,
			(unsigned long long)dip->avgs.blks.total);

		if (n_merges++ == 0) {
			merge_data.blkmin = dip->avgs.blks.min;
			merge_data.blkmax = dip->avgs.blks.max;
		}

		merge_data.nq += dip->avgs.q2c.n;
		merge_data.nd += dip->n_ds;
		merge_data.total += dip->avgs.blks.total;
		if (dip->avgs.blks.min < merge_data.blkmin)
			merge_data.blkmin = dip->avgs.blks.min;
		if (dip->avgs.blks.max > merge_data.blkmax)
			merge_data.blkmax = dip->avgs.blks.max;
	}
}

void output_dip_merge_ratio(FILE *ofp)
{
	fprintf(ofp, "%10s | %8s %8s %7s | %8s %8s %8s %8s\n", "DEV", "#Q", "#D", "Ratio", "BLKmin", "BLKavg", "BLKmax", "Total");
	fprintf(ofp, "---------- | -------- -------- ------- | -------- -------- -------- --------\n");
	dip_foreach_out(__output_dip_merge_ratio, ofp);
	if (n_merges > 1) {
		fprintf(ofp, "---------- | -------- -------- ------- | -------- -------- -------- --------\n");
		fprintf(ofp, "%10s | %8s %8s %7s | %8s %8s %8s %8s\n", "DEV", "#Q", "#D", "Ratio", "BLKmin", "BLKavg", "BLKmax", "Total");
		fprintf((FILE *)ofp, 
			"%10s | %8llu %8llu %7.1lf | %8llu %8llu %8llu %8llu\n",
			"TOTAL", merge_data.nq, merge_data.nd, 
			(float)merge_data.nq / (float)merge_data.nd,
			merge_data.blkmin, 
			merge_data.total / merge_data.nd,
			merge_data.blkmax, merge_data.total);
	}
	fprintf(ofp, "\n");
}

#define AVG(a,b) (100.0 * ((double)(a) / (double)(b)))
#define CALC_AVG(ap) (ap)->avg = ((ap)->n == 0 ? 0.0 :			  \
                                                 (BIT_TIME((ap)->total) / \
							(double)(ap)->n))
char *q2i_v_q2C(struct d_info *dip, char *s)
{
	double q2c;

	if (dip->avgs.q2i.n == 0) return " ";

	q2c = dip->avgs.q2i.avg + dip->avgs.i2d.avg + dip->avgs.d2c.avg;
	sprintf(s, "%5.1lf%%", AVG(dip->avgs.q2i.avg, q2c));

	return s;
}

char *i2d_v_q2C(struct d_info *dip, char *s)
{
	double q2c;

	if (dip->avgs.d2c.n == 0) return " ";

	q2c = dip->avgs.q2i.avg + dip->avgs.i2d.avg + dip->avgs.d2c.avg;
	sprintf(s, "%5.1lf%%", AVG(dip->avgs.i2d.avg, q2c));

	return s;
}

char *d2c_v_q2C(struct d_info *dip, char *s)
{
	double q2c;

	if (dip->avgs.d2c.n == 0) return " ";

	q2c = dip->avgs.q2i.avg + dip->avgs.i2d.avg + dip->avgs.d2c.avg;
	sprintf(s, "%5.1lf%%", AVG(dip->avgs.d2c.avg, q2c));

	return s;
}

void __output_dip_prep_ohead(struct d_info *dip, void *arg)
{
	char dev_info[15];
	char s1[16], s2[16], s3[16];

	if ((dip->avgs.q2i.n > 0 && dip->avgs.i2d.n > 0 &&
						dip->avgs.d2c.n > 0)) {
		CALC_AVG(&dip->avgs.q2i);
		CALC_AVG(&dip->avgs.i2d);
		CALC_AVG(&dip->avgs.d2c);

		fprintf((FILE *)arg, "%10s | %6s %6s %6s\n",
			make_dev_hdr(dev_info, 15, dip),
			q2i_v_q2C(dip, s1), i2d_v_q2C(dip, s2),
			d2c_v_q2C(dip, s3));
	}
}

void output_dip_prep_ohead(FILE *ofp)
{
	fprintf(ofp, "%10s | %6s %6s %6s\n", "DEV", "Q2I", "I2D", "D2C");
	fprintf(ofp, "---------- | ------ ------ ------\n");
	dip_foreach_out(__output_dip_prep_ohead, ofp);
	fprintf(ofp, "\n");
}

int n_seeks;
struct seek_mode_info {
	struct seek_mode_info *next;
	long long mode;
	int nseeks;
};
struct o_seek_info {
	long long nseeks, median;
	double mean;
	struct seek_mode_info *head;
} seek_info = {
	.nseeks = 0L,
	.median = 0L,
	.mean = 0.0,
	.head = NULL
};
void output_seek_mode_info(FILE *ofp, struct o_seek_info *sip)
{
	struct seek_mode_info *p, *this, *new_list = NULL;

	ASSERT(sip->head != NULL);
	while ((this = sip->head) != NULL) {
		sip->head = this->next;
		if (new_list == NULL) {
			this->next = NULL;
			new_list = this;
			continue;
		}
		if (this->nseeks < new_list->nseeks) {
			free(this);
			continue;
		}
		if (this->nseeks > new_list->nseeks) {
			while ((p = new_list) != NULL) {
				new_list = p->next;
				free(p);
			}
			this->next = NULL;
			new_list = this;
			continue;
		}
		for (p = new_list; p; p++) 
			if (p->mode == this->mode) {
				this->nseeks += p->nseeks;
				free(p);
				break;
			}
		if (p)
			p->next = new_list;
	}

	fprintf(ofp, "%10s | %15lld %15.1lf %15lld | %lld(%d)",
	        "Average", sip->nseeks, sip->mean / sip->nseeks, 
		sip->median / sip->nseeks, new_list->mode, new_list->nseeks);

	p = new_list;
	new_list = p->next;
	free(p);
	while((p = new_list) != NULL) {
		fprintf(ofp, " %lld", p->mode);
		new_list = p->next;
		free(p);
	}
}
void add_seek_mode_info(struct o_seek_info *sip, struct mode *mp)
{
	int i;
	long long *lp = mp->modes;
	struct seek_mode_info *smip;

	n_seeks++;
	for (i = 0; i < mp->nmds; i++) {
		for (smip = sip->head; smip; smip = smip->next) {
			if (smip->mode == lp[i]) {
				smip->nseeks += mp->most_seeks;
				break;
			}
		}
		if (!smip) {
			struct seek_mode_info *new = malloc(sizeof(*new));

			new->next = sip->head; 
			sip->head = new;
			new->mode = lp[i];
			new->nseeks = mp->most_seeks;
		}
	}
}

void __output_dip_seek_info(struct d_info *dip, void *arg)
{
	double mean;
	int i, nmodes;
	long long nseeks;
	char dev_info[15];
	long long median;
	struct mode m;
	FILE *ofp = arg;

	nseeks = seeki_nseeks(dip->seek_handle);
	if (nseeks > 0) {
		mean = seeki_mean(dip->seek_handle);
		median = seeki_median(dip->seek_handle);
		nmodes = seeki_mode(dip->seek_handle, &m);

		fprintf(ofp, "%10s | %15lld %15.1lf %15lld | %lld(%d)",
			make_dev_hdr(dev_info, 15, dip), nseeks, mean, median, 
			nmodes > 0 ? m.modes[0] : 0, m.most_seeks);
		for (i = 1; i < nmodes; i++)
			fprintf(ofp, " %lld", m.modes[i]);
		fprintf(ofp, "\n");

		seek_info.nseeks += nseeks;
		seek_info.mean += (nseeks * mean);
		seek_info.median += (nseeks * median);
		add_seek_mode_info(&seek_info, &m);
	}
}

void output_dip_seek_info(FILE *ofp)
{
	fprintf(ofp, "%10s | %15s %15s %15s | %-15s\n", "DEV", "NSEEKS", 
			"MEAN", "MEDIAN", "MODE");
	fprintf(ofp, "---------- | --------------- --------------- --------------- | ---------------\n");
	dip_foreach_out(__output_dip_seek_info, ofp);
	if (n_seeks > 1) {
		fprintf(ofp, "---------- | --------------- --------------- --------------- | ---------------\n");
		fprintf(ofp, "%10s | %15s %15s %15s | %-15s\n", 
		        "Overall", "NSEEKS", "MEAN", "MEDIAN", "MODE");
		output_seek_mode_info(ofp, &seek_info);
	}
	fprintf(ofp, "\n");
}

struct __opa {
	FILE *ofp;
	ai_pip_t (*func)(struct p_info *);
};

void __output_pip_avg(struct p_info *pip, void *arg)
{
	struct __opa *opap = arg;
	ai_pip_t ap = opap->func(pip);

	if (ap->n > 0) {
		char proc_name[15];
		snprintf(proc_name, 15, pip->name);

		ap->avg = BIT_TIME(ap->total) / (double)ap->n;
		__output_avg(opap->ofp, proc_name, ap);
	}
}

void output_pip_avg(FILE *ofp, char *hdr, ai_pip_t (*func)(struct p_info *))
{
	struct __opa opa = { .ofp = ofp, .func = func };

	output_hdr(ofp, hdr);
	pip_foreach_out(__output_pip_avg, &opa);
	fprintf(ofp, "\n");
}

void output_dip_avgs(FILE *ofp)
{
	output_hdr2(ofp,"Dev");
	dip_foreach_out(__dip_output_avg2, ofp);
	fprintf(ofp, "\n");
}

void output_pip_avgs(FILE *ofp)
{
	output_hdr2(ofp,"Exe");
	pip_foreach_out(__pip_output_avg2, ofp);
	fprintf(ofp, "\n");
}

void __dip_output_plug(struct d_info *dip, void *arg)
{
	char dev_info[15];
	FILE *ofp = arg;
	double delta;

	if (dip->is_plugged) dip_unplug(dip->device, dip->end_time, 0);

	fprintf(ofp, "%10s | %10s %10s  | %s\n", 
	        "DEV", "# Plugs", "# Timer Us", "% Time Q Plugged");
	fprintf(ofp, "---------- | ---------- ----------  | ----------------\n");
	delta = dip->end_time - dip->start_time;
	fprintf(ofp, "%10s | %10d(%10d) | %6.2lf%%\n", 
		make_dev_hdr(dev_info, 15, dip), 
		dip->nplugs, dip->n_timer_unplugs, 
		100.0 * ((dip->plugged_time / delta) / delta));
}

void output_plug_info(FILE *ofp)
{
	dip_foreach_out(__dip_output_plug, ofp);
	fprintf(ofp, "\n");
}

int output_avgs(FILE *ofp)
{
	if (exes == NULL || *exes != '\0') {
		output_section_hdr(ofp, "Per Process");
		output_pip_avg(ofp, "Q2Q", pip_q2q_avg);
		output_pip_avg(ofp, "Q2A", pip_q2a_avg);
		output_pip_avg(ofp, "Q2I", pip_q2i_avg);
		output_pip_avg(ofp, "I2D", pip_i2d_avg);
		output_pip_avg(ofp, "D2C", pip_d2c_avg);
		output_pip_avg(ofp, "Q2C", pip_q2c_avg);
	}

	output_section_hdr(ofp, "Per Device");
	output_dip_avg(ofp, "Q2Q", dip_q2q_avg);
	output_dip_avg(ofp, "Q2A", dip_q2a_avg);
	output_dip_avg(ofp, "Q2I", dip_q2i_avg);
	output_dip_avg(ofp, "I2D", dip_i2d_avg);
	output_dip_avg(ofp, "D2C", dip_d2c_avg);
	output_dip_avg(ofp, "Q2C", dip_q2c_avg);

	output_section_hdr(ofp, "All Devices");
	output_hdr(ofp, "ALL");
	__output_avg(ofp, "Q2Q", &all_avgs.q2q);
	__output_avg(ofp, "Q2A", &all_avgs.q2a);
	__output_avg(ofp, "Q2I", &all_avgs.q2i);
	__output_avg(ofp, "I2D", &all_avgs.i2d);
	__output_avg(ofp, "D2C", &all_avgs.d2c);
	__output_avg(ofp, "Q2C", &all_avgs.q2c);
	fprintf(ofp, "\n");

	output_section_hdr(ofp, "Device Overhead");
	output_dip_prep_ohead(ofp);

	if (exes == NULL || *exes != '\0') {
		output_section_hdr(ofp, "Per Process (avgs)");
		output_pip_avgs(ofp);
	}

	output_section_hdr(ofp, "Per Device (avgs)");
	output_dip_avgs(ofp);

	output_section_hdr(ofp, "Device Merge Information");
	output_dip_merge_ratio(ofp);

	output_section_hdr(ofp, "Device Seek Information");
	output_dip_seek_info(ofp);

	output_section_hdr(ofp, "Plug Information");
	output_plug_info(ofp);

	return 0;
}

void __output_ranges(FILE *ofp, struct list_head *head_p, float base)
{
	struct range_info *rip;
	struct list_head *p;
	float limit = base + 0.4;

	__list_for_each(p, head_p) {
		rip = list_entry(p, struct range_info, head);
		fprintf(ofp, "%13.9lf %5.1f\n", BIT_TIME(rip->start), base);
		fprintf(ofp, "%13.9lf %5.1f\n", BIT_TIME(rip->start), limit);
		fprintf(ofp, "%13.9lf %5.1f\n", BIT_TIME(rip->end), limit);
		fprintf(ofp, "%13.9lf %5.1f\n", BIT_TIME(rip->end), base);
	}
}

int output_regions(FILE *ofp, char *header, struct region_info *reg, 
			  float base)
{
	if (reg->qr_cur != NULL)
		list_add_tail(&reg->qr_cur->head, &reg->qranges);
	if (reg->cr_cur != NULL)
		list_add_tail(&reg->cr_cur->head, &reg->cranges);

	if (list_len(&reg->qranges) == 0 && list_len(&reg->cranges) == 0)
		return 0;

	fprintf(ofp, "# %16s : q activity\n", header);
	__output_ranges(ofp, &reg->qranges, base);
	fprintf(ofp, "\n");

	fprintf(ofp, "# %16s : c activity\n", header);
	__output_ranges(ofp, &reg->cranges, base + 0.5);
	fprintf(ofp, "\n");

	return 1;
}

struct __od {
	FILE *ofp;
	float base;
};
void __output_dev(struct d_info *dip, void *arg)
{
	char header[128];
	struct __od *odp = arg;

	sprintf(header, "%d,%d", MAJOR(dip->device), MINOR(dip->device));
	if (output_regions(odp->ofp, header, &dip->regions, odp->base))
		odp->base += 1.0;
}

float output_devs(FILE *ofp, float base)
{
	struct __od od = { .ofp = ofp, .base = base };

	fprintf(ofp, "# Per device\n" );
	dip_foreach_out(__output_dev, &od);
	return od.base;
}

static inline int exe_match(char *exe, char *name)
{
	return (exe == NULL) || (strstr(name, exe) != NULL);
}

struct __op {
	FILE *ofp;
	float base;
};
void __output_procs(struct p_info *pip, void *arg)
{
	struct __op *opp = arg;
	output_regions(opp->ofp, pip->name, &pip->regions, opp->base);
	opp->base += 1.0;
}

float output_procs(FILE *ofp, float base)
{
	struct __op op = { .ofp = ofp, .base = base };

	fprintf(ofp, "# Per process\n" );
	pip_foreach_out(__output_procs, &op);
	return op.base;
}

int output_ranges(FILE *ofp)
{
	float base = 0.0;

	fprintf(ofp, "# %s\n", "Total System");
	if (output_regions(ofp, "Total System", &all_regions, base))
		base += 1.0;

	if (n_devs > 1)
		base = output_devs(ofp, base);

	base = output_procs(ofp, base);

	return 0;
}
