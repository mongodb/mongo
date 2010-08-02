/*
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2009 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"
#include "util.h"

const char *progname;

int	bulk_callback(DB *, DBT **, DBT **);
int	bulk_read(DBT *dbt, int);
int	config(DB *, char *);
int	config_process(DB *, char **);
int	usage(void);

int
main(int argc, char *argv[])
{
	extern char *optarg;
	extern int optind;
	DB *db;
	int ch, ret, text_input, tret, verbose;
	char **config_list, **config_next;

	WT_UTILITY_INTRO(progname, argv);

	/*
	 * We can't handle configuration-line information until we've opened
	 * the DB handle, so we need a place to store it for now.
	 */
	if ((config_next =
	    config_list = calloc(argc + 1, sizeof(char *))) == NULL) {
		fprintf(stderr, "%s: %s\n", progname, strerror(errno));
		return (EXIT_FAILURE);
	}

	text_input = verbose = 0;
	while ((ch = getopt(argc, argv, "c:f:TVv")) != EOF)
		switch (ch) {
		case 'c':			/* command-line option */
			*config_next++ = optarg;
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
			usage();
		}
	argc -= optind;
	argv += optind;

	/* The remaining argument is the database name. */
	if (argc != 1)
		return (usage());

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
		if (config_process(db, config_list) != 0)
			goto err;

		(void)remove(*argv);

		if ((ret = db->open(db, *argv, 0600, WT_CREATE)) != 0) {
			fprintf(stderr, "%s: Db.open: %s: %s\n",
			    progname, *argv, wiredtiger_strerror(ret));
			goto err;
		}

		if ((ret = db->bulk_load(db, WT_DUPLICATES,
		    verbose ? __wt_progress : NULL, bulk_callback)) != 0) {
			fprintf(stderr, "%s: Db.bulk_load: %s\n",
			    progname, wiredtiger_strerror(ret));
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
 * config_process --
 *	Process command-line configuration options.
 */
int
config_process(DB *db, char **list)
{
	int ret;

	for (; *list != NULL; ++list)
		if ((ret = config(db, *list)) != 0)
			return (ret);
	return (0);
}

/*
 * config --
 *	Process a single command-line configuration option.
 */
int
config(DB *db, char *opt)
{
	u_int32_t a, b, c, d, e;
	u_long v;
	int ret;
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
		return (EXIT_FAILURE);
	}
	if (strcmp(opt, "allocsize") == 0) {
		if ((ret = db->btree_pagesize_get(db, &a, &b, &c, &d, &e)) != 0)
			return (ret);
		return (db->btree_pagesize_set(db, v, b, c, d, e));
	}
	if (strcmp(opt, "intlmin") == 0) {
		if ((ret = db->btree_pagesize_get(db, &a, &b, &c, &d, &e)) != 0)
			return (ret);
		return (db->btree_pagesize_set(db, a, v, c, d, e));
	}
	if (strcmp(opt, "intlmax") == 0) {
		if ((ret = db->btree_pagesize_get(db, &a, &b, &c, &d, &e)) != 0)
			return (ret);
		return (db->btree_pagesize_set(db, a, b, v, d, e));
	}
	if (strcmp(opt, "leafmin") == 0) {
		if ((ret = db->btree_pagesize_get(db, &a, &b, &c, &d, &e)) != 0)
			return (ret);
		return (db->btree_pagesize_set(db, a, b, c, v, e));
	}
	if (strcmp(opt, "leafmax") == 0) {
		if ((ret = db->btree_pagesize_get(db, &a, &b, &c, &d, &e)) != 0)
			return (ret);
		return (db->btree_pagesize_set(db, a, b, c, d, v));
	}

	fprintf(stderr,
	    "%s: -c option %s has an unknown keyword\n", progname, opt);
	return (EXIT_FAILURE);
}

/*
 * bulk_read --
 *	Read a line from stdin into a DBT.
 */
int
bulk_read(DBT *dbt, int iskey)
{
	static u_int64_t line = 0;
	size_t len;
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
		((u_int8_t *)(dbt->data))[len] = ch;
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

	WT_CC_QUIET(db, NULL);

	if ((ret = bulk_read(&key, 1)) != 0)
		return (ret);
	if ((ret = bulk_read(&data, 0)) != 0)
		return (ret);

	*keyp = &key;
	*datap = &data;
	return (0);
}

int
usage()
{
	(void)fprintf(stderr,
	    "usage: %s [-TVv] [-c configuration] [-f input-file] database\n",
	    progname);
	return (EXIT_FAILURE);
}
