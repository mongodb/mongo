/*-
 * Public Domain 2014-2016 MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "format.h"

GLOBAL g;

static void format_die(void);
static void startup(void);
static void usage(void)
    WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));

extern int __wt_optind;
extern char *__wt_optarg;

void (*custom_die)(void) = format_die;		/* Local death handler. */

int
main(int argc, char *argv[])
{
	time_t start;
	int ch, i, onerun, reps;
	const char *config, *home;

	config = NULL;

#ifdef _WIN32
	g.progname = "t_format.exe";
#else
	if ((g.progname = strrchr(argv[0], DIR_DELIM)) == NULL)
		g.progname = argv[0];
	else
		++g.progname;
#endif

#if 0
	/* Configure the GNU malloc for debugging. */
	(void)setenv("MALLOC_CHECK_", "2", 1);
#endif
#if 0
	/* Configure the FreeBSD malloc for debugging. */
	(void)setenv("MALLOC_OPTIONS", "AJ", 1);
#endif

	/* Track progress unless we're re-directing output to a file. */
	g.c_quiet = isatty(1) ? 0 : 1;

	/* Set values from the command line. */
	home = NULL;
	onerun = 0;
	while ((ch = __wt_getopt(
	    g.progname, argc, argv, "1C:c:H:h:Llqrt:")) != EOF)
		switch (ch) {
		case '1':			/* One run */
			onerun = 1;
			break;
		case 'C':			/* wiredtiger_open config */
			g.config_open = __wt_optarg;
			break;
		case 'c':			/* Configuration from a file */
			config = __wt_optarg;
			break;
		case 'H':
			g.helium_mount = __wt_optarg;
			break;
		case 'h':
			home = __wt_optarg;
			break;
		case 'L':			/* Re-direct output to a log */
			/*
			 * The -l option is a superset of -L, ignore -L if we
			 * have already configured logging for operations.
			 */
			if (g.logging == 0)
				g.logging = LOG_FILE;
			break;
		case 'l':			/* Turn on operation logging */
			g.logging = LOG_OPS;
			break;
		case 'q':			/* Quiet */
			g.c_quiet = 1;
			break;
		case 'r':			/* Replay a run */
			g.replay = 1;
			break;
		default:
			usage();
		}
	argc -= __wt_optind;
	argv += __wt_optind;

	/*
	 * Initialize the global RNG. Start with the standard seeds, and then
	 * use seconds since the Epoch modulo a prime to run the RNG for some
	 * number of steps, so we don't start with the same values every time.
	 */
	__wt_random_init(&g.rnd);
	for (i = (int)time(NULL) % 10007; i > 0; --i)
		(void)__wt_random(&g.rnd);

	/* Set up paths. */
	path_setup(home);

	/* If it's a replay, use the home directory's CONFIG file. */
	if (g.replay) {
		if (config != NULL)
			testutil_die(EINVAL, "-c incompatible with -r");
		if (access(g.home_config, R_OK) != 0)
			testutil_die(ENOENT, "%s", g.home_config);
		config = g.home_config;
	}

	/*
	 * If we weren't given a configuration file, set values from "CONFIG",
	 * if it exists.
	 *
	 * Small hack to ignore any CONFIG file named ".", that just makes it
	 * possible to ignore any local CONFIG file, used when running checks.
	 */
	if (config == NULL && access("CONFIG", R_OK) == 0)
		config = "CONFIG";
	if (config != NULL && strcmp(config, ".") != 0)
		config_file(config);

	/*
	 * The rest of the arguments are individual configurations that modify
	 * the base configuration.
	 */
	for (; *argv != NULL; ++argv)
		config_single(*argv, 1);

	/*
	 * Multithreaded runs can be replayed: it's useful and we'll get the
	 * configuration correct.  Obviously the order of operations changes,
	 * warn the user.
	 */
	if (g.replay && !SINGLETHREADED)
		printf("Warning: replaying a threaded run\n");

	/*
	 * Single-threaded runs historically exited after a single replay, which
	 * makes sense when you're debugging, leave that semantic in place.
	 */
	if (g.replay && SINGLETHREADED)
		g.c_runs = 1;

	/*
	 * Let the command line -1 flag override runs configured from other
	 * sources.
	 */
	if (onerun)
		g.c_runs = 1;

	/*
	 * Initialize locks to single-thread named checkpoints and backups, last
	 * last-record updates, and failures.
	 */
	testutil_check(pthread_rwlock_init(&g.append_lock, NULL));
	testutil_check(pthread_rwlock_init(&g.backup_lock, NULL));
	testutil_check(pthread_rwlock_init(&g.checkpoint_lock, NULL));
	testutil_check(pthread_rwlock_init(&g.death_lock, NULL));

	printf("%s: process %" PRIdMAX "\n", g.progname, (intmax_t)getpid());
	while (++g.run_cnt <= g.c_runs || g.c_runs == 0 ) {
		startup();			/* Start a run */

		config_setup();			/* Run configuration */
		config_print(0);		/* Dump run configuration */
		key_len_setup();		/* Setup keys */

		start = time(NULL);
		track("starting up", 0ULL, NULL);

#ifdef HAVE_BERKELEY_DB
		if (SINGLETHREADED)
			bdb_open();		/* Initial file config */
#endif
		wts_open(g.home, true, &g.wts_conn);
		wts_init();

		wts_load();			/* Load initial records */
		wts_verify("post-bulk verify");	/* Verify */

		/*
		 * If we're not doing any operations, scan the bulk-load, copy
		 * the statistics and we're done. Otherwise, loop reading and
		 * operations, with a verify after each set.
		 */
		if (g.c_timer == 0 && g.c_ops == 0) {
			wts_read_scan();		/* Read scan */
			wts_stats();			/* Statistics */
		} else
			for (reps = 1; reps <= FORMAT_OPERATION_REPS; ++reps) {
				wts_read_scan();	/* Read scan */

							/* Operations */
				wts_ops(reps == FORMAT_OPERATION_REPS);

				/*
				 * Copy out the run's statistics after the last
				 * set of operations.
				 *
				 * XXX
				 * Verify closes the underlying handle and
				 * discards the statistics, read them first.
				 */
				if (reps == FORMAT_OPERATION_REPS)
					wts_stats();

							/* Verify */
				wts_verify("post-ops verify");
			}

		track("shutting down", 0ULL, NULL);
#ifdef HAVE_BERKELEY_DB
		if (SINGLETHREADED)
			bdb_close();
#endif
		wts_close();

		/*
		 * Rebalance testing.
		 */
		wts_rebalance();

		/*
		 * If single-threaded, we can dump and compare the WiredTiger
		 * and Berkeley DB data sets.
		 */
		if (SINGLETHREADED)
			wts_dump("standard", 1);

		/*
		 * Salvage testing.
		 */
		wts_salvage();

		/* Overwrite the progress line with a completion line. */
		if (!g.c_quiet)
			printf("\r%78s\r", " ");
		printf("%4d: %s, %s (%.0f seconds)\n",
		    g.run_cnt, g.c_data_source,
		    g.c_file_type, difftime(time(NULL), start));
		fflush(stdout);
	}

	/* Flush/close any logging information. */
	fclose_and_clear(&g.logfp);
	fclose_and_clear(&g.randfp);

	config_print(0);

	testutil_check(pthread_rwlock_destroy(&g.append_lock));
	testutil_check(pthread_rwlock_destroy(&g.backup_lock));
	testutil_check(pthread_rwlock_destroy(&g.checkpoint_lock));
	testutil_check(pthread_rwlock_destroy(&g.death_lock));

	config_clear();

	return (EXIT_SUCCESS);
}

