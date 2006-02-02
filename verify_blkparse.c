#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

#define MAX_CPUS	(512)

int main(int argc, char *argv[])
{
	double this_time, last_time;
	char line[256], *p;
	int major, minor, cpu, seq, nr, alias;
	unsigned long long total_entries;
	unsigned long last_seq[MAX_CPUS];
	FILE *f;

	for (nr = 0; nr < MAX_CPUS; nr++)
		last_seq[nr] = -1;

	if (argc < 2) {
		fprintf(stderr, "%s: file\n", argv[0]);
		return 1;
	}

	f = fopen(argv[1], "r");
	if (!f) {
		perror("fopen");
		return 1;
	}

	last_time = 0;
	alias = nr = 0;
	total_entries = 0;
	while ((p = fgets(line, sizeof(line), f)) != NULL) {
		if (sscanf(p, "%3d,%3d %2d %8d %lf", &major, &minor, &cpu, &seq, &this_time) != 5)
			break;

		if (this_time < last_time) {
			fprintf(stdout, "%s", p);
			nr++;
		} else
			last_time = this_time;

		if (cpu >= MAX_CPUS) {
			fprintf(stderr, "cpu%d too large\n", cpu);
			break;
		}

		if (last_seq[cpu] == seq) {
			fprintf(stdout, "alias on sequence %u\n", seq);
			alias++;
		}

		last_seq[cpu] = seq;
		total_entries++;
	}

	fprintf(stdout, "Events %Lu: %d unordered, %d aliases\n", total_entries, nr, alias);
	fclose(f);

	return nr != 0;
}
