#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
	double this_time, last_time;
	char line[256], *p;
	int major, minor, cpu, seq, nr, alias, last_seq;
	FILE *f;

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
	last_seq = alias = nr = 0;
	while ((p = fgets(line, sizeof(line), f)) != NULL) {
		if (sscanf(p, "%3d,%3d %2d %8d %lf", &major, &minor, &cpu, &seq, &this_time) == -1)
			break;

		if (this_time < last_time) {
			fprintf(stdout, "%s", p);
			nr++;
		} else
			last_time = this_time;

		if (last_seq == seq) {
			fprintf(stdout, "alias on sequence %u\n", seq);
			alias++;
		}

		last_seq = seq;
	}

	fprintf(stdout, "%d unordered events, %d aliases\n", nr, alias);
	fclose(f);

	return nr != 0;
}