/*
 * startup --
 *	Initialize for a run.
 */
static void
startup(void)
{
	WT_DECL_RET;

	/* Flush/close any logging information. */
	fclose_and_clear(&g.logfp);
	fclose_and_clear(&g.randfp);

	/* Create or initialize the home and data-source directories. */
	if ((ret = system(g.home_init)) != 0)
		testutil_die(ret, "home directory initialization failed");

	/* Open/truncate the logging file. */
	if (g.logging != 0 && (g.logfp = fopen(g.home_log, "w")) == NULL)
		testutil_die(errno, "fopen: %s", g.home_log);

	/* Open/truncate the random number logging file. */
	if ((g.randfp = fopen(g.home_rand, g.replay ? "r" : "w")) == NULL)
		testutil_die(errno, "%s", g.home_rand);
}

/*
 * die --
 *	Report an error, dumping the configuration.
 */
static void
format_die(void)
{
	/*
	 * Single-thread error handling, our caller exits after calling
	 * us - don't release the lock.
	 */
	(void)pthread_rwlock_wrlock(&g.death_lock);

	/* Try and turn off tracking so it doesn't obscure the error message. */
	if (!g.c_quiet) {
		g.c_quiet = 1;
		fprintf(stderr, "\n");
	}

	/* Flush/close any logging information. */
	fclose_and_clear(&g.logfp);
	fclose_and_clear(&g.randfp);

	/* Display the configuration that failed. */
	if (g.run_cnt)
		config_print(1);
}

/*
 * usage --
 *	Display usage statement and exit failure.
 */
static void
usage(void)
{
	fprintf(stderr,
	    "usage: %s [-1Llqr] [-C wiredtiger-config]\n    "
	    "[-c config-file] [-H mount] [-h home] "
	    "[name=value ...]\n",
	    g.progname);
	fprintf(stderr, "%s",
	    "\t-1 run once\n"
	    "\t-C specify wiredtiger_open configuration arguments\n"
	    "\t-c read test program configuration from a file\n"
	    "\t-H mount Helium volume mount point\n"
	    "\t-h home (default 'RUNDIR')\n"
	    "\t-L output to a log file\n"
	    "\t-l log operations (implies -L)\n"
	    "\t-q run quietly\n"
	    "\t-r replay the last run\n");

	config_error();
	exit(EXIT_FAILURE);
}
