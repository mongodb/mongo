/*-
 * Public Domain 2014-2015 MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "format.h"
#include "config.h"

static void	   config_checksum(void);
static void	   config_compression(void);
static const char *config_file_type(u_int);
static CONFIG	  *config_find(const char *, size_t);
static int	   config_find_is_perm(const char *, size_t);
static void	   config_isolation(void);
static void	   config_map_checksum(const char *, u_int *);
static void	   config_map_compression(const char *, u_int *);
static void	   config_map_file_type(const char *, u_int *);
static void	   config_map_isolation(const char *, u_int *);

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

	/*
	 * Choose a data source type and a file type: they're interrelated (LSM
	 * trees are only compatible with row-store) and other items depend on
	 * them.
	 */
	if (!config_find_is_perm("data_source", strlen("data_source")))
		switch (MMRAND(1, 3)) {
		case 1:
			config_single("data_source=file", 0);
			break;
		case 2:
			config_single("data_source=lsm", 0);
			break;
		case 3:
			config_single("data_source=table", 0);
			break;
		}

	if (!config_find_is_perm("file_type", strlen("file_type")))
		switch (DATASOURCE("lsm") ? 5 : MMRAND(1, 10)) {
		case 1:
			config_single("file_type=fix", 0);
			break;
		case 2: case 3: case 4:
			config_single("file_type=var", 0);
			break;
		case 5: case 6: case 7: case 8: case 9: case 10:
			config_single("file_type=row", 0);
			break;
		}
	config_map_file_type(g.c_file_type, &g.type);

	/*
	 * If data_source and file_type were both "permanent", we may still
	 * have a mismatch.
	 */
	if (DATASOURCE("lsm") && g.type != ROW) {
		fprintf(stderr,
	    "%s: lsm data_source is only compatible with row file_type\n",
		    g.progname);
		exit(EXIT_FAILURE);
	}

	/*
	 * Build the top-level object name: we're overloading data_source in
	 * our configuration, LSM or KVS devices are "tables", but files are
	 * tested as well.
	 */
	if ((g.uri = malloc(256)) == NULL)
		die(errno, "malloc");
	strcpy(g.uri, DATASOURCE("file") ? "file:" : "table:");
	if (DATASOURCE("helium"))
		strcat(g.uri, "dev1/");
	strcat(g.uri, WT_NAME);

	/* Default single-threaded 10% of the time. */
	cp = config_find("threads", strlen("threads"));
	if (!(cp->flags & C_PERM))
		*cp->v = MMRAND(1, 100) < 10 ? 1: CONF_RAND(cp);

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

	/* Required shared libraries. */
	if (DATASOURCE("helium") && access(HELIUM_PATH, R_OK) != 0)
		die(errno, "Levyx/helium shared library: %s", HELIUM_PATH);
	if (DATASOURCE("kvsbdb") && access(KVS_BDB_PATH, R_OK) != 0)
		die(errno, "kvsbdb shared library: %s", KVS_BDB_PATH);

	/* Some data-sources don't support user-specified collations. */
	if (DATASOURCE("helium") || DATASOURCE("kvsbdb"))
		g.c_reverse = 0;

	config_checksum();
	config_compression();
	config_isolation();

	/*
	 * Periodically, set the delete percentage to 0 so salvage gets run,
	 * as long as the delete percentage isn't nailed down.
	 */
	if (!g.replay && g.run_cnt % 10 == 0) {
		cp = config_find("delete_pct", strlen("delete_pct"));
		if (cp->name != NULL &&
		    !(cp->flags & (C_IGNORE | C_PERM | C_TEMP)))
			g.c_delete_pct = 0;
	}

	/*
	 * If this is an LSM run, set the cache size and crank up the insert
	 * percentage.
	 */
	if (DATASOURCE("lsm")) {
		cp = config_find("cache", strlen("cache"));
		if (!(cp->flags & C_PERM))
			g.c_cache = 30 * g.c_chunk_size;

		cp = config_find("insert_pct", strlen("insert_pct"));
		if (cp->name != NULL &&
		    !(cp->flags & (C_IGNORE | C_PERM | C_TEMP)))
			g.c_insert_pct = MMRAND(50, 85);
	}

	/* Make the default maximum-run length 20 minutes. */
	cp = config_find("timer", strlen("timer"));
	if (!(cp->flags & C_PERM))
		g.c_timer = 20;

	/*
	 * Key/value minimum/maximum are related, correct unless specified by
	 * the configuration.
	 */
	cp = config_find("key_min", strlen("key_min"));
	if (!(cp->flags & C_PERM) && g.c_key_min > g.c_key_max)
		g.c_key_min = g.c_key_max;
	cp = config_find("key_max", strlen("key_max"));
	if (!(cp->flags & C_PERM) && g.c_key_max < g.c_key_min)
		g.c_key_max = g.c_key_min;
	if (g.c_key_min > g.c_key_max)
		die(EINVAL, "key_min may not be larger than key_max");

	cp = config_find("value_min", strlen("value_min"));
	if (!(cp->flags & C_PERM) && g.c_value_min > g.c_value_max)
		g.c_value_min = g.c_value_max;
	cp = config_find("value_max", strlen("value_max"));
	if (!(cp->flags & C_PERM) && g.c_value_max < g.c_value_min)
		g.c_value_max = g.c_value_min;
	if (g.c_value_min > g.c_value_max)
		die(EINVAL, "value_min may not be larger than value_max");

	/* Reset the key count. */
	g.key_cnt = 0;
}

