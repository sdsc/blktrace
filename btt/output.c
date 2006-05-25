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
	fprintf(ofp, "%12s %13s %13s %13s %11s\n",
	        hdr, "MIN", "AVG", "MAX", "N" );
	fprintf(ofp, "------------ ------------- ------------- ------------- -----------\n");
}

void __output_avg(FILE *ofp, char *hdr, struct avg_info *ap)
{
	if (ap->n > 0) {
		ap->avg = BIT_TIME(ap->total) / (double)ap->n;
		fprintf(ofp, "%-12s %13.9f %13.9f %13.9f %11d\n", hdr,
			BIT_TIME(ap->min), ap->avg, BIT_TIME(ap->max), ap->n);
	}
}

void output_hdr2(FILE *ofp, char*hdr)
{
	fprintf(ofp, "%12s %13s %13s %13s %13s %13s %13s\n", hdr, "Q2Q", "Q2A", "Q2I", "I2D", "D2C", "Q2C");
	fprintf(ofp, "------------ ------------- ------------- ------------- ------------- ------------- -------------\n");
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
		fprintf(ofp, "%-12s %13s %13s %13s %13s %13s %13s\n", hdr,
			avg2string(&ap->q2q,c1), avg2string(&ap->q2a,c2),
			avg2string(&ap->q2i,c3), avg2string(&ap->i2d,c4),
			avg2string(&ap->d2c,c5), avg2string(&ap->q2c,c6));
	}
}

char *make_dev_hdr(char *pad, size_t len, struct d_info *dip)
{
	if (dip->map == NULL) {
		snprintf(pad, len, "(%3d,%3d)", 
			 MAJOR(dip->device), MINOR(dip->device));
	}
	else {
		snprintf(pad, len, "[%3d,%3d]",
			dip->map->host, dip->map->target);
	}

	return pad;
}

void __output_dip_avg(FILE *ofp, struct d_info *dip, struct avg_info *ap)
{
	if (ap->n > 0) {
		char dev_info[12];
		ap->avg = BIT_TIME(ap->total) / (double)ap->n;
		__output_avg(ofp, make_dev_hdr(dev_info, 12, dip), ap);
	}
}

void output_dip_avg(FILE *ofp, char *hdr, ai_dip_t (*func)(struct d_info *))
{
	struct d_info *dip;

	output_hdr(ofp, hdr);
	if (devices == NULL) {
		struct list_head *p;

		__list_for_each(p, &all_devs) {
			dip = list_entry(p, struct d_info, head);
			__output_dip_avg(ofp, dip, func(dip));
		}
	}
	else {
		int i;
		unsigned int mjr, mnr;
		char *p = devices;

		while (p && ((i = sscanf(p, "%u,%u", &mjr, &mnr)) == 2)) {
			dip = __dip_find((__u32)((mjr << MINORBITS) | mnr));
			__output_dip_avg(ofp, dip, func(dip));

			p = strchr(p, ';');
			if (p) p++;
		}
	}

	fprintf(ofp, "\n");
}

void __output_dip_merge_ratio(FILE *ofp, struct d_info *dip)
{
	double blks_avg;
	char scratch[12];
	double ratio, q2c_n = dip->avgs.q2c.n, d2c_n = dip->n_ds;

	if (q2c_n > 0.0 && d2c_n > 0.0) {
		ratio = q2c_n / d2c_n;
		blks_avg = (double)dip->avgs.blks.total /
							(double)dip->avgs.d2c.n;
		fprintf(ofp, "%10s | %8llu %8llu %7.1lf | %8llu %8llu %8llu %8llu\n",
			make_dev_hdr(scratch, 12, dip),
			(unsigned long long)dip->avgs.q2c.n,
			(unsigned long long)dip->n_ds,
			ratio,
			(unsigned long long)dip->avgs.blks.min,
			(unsigned long long)blks_avg,
			(unsigned long long)dip->avgs.blks.max,
			(unsigned long long)dip->avgs.blks.total);

	}
}

