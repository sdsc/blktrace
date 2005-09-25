/*
 * This file contains format parsing code for blkparse, allowing you to
 * customize the individual action format and generel output format.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>

#include "blktrace.h"

#define VALID_SPECS	"BCDFGMPQRSTU"

#define HEADER		"%D %2c %8s %5T.%9t %5p %2a %3d "

static char *override_format[256];

static inline int valid_spec(int spec)
{
	return strchr(VALID_SPECS, spec) != NULL;
}

void set_all_format_specs(char *optarg)
{
	char *p;

	for (p = VALID_SPECS; *p; p++)
		if (override_format[(int)(*p)] == NULL)
			override_format[(int)(*p)] = strdup(optarg);
}

int add_format_spec(char *optarg)
{
	int spec = optarg[0];

	if (!valid_spec(spec)) {
		fprintf(stderr,"Bad format specifier %c\n", spec);
		return 1;
	}
	if (optarg[1] != ',') {
		fprintf(stderr,"Bad format specifier - need ',' %s\n", optarg);
		return 1;
	}
	optarg += 2;
	if (*optarg == '\0') {
		fprintf(stderr,"Bad format specifier - need fmt %s\n", optarg);
		return 1;
	}

	/*
	 * Set both merges (front and back)
	 */
	if (spec == 'M') {
		override_format['B'] = strdup(optarg);
		override_format['M'] = strdup(optarg);
	} else
		override_format[spec] = strdup(optarg);

	return 0;
}

static void print_field(char *act, struct per_cpu_info *pci,
			struct blk_io_trace *t, unsigned long long elapsed,
			int pdu_len, unsigned char *pdu_buf, char field,
			int minus, int has_w, int width)
{
	char format[64];

	if (has_w) {
		if (minus)
			sprintf(format, "%%-%d", width);
		else
			sprintf(format, "%%%d", width);
	} else
		sprintf(format, "%%");

	switch (field) {
	case 'a':
		fprintf(ofp, strcat(format, "s"), act);
		break;
	case 'c':
		fprintf(ofp, strcat(format, "d"), pci->cpu);
		break;
	case 'C':
		fprintf(ofp, strcat(format, "s"), t->comm);
		break;
	case 'd': {
		char rwbs[4];
		int i = 0;
		int w = t->action & BLK_TC_ACT(BLK_TC_WRITE);
		int b = t->action & BLK_TC_ACT(BLK_TC_BARRIER);
		int s = t->action & BLK_TC_ACT(BLK_TC_SYNC);
		if (w)
			rwbs[i++] = 'W';
		else
			rwbs[i++] = 'R';
		if (b)
			rwbs[i++] = 'B';
		if (s)
			rwbs[i++] = 'S';
		rwbs[i] = '\0';
		fprintf(ofp, strcat(format, "s"), rwbs);
		break;
	}
	case 'D':	/* format width ignored */
		fprintf(ofp,"%3d,%-3d", MAJOR(t->device), MINOR(t->device));
		break;
	case 'e':
		fprintf(ofp, strcat(format, "d"), t->error);
		break;
	case 'M':
		fprintf(ofp, strcat(format, "d"), MAJOR(t->device));
		break;
	case 'm':
		fprintf(ofp, strcat(format, "d"), MINOR(t->device));
		break;
	case 'n':
		fprintf(ofp, strcat(format, "u"), t->bytes >> 9);
		break;
	case 'p':
		fprintf(ofp, strcat(format, "u"), t->pid);
		break;
	case 'P':	/* format width ignored */
		if ((pdu_len > 0) && (pdu_buf != NULL)) {
			int i;
			unsigned char *p = pdu_buf;
			for (i = 0; i < pdu_len; i++) {
				if (i)
					fprintf(ofp, " ");

				fprintf(ofp, "%02x", *p++);
			}
		}
		break;
	case 's':
		fprintf(ofp, strcat(format, "ld"), t->sequence);
		break;
	case 'S':
		fprintf(ofp, strcat(format, "lu"), t->sector);
		break;
	case 't':
		sprintf(format, "%%0%dlu", has_w ? width : 9);
		fprintf(ofp, format, NANO_SECONDS(t->time));
		break;
	case 'T':
		fprintf(ofp, strcat(format, "d"), SECONDS(t->time));
		break;
	case 'u':
		if (elapsed == -1ULL) {
			fprintf(stderr, "Expecting elapsed value\n");
			exit(1);
		}
		fprintf(ofp, strcat(format, "llu"), elapsed / 1000);
		break;
	case 'U': {
		__u64 *depth = (__u64 *) ((char *) t + sizeof(*t));
		fprintf(ofp, strcat(format, "u"),
					(unsigned int) be64_to_cpu(*depth));
		break;
	}
	default:
		fprintf(ofp,strcat(format, "c"), field);
		break;
	}
}