/*
 * config_checksum --
 *	Checksum configuration.
 */
static void
config_checksum(void)
{
	CONFIG *cp;

	/* Choose a checksum mode if nothing was specified. */
	cp = config_find("checksum", strlen("checksum"));
	if (!(cp->flags & C_PERM))
		switch (MMRAND(1, 10)) {
		case 1:					/* 10% */
			config_single("checksum=on", 0);
			break;
		case 2:					/* 10% */
			config_single("checksum=off", 0);
			break;
		default:				/* 80% */
			config_single("checksum=uncompressed", 0);
			break;
		}
}

/*
 * config_compression --
 *	Compression configuration.
 */
static void
config_compression(void)
{
	CONFIG *cp;
	const char *cstr;

	/*
	 * Compression: choose something if compression wasn't specified,
	 * otherwise confirm the appropriate shared library is available.
	 * We used to verify that the libraries existed but that's no longer
	 * robust, since it's possible to build compression libraries into
	 * the WiredTiger library.
	 */
	cp = config_find("compression", strlen("compression"));
	if (!(cp->flags & C_PERM)) {
		cstr = "compression=none";
		switch (MMRAND(1, 20)) {
		case 1: case 2: case 3:			/* 30% no compression */
		case 4: case 5: case 6:
			break;
		case 7: case 8: case 9: case 10:	/* 20% bzip */
			cstr = "compression=bzip";
			break;
		case 11:				/* 5% bzip-raw */
			cstr = "compression=bzip-raw";
			break;
		case 12: case 13: case 14: case 15:	/* 20% snappy */
			cstr = "compression=snappy";
			break;
		case 16: case 17: case 18: case 19:	/* 20% zlib */
			cstr = "compression=zlib";
			break;
		case 20:				/* 5% zlib-no-raw */
			cstr = "compression=zlib-noraw";
			break;
		}

		config_single(cstr, 0);
	}
}

/*
 * config_isolation --
 *	Isolation configuration.
 */
