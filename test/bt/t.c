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

static void	run_cleanup(void);
static void	run_init(int);
static void	usage(void);

int
main(int argc, char *argv[])
{
	int ch, log, run_cnt, runs;

	if ((g.progname = strrchr(argv[0], '/')) == NULL)
		g.progname = argv[0];
	else
		++g.progname;

	/* Configure the FreeBSD malloc for debugging. */
	(void)putenv("MALLOC_OPTIONS=AJZ");

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

	srand((int)g.c_rand_seed);
	printf("%s: process %lu\n", g.progname, (u_long)getpid());
	for (run_cnt = 1; runs == 0 || run_cnt <= runs; ++run_cnt) {
		run_init(log);

		printf("%50s\r%d\n", " ", run_cnt);
		config_dump(1);

		if (wts_bulk_load())
			goto err;
		if (wts_read_key())
			goto err;
		if (wts_read_recno())
			goto err;

		run_cleanup();
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

/*
 * run_init --
 *	Initialize each run.
 */
static void
run_init(int log)
{
	/* Clean up leftover files from the last run. */
	(void)system("rm -f bdb.db wt.*");

	/* Randomize any configuration not set from the command line. */
	config_init();

	bdb_setup();
	wts_setup(log);
}

/*
 * run_cleanup --
 *	Cleanup from each run.
 */
static void
run_cleanup()
{
	if (g.logfp != NULL)
		(void)fclose(g.logfp);

	bdb_teardown();
	wts_teardown();
}
