/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wts.h"
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

	/* Seed the random number generator. */
	if (!g.replay)
		srand((u_int)(0xdeadbeef ^ (u_int)time(NULL)));

	/* Clear any temporary values. */
	config_clear();

	/* Pick a file type next, other items depend on it. */
	cp = config_find("file_type");
	if (cp->flags & C_PERM) {
		/*
		 * If a file type was specified and it's fixed, but no repeat-
		 * count was specified, go with RLE 50% of the time.
		 */
		if (g.c_file_type == FIX) {
			cp = config_find("repeat_comp_pct");
			if (!(cp->flags & C_PERM)) {
				(void)snprintf(buf, sizeof(buf),
				    "repeat_comp_pct=%d",
				    MMRAND(0, 1) == 1 ? 0 : MMRAND(10, 90));
				config_single(buf, 0);
			}
		}
	} else
		switch (MMRAND(0, 3)) {
		case 0:
			(void)snprintf(buf, sizeof(buf), "file_type=flcs");
			config_single(buf, 0);
			(void)snprintf(buf, sizeof(buf), "repeat_comp_pct=0");
			config_single(buf, 0);
			break;
		case 1:
			/*
			 * 25% of the time go with RLE, which is fixed-length
			 * and a repeat count.
			 */
			(void)snprintf(buf, sizeof(buf), "file_type=flcs");
			config_single(buf, 0);
			(void)snprintf(buf, sizeof(buf),
			    "repeat_comp_pct=%d", MMRAND(10, 90));
			config_single(buf, 0);
			break;
		case 2:
			(void)snprintf(buf, sizeof(buf), "file_type=vlcs");
			config_single(buf, 0);
			break;
		case 3:
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
 * config_print --
 *	Print configuration information.
 */
void
config_print(int error_display)
{
	CONFIG *cp;
	FILE *fp;
	char *p;

	if (error_display)
		fp = stdout;
	else {
		p = fname("run");
		if ((fp = fopen(p, "w")) == NULL) {
			fprintf(stderr, "%s: %s\n", p, strerror(errno));
			exit(EXIT_FAILURE);
		}
	}

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
			fprintf(fp, "%s=%lu\n", cp->name, (u_long)*cp->v);
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

	if ((fp = fopen(name, "r")) == NULL) {
		fprintf(stderr, "%s: %s\n", name, strerror(errno));
		exit(EXIT_FAILURE);
	}
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
		fprintf(stderr,
		    "%s: %s: value of %lu outside min/max values of %lu-%lu\n",
		    g.progname, s,
		    (u_long)*cp->v, (u_long)cp->min, (u_long)cp->max);
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
	if (strcmp(s, "row") == 0 || strcmp(s, "row-store") == 0)
		return ((uint32_t)ROW);
	if (strcmp(s, "vlcs") == 0 ||
	    strcmp(s, "variable-length column-store") == 0)
		return ((uint32_t)VAR);
	if (strcmp(s, "flcs") == 0 ||
	    strcmp(s, "fixed-length column-store") == 0)
		return ((uint32_t)FIX);

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
		if (g.c_repeat_comp_pct == 0)
			return ("fixed-length column-store");
		else
			return ("fixed-length column-store (RLE)");
	case VAR:
		return ("variable-length column-store");
	case ROW:
		return ("row-store");
	default:
		break;
	}
	return ("error: UNKNOWN FILE TYPE");
}