static char *parse_field(char *act, struct per_cpu_info *pci, 
			 struct blk_io_trace *t, unsigned long long elapsed, 
			 int pdu_len, unsigned char *pdu_buf, 
			 char *master_format)
{
	int minus = 0;
	int has_w = 0;
	int width = 0;
	char *p = master_format;

	if (*p == '-') {
		minus = 1;
		p++;
	}
	if (isdigit(*p)) {
		has_w = 1;
		do {
			width = (width * 10) + (*p++ - '0');
		} while ((*p) && (isdigit(*p)));
	}
	if (*p) {
		print_field(act, pci, t, elapsed, pdu_len, pdu_buf, *p++,
			    minus, has_w, width);
	}
	return p;
}

static char *fmt_select(int fmt_spec, struct blk_io_trace *t,
			unsigned long long elapsed)
{
	char *fmt;
	static char scratch_format[1024];

	if (override_format[fmt_spec] != NULL)
		return override_format[fmt_spec];

	switch (fmt_spec) {
	case 'C': 	/* Complete */
		if (t->action & BLK_TC_ACT(BLK_TC_PC))
			strcpy(scratch_format, HEADER "(%P) [%e]\n");
		else {
			if (elapsed != -1ULL) {
				strcpy(scratch_format,
					HEADER "%S +%n (%8u) [%e]\n");
			} else
				strcpy(scratch_format, HEADER "%S + %n [%e]\n");
		}
		fmt = scratch_format;
		break;

	case 'D': 	/* Issue */
		if (t->action & BLK_TC_ACT(BLK_TC_PC))
			strcpy(scratch_format, HEADER "%n (%P) [%C]\n");
		else {
			if (elapsed != -1ULL) {
				strcpy(scratch_format,
					HEADER "%S + %n (%8u) [%C]\n");
			} else
				strcpy(scratch_format, HEADER "%S + %n [%C]\n");
		}
		fmt = scratch_format;
		break;

	case 'I': 	/* Insert */
		if (t->action & BLK_TC_ACT(BLK_TC_PC))
			strcpy(scratch_format, HEADER "%n (%P) [%C]\n");
		else {
			if (elapsed != -1ULL) {
				strcpy(scratch_format,
					HEADER "%S + %n (%8u) [%C]\n");
			} else
				strcpy(scratch_format, HEADER "%S + %n [%C]\n");
		}
		fmt = scratch_format;
		break;

	case 'Q': 	/* Queue */
	case 'W':	/* Bounce */
		if (elapsed != -1ULL) {
			strcpy(scratch_format, HEADER "%S + %n (%8u) [%C]\n");
		} else
			strcpy(scratch_format, HEADER "%S + %n [%C]\n");
		fmt = scratch_format;
		break;

	case 'B':	/* Back merge */
	case 'F':	/* Front merge */
	case 'M':	/* Front or back merge */
		fmt = HEADER "%S + %n [%C]\n";
		break;

	case 'P':	/* Plug */
		fmt = HEADER "[%C]\n";
		break;

	case 'G':	/* Get request */
	case 'S':	/* Sleep request */
		fmt = HEADER "%S + %n [%C]\n";
		break;

	case 'U':	/* Unplug IO */
	case 'T':	/* Unplug timer */
		fmt = HEADER "[%C] %U\n";
		break;

	case 'X': 	/* Split */
		strcpy(scratch_format, HEADER "%S / %U [%C]\n");
		fmt = scratch_format;
		break;

	default:
		fprintf(stderr,"FATAL: Invalid format spec %c\n", fmt_spec);
		exit(1);
		/*NOTREACHED*/
	}

	return fmt;
}

void process_fmt(char *act, struct per_cpu_info *pci, struct blk_io_trace *t,
		 unsigned long long elapsed, int pdu_len,
		 unsigned char *pdu_buf)
{
	char *p = fmt_select(act[0], t, elapsed);

	while (*p) {
		switch (*p) {
		case '%': 	/* Field specifier */
			p++;
			if (*p == '%')
				fprintf(ofp, "%c", *p++);
			else if (!*p)
				fprintf(ofp, "%c", '%');
			else
				p = parse_field(act, pci, t, elapsed,
						pdu_len, pdu_buf, p);
			break;
		case '\\': {	/* escape */
			switch (p[1]) {
			case 'b': fprintf(ofp, "\b"); break;
			case 'n': fprintf(ofp, "\n"); break;
			case 'r': fprintf(ofp, "\r"); break;
			case 't': fprintf(ofp, "\t"); break;
			default:
				fprintf(stderr,	
					"Invalid escape char in format %c\n",
					p[1]);
				exit(1);
				/*NOTREACHED*/
			}
			p += 2;
			break;
		}
		default:
			fprintf(ofp, "%c", *p++);
			break;
		}
	}
}


