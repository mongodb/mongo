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
	WT_TOC *toc;
	u_int32_t flags;
	int ch, ret, tret;

	WT_UTILITY_INTRO(progname, argv);

	while ((ch = getopt(argc, argv, "df:p")) != EOF)
		switch (ch) {
		case 'd':
			flags = WT_DEBUG;
			break;
		case 'f':			/* output file */
			if (freopen(optarg, "w", stdout) == NULL) {
				fprintf(stderr, "%s: %s: reopen: %s\n",
				    progname, optarg, strerror(errno));
				return (EXIT_FAILURE);
			}
			break;
		case 'p':
			flags = WT_PRINTABLES;
			break;
		case 'V':			/* version */
			printf("%s\n", wt_version(NULL, NULL, NULL));
			return (EXIT_SUCCESS);
		case '?':
		default:
			usage();
		}
	argc -= optind;
	argv += optind;

	/* The remaining argument is the database name. */
	if (argc != 1)
		return (usage());

	if ((ret = __wt_simple_setup(progname, 1, &toc, &db)) == 0) {
		if ((ret = db->open(db, toc, *argv, 0, 0)) != 0) {
			fprintf(stderr, "%s: Db.open: %s: %s\n",
			    progname, *argv, wt_strerror(ret));
			goto err;
		}
		if ((ret = db->dump(db, toc, stdout, flags)) != 0) {
			fprintf(stderr, "%s: Db.dump: %s\n",
			    progname, wt_strerror(ret));
			goto err;
		}
	}

	if (0) {
err:		ret = 1;
	}
	if ((tret = __wt_simple_teardown(progname, toc, db)) != 0 && ret == 0)
		ret = tret;
	return (ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}

int
usage()
{
	(void)fprintf(stderr,
	    "usage: %s [-dpV] [-f output-file] database\n", progname);
	return (EXIT_FAILURE);
}
