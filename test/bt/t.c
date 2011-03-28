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

	/* Track progress unless we're re-directing output to a file. */
	g.track = isatty(STDOUT_FILENO) ? 1 : 0;

	/* Set values from the command line. */
	log = 0;
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
		case 'l':			/* Log operations */
			log = 1;
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

	printf("%s: process %lu\n", g.progname, (u_long)getpid());
	while (++g.run_cnt <= g.c_runs || g.c_runs == 0 ) {
		restart();			/* Clean up previous runs */

		config_setup();
		key_gen_setup();

		bdb_startup();			/* Initial file config */
		if (wts_startup(log))
			return (EXIT_FAILURE);

		config_print(0);		/* Dump run configuration */

		if (wts_bulk_load())		/* Load initial records */
			goto err;

		if (wts_verify())		/* Verify the file */
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

		if (wts_dump())			/* Dump the file */
			goto err;

#if 0
		track("salvage", 0);
		if (wts_salvage())		/* Salvage the file */
			goto err;
#endif

		if (wts_verify())		/* Verify the file */
			goto err;

		track("shutting down WT", 0);
		wts_teardown();

		track(config_dtype(), 0);
		track("\n", 0);
	}

	if (g.rand_log != NULL)
		(void)fclose(g.rand_log);

	return (EXIT_SUCCESS);

err:	config_print(1);
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
	    "usage: %s [-1clqrv] [-C config] [name=value ...]\n",
	    g.progname);
	exit(EXIT_FAILURE);
}
