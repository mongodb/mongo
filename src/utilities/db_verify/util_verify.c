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

int	usage(void);

int
main(int argc, char *argv[])
{
	extern char *optarg;
	extern int optind;
	DB *db;
	int ch, ret, tret, verbose;

	WT_UTILITY_INTRO(progname, argv);

	verbose = 0;
	while ((ch = getopt(argc, argv, "Vv")) != EOF)
		switch (ch) {
		case 'v':			/* verbose */
			verbose = 1;
			break;
		case 'V':			/* version */
			printf("%s\n", wiredtiger_version(NULL, NULL, NULL));
			return (EXIT_SUCCESS);
		case '?':
		default:
			return (usage());
		}
	argc -= optind;
	argv += optind;

	/* The remaining argument is the database name. */
	if (argc != 1)
		return (usage());

	if ((ret = wiredtiger_simple_setup(progname, &db, 0, 0)) == 0) {
		if ((ret = db->open(db, *argv, 0, 0)) != 0) {
			db->err(db, ret, "Db.open: %s", *argv);
			goto err;
		}
		if ((ret =
		    db->verify(db, verbose ? __wt_progress : NULL, 0)) != 0) {
			db->err(db, ret, "Db.verify: %s", *argv);
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

int
usage()
{
	(void)fprintf(stderr, "usage: %s [-Vv] database\n", progname);
	return (EXIT_FAILURE);
}
