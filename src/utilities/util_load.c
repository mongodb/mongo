/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"
#include "util.h"

const char *progname;

int	bulk_callback(DB *, DBT **, DBT **);
int	bulk_read(DBT *dbt, int);
int	config_read(char **);
int	config_read_single(char *);
int	config_set(DB *);
int	usage(void);

struct {
	int pagesize_set;
	uint32_t allocsize, intlmin, intlmax, leafmin, leafmax;
} config;

int
main(int argc, char *argv[])
{
	DB *db;
	int ch, ret, text_input, tret, verbose;
	char **config_list, **next;

	WT_UTILITY_INTRO(progname, argv);

	/*
	 * We can't handle configuration-line information until we've opened
	 * the DB handle, so we need a place to store it for now.
	 */
	if ((config_list = calloc((size_t)argc + 1, sizeof(char *))) == NULL) {
		fprintf(stderr, "%s: %s\n", progname, strerror(errno));
		return (EXIT_FAILURE);
	}
	next = config_list;

	text_input = verbose = 0;
	while ((ch = getopt(argc, argv, "c:f:TVv")) != EOF)
		switch (ch) {
		case 'c':			/* command-line option */
			*next++ = optarg;
			break;
		case 'f':			/* input file */
			if (freopen(optarg, "r", stdin) == NULL) {
				fprintf(stderr, "%s: %s: reopen: %s\n",
				    progname, optarg, strerror(errno));
				return (EXIT_FAILURE);
			}
			break;
		case 'T':
			text_input = 1;
			break;
		case 'V':			/* version */
			printf("%s\n", wiredtiger_version(NULL, NULL, NULL));
			return (EXIT_SUCCESS);
		case 'v':
			verbose = 1;
			break;
		case '?':
		default:
			return (usage());
		}
	argc -= optind;
	argv += optind;

	/* The remaining argument is the file name. */
	if (argc != 1)
		return (usage());

	/*
	 * Read through the command-line configuration options and convert
	 * to the config structure.
	 */
	if (config_read(config_list) != 0)
		goto err;

	/*
	 * Right now, we only support text input -- require the T option to
	 * match Berkeley DB's API.
	 */
	if (text_input == 0) {
		fprintf(stderr,
		    "%s: the -T option is currently required\n", progname);
		return (EXIT_FAILURE);
	}

	if ((ret = wiredtiger_simple_setup(progname, &db, 0, 0)) == 0) {
		if (config_set(db) != 0)
			goto err;

		(void)remove(*argv);

		if ((ret = db->open(db, *argv, 0600, WT_CREATE)) != 0) {
			db->err(db, ret, "Db.open: %s", *argv);
			goto err;
		}

		if ((ret = db->bulk_load(db,
		    verbose ? __wt_progress : NULL, bulk_callback)) != 0) {
			db->err(db, ret, "Db.bulk_load");
			goto err;
		}
		if (verbose)
			printf("\n");
	}

	if (0) {
err:		ret = 1;
	}
	if ((tret = wiredtiger_simple_teardown(progname, db)) != 0 && ret == 0)
		ret = tret;
	return (ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}

/*
 * config_read --
 *	Convert command-line options into the config structure.
 */
int
config_read(char **list)
{
	int ret;

	for (; *list != NULL; ++list)
		if ((ret = config_read_single(*list)) != 0)
			return (ret);
	return (0);
}

/*
 * config_read_single --
 *	Process a single command-line configuration option, converting it into
 *	the config structure.
 */
int
config_read_single(char *opt)
{
	u_long v;
	char *p, *ep;

	/* Get pointers to the two parts of an X=Y format string. */
	if ((p = strchr(opt, '=')) == NULL || p[1] == '\0')
		goto format;
	*p++ = '\0';
	v = strtoul(p, &ep, 10);
	if (v == ULONG_MAX && errno == ERANGE) {
format:		fprintf(stderr,
		    "%s: -c option %s is not correctly formatted\n",
		    progname, opt);
		return (1);
	}
	if (strcmp(opt, "allocsize") == 0) {
		config.allocsize = (uint32_t)v;
		config.pagesize_set = 1;
		return (0);
	}
	if (strcmp(opt, "intlmin") == 0) {
		config.intlmin = (uint32_t)v;
		config.pagesize_set = 1;
		return (0);
	}
	if (strcmp(opt, "intlmax") == 0) {
		config.intlmax = (uint32_t)v;
		config.pagesize_set = 1;
		return (0);
	}
	if (strcmp(opt, "leafmin") == 0) {
		config.leafmin = (uint32_t)v;
		config.pagesize_set = 1;
		return (0);
	}
	if (strcmp(opt, "leafmax") == 0) {
		config.leafmax = (uint32_t)v;
		config.pagesize_set = 1;
		return (0);
	}

	fprintf(stderr,
	    "%s: -c option %s has an unknown keyword\n", progname, opt);
	return (1);
}

/*
 * config_set --
 *	Set the command-line configuration options on the DB handle.
 */
int
config_set(DB *db)
{
	u_int32_t allocsize, intlmin, intlmax, leafmin, leafmax;
	int ret;

	if (config.pagesize_set) {
		if ((ret = db->btree_pagesize_get(db,
		    &allocsize, &intlmin, &intlmax, &leafmin, &leafmax)) != 0) {
			db->err(db, ret, "Db.btree_pagesize_get");
			return (1);
		}
		if (config.allocsize != 0)
			allocsize = config.allocsize;
		if (config.intlmin != 0)
			intlmin = config.intlmin;
		if (config.intlmax != 0)
			intlmax = config.intlmax;
		if (config.leafmin != 0)
			leafmin = config.leafmin;
		if (config.leafmax != 0)
			leafmax = config.leafmax;
		if ((ret = db->btree_pagesize_set(db,
		    allocsize, intlmin, intlmax, leafmin, leafmax)) != 0) {
			db->err(db, ret, "Db.btree_pagesize_set");
			return (1);
		}
	}

	return (0);
}

/*
 * bulk_read --
 *	Read a line from stdin into a DBT.
 */
int
bulk_read(DBT *dbt, int iskey)
{
	static unsigned long long line = 0;
	uint32_t len;
	int ch;

	++line;
	for (len = 0;; ++len) {
		if ((ch = getchar()) == EOF) {
			if (iskey && len == 0)
				return (1);
			fprintf(stderr, "%s: corrupted input at line %llu\n",
			    progname, line);
			return (WT_ERROR);
		}
		if (ch == '\n')
			break;
		if (len >= dbt->mem_size) {
			if ((dbt->data = realloc(dbt->data, len + 128)) == NULL)
				return (errno);
			dbt->mem_size = len + 128;
		}
		((u_int8_t *)(dbt->data))[len] = (u_int8_t)ch;
	}
	dbt->size = len;
	return (0);
}

/*
 * bulk_callback --
 *	Bulk-load callback function.
 */
int
bulk_callback(DB *db, DBT **keyp, DBT **datap)
{
	static DBT key, data;
	int ret;

	WT_UNUSED(db);

	if ((ret = bulk_read(&key, 1)) != 0)
		return (ret);
	if ((ret = bulk_read(&data, 0)) != 0)
		return (ret);

	*keyp = &key;
	*datap = &data;
	return (0);
}

int
usage(void)
{
	(void)fprintf(stderr,
	    "usage: %s [-TVv] [-c configuration] [-f input-file] file\n",
	    progname);
	return (EXIT_FAILURE);
}
