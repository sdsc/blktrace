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

struct devmap *all_devmaps = NULL;

void dev_map_exit(void)
{
	struct devmap *dmp;

	while ((dmp = all_devmaps) != NULL) {
		all_devmaps = dmp->next;
		free(dmp);
	}
}

void dev_map_add(struct devmap *dmp)
{
	struct devmap *this = malloc(sizeof(struct devmap));

	*this = *dmp;
	this->next = all_devmaps;
	all_devmaps = this;
}

struct devmap *dev_map_find(__u32 device)
{
	char this[128];
	struct devmap *dmp;

	sprintf(this, "%u,%u", MAJOR(device), MINOR(device));
	for (dmp = all_devmaps; dmp != NULL; dmp = dmp->next)
		if (!strcmp(this, dmp->devno))
			break;

	return dmp;
}

int dev_map_read(char *fname)
{
	char line[256];
	struct devmap dm;
	FILE *fp = fopen(fname, "r");

	if (!fp) {
		perror(fname);
		return 1;
	}

	while (fscanf(fp, "%255[a-zA-Z0-9 :.,/_-]\n", line) == 1) {
		if (strstr(line, "Device") != NULL) continue;
		if (sscanf(line, "%s %s %u %u %u %u %s %s %u %u %s",
				&dm.device[0], &dm.model[0], &dm.host, &dm.bus,
				&dm.target, &dm.lun, &dm.node[0], &dm.pci[0],
				&dm.irq, &dm.cpu, &dm.devno[0]) != 11)
			break;
		dev_map_add(&dm);
	}

	return 0;
}