static void
config_isolation(void)
{
	CONFIG *cp;
	const char *cstr;

	/*
	 * Isolation: choose something if isolation wasn't specified.
	 */
	cp = config_find("isolation", strlen("isolation"));
	if (!(cp->flags & C_PERM)) {
		/* Avoid "maybe uninitialized" warnings. */
		switch (MMRAND(1, 4)) {
		case 1:
			cstr = "isolation=random";
			break;
		case 2:
			cstr = "isolation=read-uncommitted";
			break;
		case 3:
			cstr = "isolation=read-committed";
			break;
		case 4:
		default:
			cstr = "isolation=snapshot";
			break;
		}
		config_single(cstr, 0);
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

	/* Display configuration names. */
	fprintf(stderr, "\n");
	fprintf(stderr, "Configuration names:\n");
	for (cp = c; cp->name != NULL; ++cp)
		if (strlen(cp->name) > 17)
			fprintf(stderr,
			    "%s\n%17s: %s\n", cp->name, " ", cp->desc);
		else
			fprintf(stderr, "%17s: %s\n", cp->name, cp->desc);
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
		if ((fp = fopen(g.home_config, "w")) == NULL)
			die(errno, "fopen: %s", g.home_config);

	fprintf(fp, "############################################\n");
	fprintf(fp, "#  RUN PARAMETERS\n");
	fprintf(fp, "############################################\n");

	/* Display configuration values. */
	for (cp = c; cp->name != NULL; ++cp)
		if (cp->flags & C_STRING)
			fprintf(fp, "%s=%s\n", cp->name,
			    *cp->vstr == NULL ? "" : *cp->vstr);
		else
			fprintf(fp, "%s=%" PRIu32 "\n", cp->name, *cp->v);

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
void
config_clear(void)
{
	CONFIG *cp;

	/* Clear configuration data. */
	for (cp = c; cp->name != NULL; ++cp) {
		cp->flags &= ~(uint32_t)C_TEMP;
		if (!(cp->flags & C_PERM) &&
		    cp->flags & C_STRING && cp->vstr != NULL) {
			free(*cp->vstr);
			*cp->vstr = NULL;
		}
	}
	free(g.uri);
	g.uri = NULL;
}

/*
 * config_single --
 *	Set a single configuration structure value.
 */
void
config_single(const char *s, int perm)
{
	CONFIG *cp;
	u_long v;
	char *p;
	const char *ep;

	if ((ep = strchr(s, '=')) == NULL) {
		fprintf(stderr,
		    "%s: %s: illegal configuration value\n", g.progname, s);
		exit(EXIT_FAILURE);
	}

	cp = config_find(s, (size_t)(ep - s));
	cp->flags |= perm ? C_PERM : C_TEMP;
	++ep;

	if (cp->flags & C_STRING) {
		if (strncmp(s, "data_source", strlen("data_source")) == 0 &&
		    strncmp("file", ep, strlen("file")) != 0 &&
		    strncmp("helium", ep, strlen("helium")) != 0 &&
		    strncmp("kvsbdb", ep, strlen("kvsbdb")) != 0 &&
		    strncmp("lsm", ep, strlen("lsm")) != 0 &&
		    strncmp("table", ep, strlen("table")) != 0) {
			    fprintf(stderr,
				"Invalid data source option: %s\n", ep);
			    exit(EXIT_FAILURE);
		}

		if (strncmp(s, "checksum", strlen("checksum")) == 0) {
			config_map_checksum(ep, &g.c_checksum_flag);
			*cp->vstr = strdup(ep);
		} else if (strncmp(
		    s, "compression", strlen("compression")) == 0) {
			config_map_compression(ep, &g.c_compression_flag);
			*cp->vstr = strdup(ep);
		} else if (strncmp(s, "isolation", strlen("isolation")) == 0) {
			config_map_isolation(ep, &g.c_isolation_flag);
			*cp->vstr = strdup(ep);
		} else if (strncmp(s, "file_type", strlen("file_type")) == 0) {
			config_map_file_type(ep, &g.type);
			*cp->vstr = strdup(config_file_type(g.type));
		} else {
			if (*cp->vstr != NULL)
				free(*cp->vstr);
			*cp->vstr = strdup(ep);
		}
		if (*cp->vstr == NULL)
			die(errno, "malloc");

		return;
	}

	v = strtoul(ep, &p, 10);
	if (*p != '\0') {
		fprintf(stderr, "%s: %s: illegal numeric value\n",
		    g.progname, s);
		exit(EXIT_FAILURE);
	}
	if (cp->flags & C_BOOL) {
		if (v != 0 && v != 1) {
			fprintf(stderr, "%s: %s: value of boolean not 0 or 1\n",
			    g.progname, s);
			exit(EXIT_FAILURE);
		}
	} else if ((uint32_t)v < cp->min || (uint32_t)v > cp->maxset) {
		fprintf(stderr, "%s: %s: value of %" PRIu32
		    " outside min/max values of %" PRIu32 "-%" PRIu32 "\n",
		    g.progname, s, *cp->v, cp->min, cp->maxset);
		exit(EXIT_FAILURE);
	}
	*cp->v = (uint32_t)v;
}

/*
 * config_map_file_type --
 *	Map a file type configuration to a flag.
 */
static void
config_map_file_type(const char *s, u_int *vp)
{
	if (strcmp(s, "fix") == 0 ||
	    strcmp(s, "fixed-length column-store") == 0)
		*vp = FIX;
	else if (strcmp(s, "var") == 0 ||
	    strcmp(s, "variable-length column-store") == 0)
		*vp = VAR;
	else if (strcmp(s, "row") == 0 ||
	    strcmp(s, "row-store") == 0)
		*vp = ROW;
	else
		die(EINVAL, "illegal file type configuration: %s", s);
}

/*
 * config_map_checksum --
 *	Map a checksum configuration to a flag.
 */
static void
config_map_checksum(const char *s, u_int *vp)
{
	if (strcmp(s, "on") == 0)
		*vp = CHECKSUM_ON;
	else if (strcmp(s, "off") == 0)
		*vp = CHECKSUM_ON;
	else if (strcmp(s, "uncompressed") == 0)
		*vp = CHECKSUM_UNCOMPRESSED;
	else
		die(EINVAL, "illegal checksum configuration: %s", s);
}

/*
 * config_map_compression --
 *	Map a compression configuration to a flag.
 */
static void
config_map_compression(const char *s, u_int *vp)
{
	if (strcmp(s, "none") == 0)
		*vp = COMPRESS_NONE;
	else if (strcmp(s, "bzip") == 0)
		*vp = COMPRESS_BZIP;
	else if (strcmp(s, "bzip-raw") == 0)
		*vp = COMPRESS_BZIP_RAW;
	else if (strcmp(s, "lzo") == 0)
		*vp = COMPRESS_LZO;
	else if (strcmp(s, "snappy") == 0)
		*vp = COMPRESS_SNAPPY;
	else if (strcmp(s, "zlib") == 0)
		*vp = COMPRESS_ZLIB;
	else if (strcmp(s, "zlib-noraw") == 0)
		*vp = COMPRESS_ZLIB_NO_RAW;
	else
		die(EINVAL, "illegal compression configuration: %s", s);
}

/*
 * config_map_isolation --
 *	Map an isolation configuration to a flag.
 */
static void
config_map_isolation(const char *s, u_int *vp)
{
	if (strcmp(s, "random") == 0)
		*vp = ISOLATION_RANDOM;
	else if (strcmp(s, "read-uncommitted") == 0)
		*vp = ISOLATION_READ_UNCOMMITTED;
	else if (strcmp(s, "read-committed") == 0)
		*vp = ISOLATION_READ_COMMITTED;
	else if (strcmp(s, "snapshot") == 0)
		*vp = ISOLATION_SNAPSHOT;
	else
		die(EINVAL, "illegal isolation configuration: %s", s);
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
		if (strncmp(s, cp->name, len) == 0 && cp->name[len] == '\0')
			return (cp);

	fprintf(stderr,
	    "%s: %s: unknown configuration keyword\n", g.progname, s);
	config_error();
	exit(EXIT_FAILURE);
}

/*
 * config_find_is_perm
 *	Return if a specific configuration entry was permanently set.
 */
static int
config_find_is_perm(const char *s, size_t len)
{
	CONFIG *cp;

	cp = config_find(s, len);
	return (cp->flags & C_PERM ? 1 : 0);
}

/*
 * config_file_type --
 *	Return the file type as a string.
 */
static const char *
config_file_type(u_int type)
{
	switch (type) {
	case FIX:
		return ("fixed-length column-store");
	case VAR:
		return ("variable-length column-store");
	case ROW:
		return ("row-store");
	default:
		break;
	}
	return ("error: unknown file type");
}
