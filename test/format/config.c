/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "format.h"
#include "config.h"

static void	   config_clear(void);
static CONFIG	  *config_find(const char *, size_t);
static uint32_t	   config_translate(const char *);

/*
 * config_setup --
 *	Initialize configuration for a run.
 */
void
config_setup(void)
{
	CONFIG *cp;

	/* Clear any temporary values. */
	config_clear();

	/* Pick a file type next, other items depend on it. */
	cp = config_find("file_type", strlen("file_type"));
	if (!(cp->flags & C_PERM)) {
		switch (MMRAND(0, 2)) {
		case 0:
			config_single("file_type=fix", 0);
			break;
		case 1:
			config_single("file_type=var", 0);
			break;
		case 2:
			config_single("file_type=row", 0);
			break;
		}
	}

	/* Reset the key count. */
	g.key_cnt = 0;

	/* Default single-threaded half of the time. */
	cp = config_find("threads", strlen("threads"));
	if (!(cp->flags & C_PERM))
		*cp->v = MMRAND(0, 1) ? 1: CONF_RAND(cp);

	/* Fill in random values for the rest of the run. */
	for (cp = c; cp->name != NULL; ++cp) {
		if (cp->flags & (C_IGNORE | C_PERM | C_TEMP))
			continue;

		/*
		 * Boolean flags are 0 or 1, but only set N in 100 where the
		 * variable's min value is N.  Set the flag if we rolled >=
		 * the min, 0 otherwise.
		 */
		if (cp->flags & C_BOOL)
			*cp->v = MMRAND(1, 100) <= cp->min ? 1 : 0;
		else
			*cp->v = CONF_RAND(cp);
	}

	/* Clear operations values if the whole run is read-only. */
	if (g.c_ops == 0)
		for (cp = c; cp->name != NULL; ++cp)
			if (cp->flags & C_OPS)
				*cp->v = 0;

	/* Multi-threaded runs cannot be replayed. */
	if (g.replay && g.c_threads != 1) {
		fprintf(stderr,
		    "%s: -r is incompatible with threaded runs\n", g.progname);
		exit(EXIT_FAILURE);
	}

	/*
	 * Periodically, set the delete percentage to 0 so salvage gets run,
	 * as long as the delete percentage isn't nailed down.
	 */
	if (!g.replay && g.run_cnt % 10 == 0) {
		for (cp = c; cp->name != NULL; ++cp)
			if (strcmp(cp->name, "delete_pct") == 0)
				break;
		if (cp->name != NULL &&
		    !(cp->flags & (C_IGNORE | C_PERM | C_TEMP)))
			g.c_delete_pct = 0;
	}
}

/*
 * config_error --
 *	Display configuration information on error.
 */
void
config_error(void)
{
	CONFIG *cp;
	FILE *fp;

	/* Display configuration names. */
	fprintf(stderr, "Configuration names:\n");
	for (cp = c; cp->name != NULL; ++cp)
		fprintf(stderr, "%16s : %s\n", cp->name, cp->desc);

	fprintf(stderr, "\n");
	if ((fp = fopen("CONFIG.example", "w")) != NULL) {
		fprintf(stderr, "Re-creating CONFIG.example file... ");
		for (cp = c; cp->name != NULL; ++cp)
			fprintf(fp, "#%s\n#\t%s\n\n", cp->name, cp->desc);
		(void)fclose(fp);
		fprintf(stderr, "done\n");
	}
}

/*
 * config_print --
 *	Print configuration information.
 */
void
config_print(int error_display)
{
	CONFIG *cp;
	FILE *fp;

	if (error_display)
		fp = stdout;
	else
		if ((fp = fopen("__run", "w")) == NULL)
			die(errno, "fopen: __run");

	fprintf(fp, "############################################\n");
	fprintf(fp, "#  RUN PARAMETERS\n");
	fprintf(fp, "############################################\n");

	/* Display configuration values. */
	for (cp = c; cp->name != NULL; ++cp)
		if (cp->type_mask != 0 &&
		    ((g.c_file_type == FIX && !(cp->type_mask & C_FIX)) ||
		    (g.c_file_type == ROW && !(cp->type_mask & C_ROW)) ||
		    (g.c_file_type == VAR && !(cp->type_mask & C_VAR))))
			fprintf(fp,
			    "# %s not applicable to this run\n", cp->name);
		else if (strcmp(cp->name, "file_type"))
			fprintf(fp, "%s=%" PRIu32 "\n", cp->name, *cp->v);
		else
			fprintf(fp, "%s=%s\n", cp->name, config_dtype());

	fprintf(fp, "############################################\n");
	if (fp != stdout)
		(void)fclose(fp);
}

