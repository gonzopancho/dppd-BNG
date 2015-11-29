/*
  Copyright(c) 2010-2015 Intel Corporation.
  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions
  are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in
      the documentation and/or other materials provided with the
      distribution.
    * Neither the name of Intel Corporation nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "cfgfile.h"

#include <rte_string_fns.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>

#include "parse_utils.h"
#include "log.h"
#include "quit.h"

#define UINT32_MAX_STR "4294967295"

/*
 * Allocate cfg_file structure.
 * Returns pointer to the allocated structure, NULL otherwise.
 */
struct cfg_file *cfg_open(const char *cfg_name)
{
	if (cfg_name == NULL) {
		plog_err("\tNo config file name provided\n");
		return NULL;
	}
	if (access(cfg_name, F_OK)) {
                plog_err("\tError opening config file '%s': %s\n", cfg_name, strerror(errno));
		return NULL;
	}

	FILE *pf = fopen(cfg_name, "rb");
	if (pf == NULL) {
		plog_err("\tError opening config file '%s'\n", cfg_name);
		return NULL;
	}

	struct cfg_file *pcfg = calloc(1, sizeof(struct cfg_file));

	if (pcfg == NULL) {
		fclose(pf);
		plog_err("\tCouldn't allocate memory for config file struct\n");
		return NULL;
	}

	pcfg->pfile = pf;
	pcfg->name = strdup(cfg_name);

	return pcfg;
}

/* Free memory allocated for cfg_file structure.
 * Returns 0 on success, -1 if the pointer to the pcfg is invalid */
int cfg_close(struct cfg_file *pcfg)
{
	if (pcfg == NULL) {
		return -1;
	}

	if (pcfg->name != NULL) {
		free(pcfg->name);
	}
	if (pcfg->err_section != NULL) {
		free(pcfg->err_section);
	}
	if (pcfg->pfile != NULL) {
		fclose(pcfg->pfile);
	}

	free(pcfg);
	return 0;
}

static int cfg_get_pos(struct cfg_file *pcfg, fpos_t *pos)
{
	pcfg->index_line = pcfg->line;
	return fgetpos(pcfg->pfile, pos);
}

static int cfg_set_pos(struct cfg_file *pcfg, fpos_t *pos)
{
	pcfg->line = pcfg->index_line;
	return fsetpos(pcfg->pfile, pos);
}

/*
 * Read a line from the configuration file.
 * Returns: on success length of the line read from the file is returned,
 *          0 to indicate End of File,
 *         -1 in case of wrong function parameters
 */
static int cfg_get_line(struct cfg_file *pcfg, char *buffer, unsigned len)
{
	char *ptr;

	if (pcfg == NULL || pcfg->pfile == NULL || buffer == NULL || len == 0) {
		return -1;
	}

	do {
		ptr = fgets(buffer, len, pcfg->pfile);
		if (ptr == NULL) {
			return 0; /* end of file */
		}
		++pcfg->line;

		/* remove comments */
		ptr = strchr(buffer, ';');
		if (ptr != NULL) {
			*ptr = '\0';
		}
		else {
			ptr = strchr(buffer, '\0');
		}

		/* remove trailing spaces */
		if (ptr != buffer) {
			ptr--;
			while (isspace(*ptr)) {
				*ptr = '\0';
				ptr--;
			}
		}

		ptr = buffer;
		/* remove leading spaces */
		while (*ptr && isspace(*ptr)) {
			++ptr;
		}
		if (ptr != buffer) {
			strcpy(buffer, ptr);
			ptr = buffer;
		}
	}
	while (*ptr == '\0'); /* skip empty strings */

	return strlen(buffer);
}

/*
 * Checks if buffer contains section name specified by the cfg_section pointer.
 * Returns NULL if section name does not match, cfg_section pointer otherwise
 */
static struct cfg_section *cfg_check_section(char *buffer, struct cfg_section *psec)
{
	char *pend;
	unsigned len;
	static const char *valid = "0123456789,hs- \t";

	pend = strchr(buffer, ']');
	if (pend == NULL) {
		return NULL; /* ']' not found: invalid section name */
	}

	*pend = '\0';

	/* check if section is indexed */
	pend = strchr(psec->name, '#');
	if (pend == NULL) {
		return (strcmp(buffer, psec->name) == 0) ? psec : NULL;
	}

	/* get section index */
	len = pend - psec->name;
	if (strncmp(buffer, psec->name, len) != 0) {
		return NULL;
	}
	pend = buffer + len;
	if (*pend == '\0') {
		return NULL;
	}
	/* only numeric characters are valid for section index
	   (currently, variables not checked!) */
	if (pend[0] != '$') {
		for (len = 0; pend[len] != '\0'; ++len) {
			if (strchr(valid, pend[len]) == NULL) {
				return NULL;
			}
		}
	}

