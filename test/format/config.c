/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "format.h"
#include "config.h"

static void	   config_clear(void);
static CONFIG	  *config_find(const char *);
static uint32_t	   config_translate(char *);

/*
 * config_setup --
 *	Initialize configuration for a run.
 */
void
config_setup(void)
{
	CONFIG *cp;
	char buf[64];

	/* Clear any temporary values. */
	config_clear();

	/* Pick a file type next, other items depend on it. */
	cp = config_find("file_type");
	if (!(cp->flags & C_PERM))
		switch (MMRAND(0, 2)) {
		case 0:
#if 0
			(void)snprintf(buf, sizeof(buf), "file_type=flcs");
			config_single(buf, 0);
			break;
#else
			/* FALLTHROUGH */
#endif
		case 1:
			(void)snprintf(buf, sizeof(buf), "file_type=vlcs");
			config_single(buf, 0);
			break;
		case 2:
			(void)snprintf(buf, sizeof(buf), "file_type=row");
			config_single(buf, 0);
			break;
		}

	/* Reset the key count. */
	g.key_cnt = 0;

	/* Fill in random values for the rest of the run. */
	for (cp = c; cp->name != NULL; ++cp) {
		if (cp->flags & (C_IGNORE | C_PERM | C_TEMP))
			continue;

		*cp->v = CONF_RAND(cp);
		/*
		 * Boolean flags are 0 or 1, but only set 1 in N where the
		 * variable's min/max values are 1 and N.  Set the flag if
		 * we rolled a 1, otherwise don't.
		 */
		if (cp->flags & C_BOOL)
			*cp->v = *cp->v == 1 ? 1 : 0;
	}

	/* Periodically, set the delete percentage to 0 so salvage gets run. */
	if (!g.replay && g.run_cnt % 10 == 0)
		g.c_delete_pct = 0;

	/*
	 * There are a couple of corrections the table doesn't handle, where
	 * initialized values are relative to each other.
	 */
	if (g.c_intl_node_max < g.c_intl_node_min)
		g.c_intl_node_max = g.c_intl_node_min;
	if (g.c_leaf_node_max < g.c_leaf_node_min)
		g.c_leaf_node_max = g.c_leaf_node_min;
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
			die("__run", errno);

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
		else {
			if (!strcmp(cp->name, "file_type"))
				fprintf(fp,
				    "# file type: %s\n", config_dtype());
			fprintf(fp, "%s=%" PRIu32 "\n", cp->name, *cp->v);
		}

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
		die(name, errno);
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
config_single(char *s, int perm)
{
	CONFIG *cp;
	char *vp;

	if ((vp = strchr(s, '=')) == NULL) {
		fprintf(stderr,
		    "%s: %s: illegal command-line value\n", g.progname, s);
		exit(EXIT_FAILURE);
	}
	*vp++ = '\0';

	cp = config_find(s);
	cp->flags |= perm ? C_PERM : C_TEMP;

	*cp->v = config_translate(vp);
	if (*cp->v < cp->min || *cp->v > cp->max) {
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
config_translate(char *s)
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
config_find(const char *s)
{
	CONFIG *cp;

	for (cp = c; cp->name != NULL; ++cp) 
		if (strcmp(s, cp->name) == 0)
			return (cp);

	fprintf(stderr,
	    "%s: %s: unknown configuration keyword; use the -c option to "
	    "display available configuration keywords\n",
	    g.progname, s);
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
