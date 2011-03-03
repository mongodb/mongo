/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wts.h"

GLOBAL g;

static void restart(void);
static void usage(void);

int
main(int argc, char *argv[])
{
	int ch, log, reps;

	if ((g.progname = strrchr(argv[0], '/')) == NULL)
		g.progname = argv[0];
	else
		++g.progname;

	/* Configure the FreeBSD malloc for debugging. */
	(void)setenv("MALLOC_OPTIONS", "AJZ", 1);

	/* Set values from the "CONFIG" file, if it exists. */
	if (access("CONFIG", R_OK) == 0) {
		printf("... reading CONFIG file\n");
		config_file("CONFIG");
	}

	/* Set values from the command line. */
	log = 0;
	while ((ch = getopt(argc, argv, "1C:clrv")) != EOF)
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
		case 'l':
			log = 1;
			break;
		case 'r':
			g.replay = 1;
			g.c_runs = 1;
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

		config_setup();
		key_gen_setup();

		bdb_startup();			/* Initial file config */
		if (wts_startup(log))
			return (EXIT_FAILURE);

		config_dump(0);			/* Dump run configuration */

		if (wts_bulk_load())		/* Load initial records */
			goto err;

		if (wts_verify())		/* Verify the file */
			goto err;

						/* Loop reading & operations */
		for (reps = 0; reps < 3; ++reps) {		
			switch (g.c_file_type) {
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

			/*
			 * If no operations scheduled, quit after a single
			 * read pass.
			 */
			if (g.c_ops == 0)
				break;

			if (wts_ops())		/* Random operations */
				goto err;

			if (wts_verify())	/* Verify the file */
				goto err;

			wts_teardown();		/* Close and  re-open */
			if (wts_startup(0))
				goto err;

			if (wts_verify())	/* Verify the file */
				goto err;
		}

		if (wts_stats())		/* Statistics */
			goto err;
						/* Close the file */
		track("shutting down BDB", 0);
		bdb_teardown();	

		if (wts_dump())
			goto err;

		track("shutting down WT", 0);
		wts_teardown();

		track(config_dtype(), 0);
		track("\n", 0);
	}

	if (g.rand_log != NULL)
		(void)fclose(g.rand_log);

	return (EXIT_SUCCESS);

err:	config_dump(1);
	return (EXIT_FAILURE);
}

/*
 * restart --
 *	Clean up from previous runs.
 */
static void
restart(void)
{
	system("rm -f __bdb* __wt*");
}

/*
 * usage --
 *	Display usage statement and exit failure.
 */
static void
usage(void)
{
	(void)fprintf(stderr,
	    "usage: %s [-1clrv] [-C config] [name=value ...]\n",
	    g.progname);
	exit(EXIT_FAILURE);
}