	psec->nbindex = parse_list_set(psec->indexp, pend, MAX_INDEX);
	DPPD_PANIC(psec->nbindex == -1, "\t\tError in cfg_check_section('%s'): %s\n", buffer, get_parse_err());

	for (int i = 0; i < psec->nbindex; ++i) {
		psec->indexp[i] |= CFG_INDEXED;
	}

	return psec;
}

static char *cfg_get_section_name(struct cfg_section *psec)
{
	char *name;

	if (!(psec->indexp[0] & CFG_INDEXED)) {
		return strdup(psec->name);
	}

	name = malloc(strlen(psec->name) + strlen(UINT32_MAX_STR));
	if (name != NULL) {
		strcpy(name, psec->name);
		char *pidx = strchr(name, '#');
		if (pidx != NULL) {
			sprintf(pidx, "%u", psec->indexp[0] & ~CFG_INDEXED);
		}
	}
	return name;
}

/*
 * Reads configuration file and parses section specified by psec pointer.
 * Returns 0 on success, -1 otherwise
 */
int cfg_parse(struct cfg_file *pcfg, struct cfg_section *psec)
{
	int error;
	unsigned entry = 0;
	fpos_t pos;
	int index_count = 0;
	struct cfg_section *section = NULL;
	char buffer[sizeof(pcfg->cur_line)] = {0};

	if (pcfg == NULL || psec == NULL) {
		return -1;
	}

	pcfg->line = 0;
	fseek(pcfg->pfile, 0, SEEK_SET);

	/* read configuration file and parse section specified by psec pointer */
	while (1) {
		while (cfg_get_line(pcfg, buffer, MAX_CFG_STRING_LEN) > 0) {
			strncpy(pcfg->cur_line, buffer, sizeof(pcfg->cur_line));
			if (*buffer == '[') {
				if (index_count + 1 < psec->nbindex) {
					// Need to loop - go back to recorded postion in file
					cfg_set_pos(pcfg, &pos);
					++index_count;
					continue;
				}
				else {
					section = cfg_check_section(buffer + 1, psec);
					entry = 0;
					index_count = 0;
					cfg_get_pos(pcfg, &pos);
					continue;
				}
			}
			/* call parser procedure for each line in the section */
			if (section != NULL) {
				error = section->parser(section->indexp[index_count], buffer, section->data);
				if (error != 0) {
					section->error = error;
					/* log only the very first error */
					if (!pcfg->err_section) {
						pcfg->err_line = pcfg->line;
						pcfg->err_entry = entry;
						pcfg->err_section = cfg_get_section_name(section);
					}
					return 0;
				}
				++entry;
			}
		}
		if (index_count + 1 < psec->nbindex) {
			// Last core config contained multiple cores - loop back
			cfg_set_pos(pcfg, &pos);
			++index_count;
		}
		else {
			break;
		}
	}
	return 0;
}

struct csv_file* csv_open(const char *name, int min_fields, int max_fields)
{
	return csv_open_delim(name, min_fields, max_fields, ',');
}

struct csv_file* csv_open_delim(const char *name, int min_fields, int max_fields, char delim)
{
	struct csv_file* file;

	if (min_fields > max_fields || min_fields <= 0 || name == NULL)
		return NULL;

	file = malloc(sizeof(struct csv_file));
	file->fp = fopen(name, "r");

	if (file->fp == NULL) {
		free(file);
		return NULL;
	}

	file->delim = delim;
	file->line = 0;
	file->min_fields = min_fields;
	file->max_fields = max_fields;
	file->fields = malloc((max_fields + 1) * sizeof(char *));
	file->str = malloc(255);
	return file;
}

int csv_next(struct csv_file *file, char** fields)
{
	char *ret;
	int num_fields, i = 0;
	size_t len;

	do {
		ret = fgets(file->str, 255, file->fp);
		file->line++;
		if (ret == NULL) {
			return 0;
		}
	}
	while (*file->str == '#' || *file->str == '\n');

        num_fields = rte_strsplit(file->str, 255, file->fields, file->max_fields + 1, file->delim);

	if (num_fields > file->max_fields || num_fields < file->min_fields) {
		return -2;
	}

	if (fields != NULL) {
		/* Strip white spaces and copy result fields */
		for (i = 0; i < num_fields; ++i) {
			while (isspace(*file->fields[i])) {
				file->fields[i]++;
			}
			len = strlen(file->fields[i]);

			while (len && isspace(file->fields[i][len - 1])) {
				file->fields[i][len - 1] = 0;
				len--;
			}

			fields[i] = file->fields[i];
		}
	}

	return num_fields;
}

int csv_close(struct csv_file *file)
{
	if (file->fp) {
		fclose(file->fp);
	}
	free(file->fields);
	free(file->str);
	free(file);
	return 0;
}
