/*-
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

static void startup(void);
static void usage(void);

extern int __wt_optind;
extern int __wt_getopt(const char *, int, char * const *, const char *);
extern char *__wt_optarg;

int
main(int argc, char *argv[])
{
	int ch, reps, ret;
	const char *config, *home;

	config = NULL;

	if ((g.progname = strrchr(argv[0], '/')) == NULL)
		g.progname = argv[0];
	else
		++g.progname;

#if 0
	/* Configure the GNU malloc for debugging. */
	(void)setenv("MALLOC_CHECK_", "2", 1);
#endif
#if 0
	/* Configure the FreeBSD malloc for debugging. */
	(void)setenv("MALLOC_OPTIONS", "AJ", 1);
#endif

	/* Track progress unless we're re-directing output to a file. */
	g.track = isatty(1) ? 1 : 0;

	/* Set values from the command line. */
	home = NULL;
	while ((ch = __wt_getopt(
	    g.progname, argc, argv, "1C:c:H:h:Llqrt:")) != EOF)
		switch (ch) {
		case '1':			/* One run */
			g.c_runs = 1;
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
			g.track = 0;
			break;
		case 'r':			/* Replay a run */
			g.replay = 1;
			break;
		default:
			usage();
		}
	argc -= __wt_optind;
	argv += __wt_optind;

	/* Set up paths. */
	path_setup(home);

	/* If it's a replay, use the home directory's CONFIG file. */
	if (g.replay) {
		if (config != NULL)
			die(EINVAL, "-c incompatible with -r");
		if (access(g.home_config, R_OK) != 0)
			die(ENOENT, "%s", g.home_config);
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

	/* Use line buffering on stdout so status updates aren't buffered. */
	(void)setvbuf(stdout, NULL, _IOLBF, 32);

	/*
	 * Initialize locks to single-thread named checkpoints and backups, and
	 * to single-thread last-record updates.
	 */
	if ((ret = pthread_rwlock_init(&g.append_lock, NULL)) != 0)
		die(ret, "pthread_rwlock_init: append lock");
	if ((ret = pthread_rwlock_init(&g.backup_lock, NULL)) != 0)
		die(ret, "pthread_rwlock_init: backup lock");

	/* Seed the random number generator. */
	srand((u_int)(0xdeadbeef ^ (u_int)time(NULL)));

	printf("%s: process %" PRIdMAX "\n", g.progname, (intmax_t)getpid());
	while (++g.run_cnt <= g.c_runs || g.c_runs == 0 ) {
		startup();			/* Start a run */

		config_setup();			/* Run configuration */
		config_print(0);		/* Dump run configuration */
		key_len_setup();		/* Setup keys */

		track("starting up", 0ULL, NULL);
		if (SINGLETHREADED)
			bdb_open();		/* Initial file config */
		wts_open(g.home, 1, &g.wts_conn);
		wts_create();

		wts_load();			/* Load initial records */
		wts_verify("post-bulk verify");	/* Verify */

						/* Loop reading & operations */
		for (reps = 0; reps < 3; ++reps) {
			wts_read_scan();	/* Read scan */

			if (g.c_ops != 0)	/* Random operations */
				wts_ops();

			/*
			 * Statistics.
			 *
			 * XXX
			 * Verify closes the underlying handle and discards the
			 * statistics, read them first.
			 */
			if (g.c_ops == 0 || reps == 2)
				wts_stats();

						/* Verify */
			wts_verify("post-ops verify");

			/*
			 * If no operations scheduled, quit after a single
			 * read pass.
			 */
			if (g.c_ops == 0)
				break;
		}

		track("shutting down", 0ULL, NULL);
		if (SINGLETHREADED)
			bdb_close();
		wts_close();

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
		if (g.track)
			printf("\r%78s\r", " ");
		printf("%4d: %s, %s\n",
		    g.run_cnt, g.c_data_source, g.c_file_type);
	}

	/* Flush/close any logging information. */
	if (g.logfp != NULL)
		(void)fclose(g.logfp);
	if (g.rand_log != NULL)
		(void)fclose(g.rand_log);

	config_print(0);

	if ((ret = pthread_rwlock_destroy(&g.append_lock)) != 0)
		die(ret, "pthread_rwlock_destroy: append lock");
	if ((ret = pthread_rwlock_destroy(&g.backup_lock)) != 0)
		die(ret, "pthread_rwlock_destroy: backup lock");

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
	int ret;

	/* Close the logging file. */
	if (g.logfp != NULL) {
		(void)fclose(g.logfp);
		g.logfp = NULL;
	}

	/* Close the random number logging file. */
	if (g.rand_log != NULL) {
		(void)fclose(g.rand_log);
		g.rand_log = NULL;
	}

	/* Create home if it doesn't yet exist. */
	if (access(g.home, X_OK) != 0 && mkdir(g.home, 0777) != 0)
		die(errno, "mkdir: %s", g.home);

	/* Remove the run's files except for rand. */
	if ((ret = system(g.home_init)) != 0)
		die(ret, "home directory initialization failed");

	/* Create the data-source directory. */
	if (mkdir(g.home_kvs, 0777) != 0)
		die(errno, "mkdir: %s", g.home_kvs);

	/*
	 * Open/truncate the logging file; line buffer so we see up-to-date
	 * information on error.
	 */
	if (g.logging != 0) {
		if ((g.logfp = fopen(g.home_log, "w")) == NULL)
			die(errno, "fopen: %s", g.home_log);
		(void)setvbuf(g.logfp, NULL, _IOLBF, 0);
	}

	/*
	 * Open/truncate the random number logging file; line buffer so we see
	 * up-to-date information on error.
	 */
	if ((g.rand_log = fopen(g.home_rand, g.replay ? "r" : "w")) == NULL)
		die(errno, "%s", g.home_rand);
	(void)setvbuf(g.rand_log, NULL, _IOLBF, 32);
}

/*
 * die --
 *	Report an error and quit, dumping the configuration.
 */
void
die(int e, const char *fmt, ...)
{
	va_list ap;

	if (fmt != NULL) {				/* Death message. */
		fprintf(stderr, "%s: ", g.progname);
		va_start(ap, fmt);
		vfprintf(stderr, fmt, ap);
		va_end(ap);
		if (e != 0)
			fprintf(stderr, ": %s", wiredtiger_strerror(e));
		fprintf(stderr, "\n");
	}

	/* Flush/close any logging information. */
	if (g.logfp != NULL)
		(void)fclose(g.logfp);
	if (g.rand_log != NULL)
		(void)fclose(g.rand_log);

	/* Display the configuration that failed. */
	if (g.run_cnt)
		config_print(1);

	exit(EXIT_FAILURE);
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
