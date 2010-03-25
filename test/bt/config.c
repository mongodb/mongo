/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wts.h"
#include "config.h"

static const char *config_dtype(void);
static CONFIG *config_find(const char *);

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
 * config --
 *	Initialize the system.
 */
void
config(void)
{
	CONFIG *cp;

	/* Clean up from any previous runs. */
	(void)system("rm -f __bdb* __wt*");

	/* Pick a random number seed. */
	cp = config_find("rand_seed");
	if (!(cp->flags & C_FIXED))
		*cp->v = (0xdeadbeef ^ (u_int)time(NULL));
	srand((int)g.c_rand_seed);

	/* Pick a database type next, other items depend on it. */
	cp = config_find("database_type");
	if (!(cp->flags & C_FIXED))
		switch (MMRAND(0, 2)) {
		case 0:
			g.c_database_type = FIX;
			break;
		case 1:
			g.c_database_type = VAR;
			break;
		case 2:
			g.c_database_type = ROW;
			break;
		}

	/* Reset the key count. */
	g.key_cnt = 0;

	/* Fill in random values for the rest of the run. */
	for (cp = c; cp->name != NULL; ++cp)
		if (!(cp->flags & (C_FIXED | C_IGNORE)))
			*cp->v = CONF_RAND(cp);

	/* Specials. */
	cp = config_find("write_ops");
	if (!(cp->flags & (C_FIXED | C_IGNORE)))
		g.c_write_ops = 100 - g.c_read_ops;
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
		if (cp->type_mask != 0 &&
		    ((g.c_database_type == FIX && !(cp->type_mask & C_FIX)) ||
		    (g.c_database_type == ROW && !(cp->type_mask & C_ROW)) ||
		    (g.c_database_type == VAR && !(cp->type_mask & C_VAR))))
			fprintf(fp,
			    "# %s not applicable to this run\n", cp->name);
		else {
			if (!strcmp(cp->name, "database_type"))
				fprintf(fp,
				    "# database type: %s\n", config_dtype());
			fprintf(fp, "%s=%lu\n", cp->name, (u_long)*cp->v);
		}

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

	cp = config_find(s);
	cp->flags |= C_FIXED;
	*cp->v = (u_int32_t)atoi(vp);
	if (*cp->v < cp->min || *cp->v > cp->max) {
		fprintf(stderr,
		    "%s: %s: value of %lu outside min/max values of %lu-%lu\n",
		    g.progname, s,
		    (u_long)*cp->v, (u_long)cp->min, (u_long)cp->max);
		exit (EXIT_FAILURE);
	}
}

/*
 * config_find
 *	Find a specific configuration entry.
 */
static CONFIG *
config_find(const char *s)
{
	CONFIG *cp;

	for (cp = c; cp->name != NULL; ++cp) 
		if (strcmp(s, cp->name) == 0)
			return (cp);

	fprintf(stderr,
	    "%s: %s: unknown configuration value; use the -c option to "
	    "display available configuration values\n",
	    g.progname, s);
	exit (EXIT_FAILURE);
}

/*
 * config_dtype --
 *	Return the database type as a string.
 */
static const char *
config_dtype()
{
	switch (g.c_database_type) {
	case FIX:
		return ("fixed-length column store");
	case VAR:
		return ("variable-length column store");
	case ROW:
		return ("row store");
	}
	return ("error: UNKNOWN DATABASE TYPE");
}