/*
 * config_file --
 *	Read configuration values from a file.
 */
void
config_file(const char *name)
{
	FILE *fp;
	char *p, buf[256];

	if ((fp = fopen(name, "r")) == NULL)
		die(errno, "fopen: %s", name);
	while (fgets(buf, sizeof(buf), fp) != NULL) {
		for (p = buf; *p != '\0' && *p != '\n'; ++p)
			;
		*p = '\0';
		if (buf[0] == '\0' || buf[0] == '#')
			continue;
		config_single(buf, 1);
	}
	(void)fclose(fp);
}

/*
 * config_clear --
 *	Clear per-run values.
 */
static void
config_clear(void)
{
	CONFIG *cp;

	/* Display configuration names. */
	for (cp = c; cp->name != NULL; ++cp)
		cp->flags &= ~(uint32_t)C_TEMP;
}

/*
 * config_single --
 *	Set a single configuration structure value.
 */
void
config_single(const char *s, int perm)
{
	CONFIG *cp;
	const char *ep;

	if ((ep = strchr(s, '=')) == NULL) {
		fprintf(stderr,
		    "%s: %s: illegal configuration value\n", g.progname, s);
		exit(EXIT_FAILURE);
	}

	cp = config_find(s, (size_t)(ep - s));
	cp->flags |= perm ? C_PERM : C_TEMP;

	*cp->v = config_translate(ep + 1);
	if (cp->flags & C_BOOL) {
		if (*cp->v != 0 && *cp->v != 1) {
			fprintf(stderr, "%s: %s: value of boolean not 0 or 1\n",
			    g.progname, s);
			exit(EXIT_FAILURE);
		}
	} else if (*cp->v < cp->min || *cp->v > cp->max) {
		fprintf(stderr, "%s: %s: value of %" PRIu32
		    " outside min/max values of %" PRIu32 "-%" PRIu32 "\n",
		    g.progname, s, *cp->v, cp->min, cp->max);
		exit(EXIT_FAILURE);
	}
}

/*
 * config_translate --
 *	Return an integer value representing the argument.
 */
static uint32_t
config_translate(const char *s)
{
	/* If it's already a integer value, we're done. */
	if (isdigit(s[0]))
		return (uint32_t)atoi(s);

	/* Currently, all we translate are the file type names. */
	if (strcmp(s, "fix") == 0 ||
	    strcmp(s, "flcs") == 0 ||		/* Deprecated */
	    strcmp(s, "fixed-length column-store") == 0)
		return ((uint32_t)FIX);
	if (strcmp(s, "var") == 0 ||
	    strcmp(s, "vlcs") == 0 ||		/* Deprecated */
	    strcmp(s, "variable-length column-store") == 0)
		return ((uint32_t)VAR);
	if (strcmp(s, "row") == 0 ||
	    strcmp(s, "row-store") == 0)
		return ((uint32_t)ROW);

	fprintf(stderr, "%s: %s: unknown configuration value\n", g.progname, s);
	exit(EXIT_FAILURE);
}

/*
 * config_find
 *	Find a specific configuration entry.
 */
static CONFIG *
config_find(const char *s, size_t len)
{
	CONFIG *cp;

	for (cp = c; cp->name != NULL; ++cp) 
		if (strncmp(s, cp->name, len) == 0)
			return (cp);

	fprintf(stderr,
	    "%s: %s: unknown configuration keyword\n", g.progname, s);
	config_error();
	exit(EXIT_FAILURE);
}

/*
 * config_dtype --
 *	Return the file type as a string.
 */
const char *
config_dtype(void)
{
	switch (g.c_file_type) {
	case FIX:
		return ("fixed-length column-store");
	case VAR:
		return ("variable-length column-store");
	case ROW:
		return ("row-store");
	default:
		break;
	}
	return ("error: UNKNOWN FILE TYPE");
}
