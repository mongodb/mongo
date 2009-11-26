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
void	progress(const char *, u_int32_t);

int
main(int argc, char *argv[])
{
	extern char *optarg;
	extern int optind;
	DB *db;
	int ch, ret, tret, verbose;

	WT_UTILITY_INTRO(progname, argv);

	while ((ch = getopt(argc, argv, "Vv")) != EOF)
		switch (ch) {
		case 'v':			/* verbose */
			verbose = 1;
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

	if ((ret = wiredtiger_simple_setup(progname, &db)) == 0) {
		if ((ret = db->open(db, *argv, 0, 0)) != 0) {
			fprintf(stderr, "%s: Db.open: %s: %s\n",
			    progname, *argv, wt_strerror(ret));
			goto err;
		}
		if ((ret = db->verify(db, verbose ? progress : NULL, 0)) != 0) {
			fprintf(stderr, "%s: Db.verify: %s\n",
			    progname, wt_strerror(ret));
			goto err;
		}
	}

	if (0) {
err:		ret = 1;
	}
	if ((tret = wiredtiger_simple_teardown(progname, db)) != 0 && ret == 0)
		ret = tret;
	return (ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}

void
progress(const char *s, u_int32_t p)
{
	printf("\r\t%s: %lu", s, (u_long)p);
	fflush(stdout);
}

int
usage()
{
	(void)fprintf(stderr, "usage: %s [-Vv] database\n", progname);
	return (EXIT_FAILURE);
}
