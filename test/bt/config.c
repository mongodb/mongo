/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wts.h"

/*
 * Configuration for the wts program is an array of string-based paramters.
 * This is the structure used to declare them.
 */
typedef struct {
	const char	*name;			/* Configuration item */
	u_int32_t	 random;		/* 1/0: if randomized */
	u_int32_t	 min;			/* Minimum value */
	u_int32_t	 max;			/* Maximum value */
	u_int32_t	 *v;			/* Value for this run */
} CONFIG;

/* Get a random value between a config min/max pair. */
#define	CONF_RAND(cp)	MMRAND((cp)->min, (cp)->max)

static CONFIG c[] = {
  { "bulk_load",	1,	0,	M(1),		&g.c_bulk_keys },
  { "cache",		1,	2,	30,		&g.c_cache },
  { "data_len",		0,	0,	0,		&g.c_data_len },
  { "data_max",		1,	32,	4096,		&g.c_data_max },
  { "data_min",		1,	10,	64,		&g.c_data_min },
  { "database_type",	1,	0,	2,		&g.c_database_type },
  { "fixed_length",	1,	1,	24,		&g.c_fixed_length },
  { "huffman_data",	1,	0,	1,		&g.c_huffman_data },
  { "huffman_key",	1,	0,	1,		&g.c_huffman_key },
  { "internal_node",	1,	9,	17,		&g.c_internal_node },
  { "key_cnt",		1,	1000,	M(1),		&g.c_key_cnt },
  { "key_len",		0,	0,	0,		&g.c_key_len },
  { "key_max",		1,	64,	128,		&g.c_key_max },
  { "key_min",		1,	10,	32,		&g.c_key_min },
  { "leaf_node",	1,	9,	17,		&g.c_leaf_node },
  { "rand_seed",	0,	0,	INT_MAX,	&g.c_rand_seed },
  { "read_ops",		0,	0,	100,		&g.c_read_ops },
  { "repeat_comp",	1,	0,	1,		&g.c_repeat_comp },
  { "write_ops",	0,	0,	100,		&g.c_write_ops },
  { NULL, 0, 0, 0, NULL }
};

/*
 * config_names --
 *	Display configuration names.
 */
void
config_names(void)
{
	CONFIG *cp;

	/* Display configuration names. */
	for (cp = c; cp->name != NULL; ++cp)
		printf("%s\n", cp->name);
}

/*
 * config_init --
 *	Initialize configuration structure.
 */
void
config_init(void)
{
	CONFIG *cp;
	u_int i;

	/*
	 * Walk the configuration array and fill in random values for this
	 * run.
	 */
	for (cp = c; cp->name != NULL; ++cp) {
		/* Skip items that aren't randomized. */
		if (cp->random == 0)
			continue;
		*cp->v = CONF_RAND(cp);
	}

	/* Specials. */
	if (g.c_rand_seed == 0)
		g.c_rand_seed = (0xdeadbeef ^ (u_int)time(NULL));
	if (g.c_read_ops == 0)
		g.c_read_ops = (rand() % 100) + 1;
	if (g.c_write_ops == 0)
		g.c_write_ops = 100 - g.c_read_ops;

	/* Fill in the random key lengths. */
	for (i = 0; i < sizeof(g.key_rand_len) / sizeof(g.key_rand_len[0]); ++i)
		g.key_rand_len[i] = MMRAND(g.c_key_min, g.c_key_max);

	/* Reset the key count. */
	g.key_cnt = 0;
}

/*
 * config_dump --
 *	Dump configuration structure.
 */
void
config_dump(int logfile)
{
	CONFIG *cp;
	FILE *fp;
	char *p;

	if (logfile) {
		p = fname(WT_PREFIX, "run");
		if ((fp = fopen(p, "w")) == NULL) {
			fprintf(stderr, "%s: %s\n", p, strerror(errno));
			exit (EXIT_FAILURE);
		}
	} else
		fp = stdout;

	fprintf(fp, "############################################\n");
	fprintf(fp, "#  RUN PARAMETERS\n");
	fprintf(fp, "############################################\n");

	/* Display configuration values. */
	for (cp = c; cp->name != NULL; ++cp)
		fprintf(fp, "%s=%lu\n", cp->name, (u_long)*cp->v);

	fprintf(fp, "############################################\n");
	if (logfile)
		(void)fclose(fp);
}

/*
 * config_file --
 *	Read configuration values from a file.
 */
void
config_file(char *fname)
{
	FILE *fp;
	char *p, buf[256];

	if ((fp = fopen(fname, "r")) == NULL) {
		fprintf(stderr, "%s: %s\n", fname, strerror(errno));
		exit (EXIT_FAILURE);
	}
	while (fgets(buf, sizeof(buf), fp) != NULL) {
		for (p = buf; *p != '\0'; ++p)
			if (!isspace(*p))
				break;
		if (*p == '\0' || *p == '#' || *p == '\n')
			continue;
		config_single(buf);
	}
	(void)fclose(fp);
}

/*
 * config_single --
 *	Set a single configuration structure value.
 */
void
config_single(char *s)
{
	CONFIG *cp;
	char *vp;

	if ((vp = strchr(s, '=')) == NULL) {
		fprintf(stderr,
		    "%s: %s: illegal command-line value\n", g.progname, s);
		exit (EXIT_FAILURE);
	}
	*vp++ = '\0';

	for (cp = c; cp->name != NULL; ++cp) 
		if (strcmp(s, cp->name) == 0) {
			*cp->v = (u_int32_t)atoi(vp);
			if (*cp->v < cp->min || *cp->v > cp->max) {
				fprintf(stderr,
				    "%s: %s: value of %lu outside min/max "
				    "values of %lu-%lu\n",
				    g.progname, s, (u_long)*cp->v,
				    (u_long)cp->min, (u_long)cp->max);
				exit (EXIT_FAILURE);
			}
			cp->random = 0;
			return;
		}
	fprintf(stderr,
	    "%s: %s: unknown configuration value; use the -c option to "
	    "display available configuration values\n",
	    g.progname, s);
	exit (EXIT_FAILURE);
}