void output_dip_merge_ratio(FILE *ofp)
{
	struct d_info *dip;

	fprintf(ofp, "%10s | %8s %8s %7s | %8s %8s %8s %8s\n", "DEV", "#Q", "#D", "Ratio", "BLKmin", "BLKavg", "BLKmax", "Total");
	fprintf(ofp, "---------- | -------- -------- ------- | -------- -------- -------- --------\n");
	if (devices == NULL) {
		struct list_head *p;

		__list_for_each(p, &all_devs) {
			dip = list_entry(p, struct d_info, head);
			__output_dip_merge_ratio(ofp, dip);
		}
	}
	else {
		int i;
		unsigned int mjr, mnr;
		char *p = devices;

		while (p && ((i = sscanf(p, "%u,%u", &mjr, &mnr)) == 2)) {
			dip = __dip_find((__u32)((mjr << MINORBITS) | mnr));
			__output_dip_merge_ratio(ofp, dip);

			p = strchr(p, ';');
			if (p) p++;
		}
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

	if (dip->avgs.i2d.n == 0) return " ";

	q2c = dip->avgs.q2i.avg + dip->avgs.i2d.avg + dip->avgs.d2c.avg;
	sprintf(s, "%5.1lf%%", AVG(dip->avgs.i2d.avg, q2c));

	return s;
}

char *d2c_v_q2C(struct d_info *dip, char *s)
{
	double q2c;

	if (dip->avgs.i2d.n == 0) return " ";

	q2c = dip->avgs.q2i.avg + dip->avgs.i2d.avg + dip->avgs.d2c.avg;
	sprintf(s, "%5.1lf%%", AVG(dip->avgs.d2c.avg, q2c));

	return s;
}

void __output_dip_prep_ohead(FILE *ofp, struct d_info *dip)
{
	char dev_info[12];
	char s1[16], s2[16], s3[16];

	if ((dip->avgs.q2i.n > 0 && dip->avgs.i2d.n > 0 &&
						dip->avgs.d2c.n > 0)) {
		CALC_AVG(&dip->avgs.q2i);
		CALC_AVG(&dip->avgs.i2d);
		CALC_AVG(&dip->avgs.d2c);

		fprintf(ofp, "%10s | %6s %6s %6s\n",
			make_dev_hdr(dev_info, 12, dip),
			q2i_v_q2C(dip, s1), i2d_v_q2C(dip, s2),
			d2c_v_q2C(dip, s3));
	}
}

void output_dip_prep_ohead(FILE *ofp)
{
	struct d_info *dip;

	fprintf(ofp, "%10s | %6s %6s %6s\n", "DEV", "Q2I", "I2D", "D2C");
	fprintf(ofp, "---------- | ------ ------ ------\n");

	if (devices == NULL) {
		struct list_head *p;

		__list_for_each(p, &all_devs) {
			dip = list_entry(p, struct d_info, head);
			__output_dip_prep_ohead(ofp, dip);
		}
	}
	else {
		int i;
		unsigned int mjr, mnr;
		char *p = devices;

		while (p && ((i = sscanf(p, "%u,%u", &mjr, &mnr)) == 2)) {
			dip = __dip_find((__u32)((mjr << MINORBITS) | mnr));
			__output_dip_prep_ohead(ofp, dip);

			p = strchr(p, ';');
			if (p) p++;
		}
	}

	fprintf(ofp, "\n");
}

void __output_pip_avg(FILE *ofp, struct p_info *pip, struct avg_info *ap)
{
	if (ap->n > 0) {
		char proc_name[12];
		snprintf(proc_name, 12, pip->name);

		ap->avg = BIT_TIME(ap->total) / (double)ap->n;
		__output_avg(ofp, proc_name, ap);
	}
}

void output_pip_avg(FILE *ofp, char *hdr, ai_pip_t (*func)(struct p_info *))
{
	struct p_info *pip;

	output_hdr(ofp, hdr);
	if (exes == NULL) {
		struct list_head *p;

		__list_for_each(p, &all_procs) {
			pip = list_entry(p, struct p_info, head);
			__output_pip_avg(ofp, pip, func(pip));
		}
	}
	else {
		char *exe, *p, *next, *exes_save = strdup(exes);

		p = exes_save;
		while (exes_save != NULL) {
			exe = exes_save;
			if ((next = strchr(exes_save, ',')) != NULL) {
				*next = '\0';
				exes_save = next+1;
			}
			else
				exes_save = NULL;

			pip = find_process((__u32)-1, exe);
			if (pip)
				__output_pip_avg(ofp, pip, func(pip));
		}
	}

	fprintf(ofp, "\n");
}

void output_dip_avgs(FILE *ofp)
{
	char dev_info[12];
	struct d_info *dip;

	output_hdr2(ofp,"Dev");
	if (devices == NULL) {
		struct list_head *p;

		__list_for_each(p, &all_devs) {
			dip = list_entry(p, struct d_info, head);
			__output_avg2(ofp, make_dev_hdr(dev_info, 12, dip),
					&dip->avgs);
		}
	}
	else {
		int i;
		unsigned int mjr, mnr;
		char *p = devices;

		while (p && ((i = sscanf(p, "%u,%u", &mjr, &mnr)) == 2)) {
			dip = __dip_find((__u32)((mjr << MINORBITS) | mnr));
			__output_avg2(ofp, make_dev_hdr(dev_info, 12, dip),
					&dip->avgs);

			p = strchr(p, ';');
			if (p) p++;
		}
	}

	fprintf(ofp, "\n");
}

void output_pip_avgs(FILE *ofp)
{
	char exe[16];
	struct p_info *pip;

	output_hdr2(ofp,"Exe");
	if (exes == NULL) {
		struct list_head *p;

		__list_for_each(p, &all_procs) {
			pip = list_entry(p, struct p_info, head);
			snprintf(exe, 12, pip->name);
			__output_avg2(ofp, exe, &pip->avgs);
		}
	}
	else {
		char *exe, *p, *next, *exes_save = strdup(exes);

		p = exes_save;
		while (exes_save != NULL && *exes_save != '\0') {
			exe = exes_save;
			if ((next = strchr(exes_save, ',')) != NULL) {
				*next = '\0';
				exes_save = next+1;
			}
			else
				exes_save = NULL;

			pip = find_process((__u32)-1, exe);
			if (pip) {
				snprintf(exe, 12, pip->name);
				__output_avg2(ofp, exe, &pip->avgs);
			}
		}
	}

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

	if (exes == NULL || *exes != '\0') {
		output_section_hdr(ofp, "Per Process (avgs)");
		output_pip_avgs(ofp);
	}

	output_section_hdr(ofp, "Per Device (avgs)");
	output_dip_avgs(ofp);

	output_section_hdr(ofp, "Device Merge Information");
	output_dip_merge_ratio(ofp);

	output_section_hdr(ofp, "Device Overhead");
	output_dip_prep_ohead(ofp);

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

float __output_dev(FILE *ofp, struct d_info *dip, float base)
{
	char header[128];
	sprintf(header, "%d,%d", MAJOR(dip->device), MINOR(dip->device));
	if (output_regions(ofp, header, &dip->regions, base))
		base += 1.0;

	return base;
}

float output_devs(FILE *ofp, float base)
{
	struct d_info *dip;

	fprintf(ofp, "# Per device\n" );
	if (devices == NULL) {
		struct list_head *p;
		__list_for_each(p, &all_devs) {
			dip = list_entry(p, struct d_info, head);
			base = __output_dev(ofp, dip, base);
		}
	}
	else {
		int i;
		unsigned int mjr, mnr;
		char *p = devices;

		while (p && ((i = sscanf(p, "%u,%u", &mjr, &mnr)) == 2)) {
			dip = __dip_find((__u32)((mjr << MINORBITS) | mnr));
			ASSERT(dip);

			base = __output_dev(ofp, dip, base);

			p = strchr(p, ';');
			if (p) p++;
		}
	}

	return base;
}

static inline int exe_match(char *exe, char *name)
{
	return (exe == NULL) || (strstr(name, exe) != NULL);
}

float __output_procs(FILE *ofp, float base, char *match)
{
	struct p_info *pip;
	struct list_head *p;

	__list_for_each(p, &all_procs) {
		pip = list_entry(p, struct p_info, head);

		if (exe_match(match, pip->name) &&
				output_regions(ofp, pip->name,
					       &pip->regions, base))
			base += 1.0;
	}

	return base;
}

float output_procs(FILE *ofp, float base)
{
	fprintf(ofp, "# Per process\n" );
	if (exes == NULL)
		base = __output_procs(ofp, base, NULL);
	else {
		char *exe, *next, *p, *exes_save = strdup(exes);

		p = exes_save;
		while (exes_save != NULL) {
			exe = exes_save;
			if ((next = strchr(exes_save, ',')) != NULL) {
				*next = '\0';
				exes_save = next+1;
			}
			else
				exes_save = NULL;

			base = __output_procs(ofp, base, exe);
		}
		free(p);
	}

	return base;
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
