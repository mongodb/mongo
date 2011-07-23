/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wts.h"

GLOBAL g;

static void restart(void);
static void usage(void);

int
main(int argc, char *argv[])
{
	int ch, reps, ret;

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

	/* Track progress unless we're re-directing output to a file. */
	g.track = isatty(STDOUT_FILENO) ? 1 : 0;

	/* Set values from the command line. */
	while ((ch = getopt(argc, argv, "1C:clqrv")) != EOF)
		switch (ch) {
		case '1':			/* One run */
			g.c_runs = 1;
			break;
		case 'C':			/* Configuration from a file */
			config_file(optarg);
			break;
		case 'c':			/* Display config strings */
			config_names();
			return (EXIT_SUCCESS);
		case 'l':
			g.logging = 1;
			break;
		case 'r':			/* Replay a run */
			g.replay = 1;
			g.c_runs = 1;
			break;
		case 'q':			/* Quiet */
			g.track = 0;
			break;
		case 'v':			/* Verbose */
			g.verbose = 1;
			break;
		default:
			usage();
		}

	argc -= optind;
	argv += optind;
	for (; *argv != NULL; ++argv)
		config_single(*argv, 1);

	printf("%s: process %" PRIdMAX "\n", g.progname, (intmax_t)getpid());
	while (++g.run_cnt <= g.c_runs || g.c_runs == 0 ) {
		restart();			/* Clean up previous runs */

		config_setup();
		key_gen_setup();

		bdb_startup();			/* Initial file config */
		if (wts_startup())
			return (EXIT_FAILURE);

		config_print(0);		/* Dump run configuration */

		if (wts_bulk_load())		/* Load initial records */
			goto err;
						/* Close, verify, re-open */
		if (wts_teardown() || wts_verify("bulk") || wts_startup())
			goto err;
						/* Loop reading & operations */
		for (reps = 0; reps < 3; ++reps) {		
			if (wts_read_scan())
				goto err;

			/*
			 * If no operations scheduled, quit after a single
			 * read pass.
			 */
			if (g.c_ops == 0)
				break;

			if (wts_ops())		/* Random operations */
				goto err;
						/* Close, verify, re-open */
			if (wts_teardown() ||
			    wts_verify("ops") || wts_startup())
				goto err;
		}

		if (wts_stats())		/* Statistics */
			goto err;
						/* Close the file */
		track("shutting down BDB", 0ULL);
		bdb_teardown();	

		if (wts_dump("standard", 1))	/* Dump the file */
			goto err;

		/*
		 * If we don't delete any records, we can salvage the file.  The
		 * problem with deleting records is that WiredTiger will restore
		 * deleted records during salvage when a page fragments, leaving
		 * a deleted record on one side of the split.
		 *
		 * Close, salvage, verify, re-open, dump.
		 */
		if (g.c_delete_pct == 0 && (
		    wts_teardown() ||
		    wts_salvage() ||
		    wts_verify("salvage") ||
		    wts_startup() ||
		    wts_dump("salvage", 0)))
			goto err;

		track("shutting down WT", 0ULL);
		if (wts_teardown())
			goto err;

		track(config_dtype(), 0ULL);
		track("\n", 0ULL);
	}

	if (0) {
err:		ret = EXIT_FAILURE;
	}

	if (g.rand_log != NULL)
		(void)fclose(g.rand_log);

	if (g.logfp != NULL)
		(void)fclose(g.logfp);

	config_print(ret ? 1 : 0);
	return (ret);
}

/*
 * restart --
 *	Clean up from previous runs.
 */
static void
restart(void)
{
	const char *p;

	if (g.logfp != NULL)
		(void)fclose(g.logfp);

	(void)system("rm -f __db* __bdb* __log __wt*");

	p = "__log";
	if (g.logging &&
	    (g.logfp = fopen(p, "w")) == NULL) {
		fprintf(stderr, "%s: %s\n", p, strerror(errno));
		exit(EXIT_FAILURE);
	}
}

/*
 * usage --
 *	Display usage statement and exit failure.
 */
static void
usage(void)
{
	(void)fprintf(stderr,
	    "usage: %s [-1clqrv] [-C config] [name=value ...]\n",
	    g.progname);
	exit(EXIT_FAILURE);
}
