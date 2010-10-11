/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2010 WiredTiger, Inc.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wts.h"

GLOBAL g;

static void	usage(void);
static void	restart(void);

int
main(int argc, char *argv[])
{
	int ch, log, reps;

	if ((g.progname = strrchr(argv[0], '/')) == NULL)
		g.progname = argv[0];
	else
		++g.progname;

	/* Configure the FreeBSD malloc for debugging. */
	(void)putenv("MALLOC_OPTIONS=AJZ");

	/* Set values from the "CONFIG" file, if it exists. */
	if (access("CONFIG", R_OK) == 0) {
		printf("... reading CONFIG file\n");
		config_file("CONFIG");
	}

	/* Set values from the command line. */
	log = 0;
	while ((ch = getopt(argc, argv, "1C:cd:lrsv")) != EOF)
		switch (ch) {
		case '1':
			g.c_runs = 1;
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
			break;
		case 'l':
			log = 1;
			break;
		case 'r':
			g.replay = 1;
			g.c_runs = 1;
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
	while (++g.run_cnt <= g.c_runs || g.c_runs == 0 ) {
		restart();			/* Clean up previous runs */

		bdb_setup(0);			/* Open the databases */
		if (wts_setup(0, log))
			goto err;

		config_dump(0);

		if (wts_bulk_load())		/* Load initial records */
			goto err;

		if (wts_verify())		/* Verify the database */
			goto err;

		if (g.dump && wts_dump())	/* Optional dump */
			goto err;

		/* XXX: can't get dups, don't have cursor ops yet. */
		if (g.c_duplicates_pct != 0)
			goto skip_ops;
						/* Loop reading & operations */
		for (reps = 0; reps < 3; ++reps) {		
			switch (g.c_database_type) {
			case ROW:
				if (wts_read_row_scan())
					goto err;
				break;
			case FIX:
			case VAR:
				if (wts_read_col_scan())
					goto err;
				break;
			}

						/* Close/re-open database */
			track("flushing & re-opening WT", (u_int64_t)0);
			wts_teardown();
			if (wts_setup(1, log))
				goto err;

			if (wts_ops())		/* Random operations */
				goto err;
		}

skip_ops:	if (g.stats && wts_stats())	/* Optional statistics */
			goto err;
						/* Close the databases */
		track("shutting down BDB", (u_int64_t)0);
		bdb_teardown();	
		track("shutting down WT", (u_int64_t)0);
		wts_teardown();

		track("done", (u_int64_t)0);
		printf("\n");
	}

	return (EXIT_SUCCESS);

err:	config_dump(1);
	return (EXIT_FAILURE);
}

/*
 * restart --
 *	Clean up from previous runs.
 */
static void
restart()
{
	system("rm -f __bdb* __wt*");
}

/*
 * usage --
 *	Display usage statement and exit failure.
 */
static void
usage()
{
	(void)fprintf(stderr,
	    "usage: %s [-1clrsv] [-C config] [-d debug | print] "
	    "[name=value ...]\n",
	    g.progname);
	exit(EXIT_FAILURE);
}
