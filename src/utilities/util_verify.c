/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"
#include "util.h"

const char *progname;

int	usage(void);

int
main(int argc, char *argv[])
{
	BTREE *btree;
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

	/* The remaining argument is the file name. */
	if (argc != 1)
		return (usage());

	if ((ret = wiredtiger_simple_setup(progname, verbose ?
	    __wt_event_handler_verbose : NULL, NULL, &btree)) == 0) {
		if ((ret = btree->open(btree, NULL, *argv, 0, 0)) != 0) {
			fprintf(stderr, "%s: btree.open(%s): %s\n",
			    progname, *argv, wiredtiger_strerror(ret));
			goto err;
		}
		/* XXX verbose */
		if ((ret = btree->verify(btree, NULL, 0)) != 0) {
			fprintf(stderr, "%s: btree.verify(%s): %s\n",
			    progname, *argv, wiredtiger_strerror(ret));
			goto err;
		}
		if (verbose)
			printf("\n");
	}

	if (0) {
err:		ret = 1;
	}
	if ((tret = wiredtiger_simple_teardown(progname, btree)) != 0 && ret == 0)
		ret = tret;
	return (ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}

int
usage(void)
{
	(void)fprintf(stderr, "usage: %s [-Vv] file\n", progname);
	return (EXIT_FAILURE);
}
