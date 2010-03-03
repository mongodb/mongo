/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wts.h"

GLOBAL g;

static void	usage(void);

int
main(int argc, char *argv[])
{
	int ch, log, runs;

	if ((g.progname = strrchr(argv[0], '/')) == NULL)
		g.progname = argv[0];
	else
		++g.progname;

	/* Configure the FreeBSD malloc for debugging. */
	(void)putenv("MALLOC_OPTIONS=AJZ");

	/* Set values from the "wts.config" file, if it exists. */
	if (access("wts.config", R_OK) == 0) {
		printf("... configuring from wts.config\n");
		config_file("wts.config");
	}

	/* Set values from the command line. */
	log = runs = 0;
	while ((ch = getopt(argc, argv, "1C:cd:lsv")) != EOF)
		switch (ch) {
		case '1':
			runs = 1;
			break;
		case 'C':
			config_file(optarg);
			break;
		case 'c':
			config_names();
			return (EXIT_SUCCESS);
		case 'd':
			switch (optarg[0]) {
			case 'd':
				g.dump = DUMP_DEBUG;
				break;
			case 'p':
				g.dump = DUMP_PRINT;
				break;
			default:
				usage();
			}
		case 'l':
			log = 1;
			break;
		case 's':
			g.stats = 1;
			break;
		case 'v':
			g.verbose = 1;
			break;
		default:
			usage();
		}

	argc -= optind;
	argv += optind;
	for (; *argv != NULL; ++argv)
		config_single(*argv);

	printf("%s: process %lu\n", g.progname, (u_long)getpid());
	for (g.run_cnt = 1; runs == 0 || g.run_cnt <= runs; ++g.run_cnt) {
		config();

		bdb_setup(0);
		wts_setup(0, log);

		config_dump(1);

		if (wts_bulk_load())
			goto err;

		bdb_teardown();
		wts_teardown();
		bdb_setup(1);
		wts_setup(1, log);

		if (g.c_database_type == ROW && wts_read_key())
			goto err;
		if (wts_read_recno())
			goto err;

		bdb_teardown();
		wts_teardown();
	}

	return (EXIT_SUCCESS);

err:	config_dump(0);
	return (EXIT_FAILURE);
}

/*
 * usage --
 *	Display usage statement and exit failure.
 */
static void
usage()
{
	(void)fprintf(stderr,
	    "usage: %s [-1clsv] [-C config] [-d debug | print] "
	    "[name=value ...]\n",
	    g.progname);
	exit (EXIT_FAILURE);
}
