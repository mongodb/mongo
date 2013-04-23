/*-
 * Public Domain 2008-2013 WiredTiger, Inc.
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
WT_EXTENSION_API *wt_api;

static void onint(int);
static void startup(void);
static void usage(void);

int
main(int argc, char *argv[])
{
	int ch, reps;

	if ((g.progname = strrchr(argv[0], '/')) == NULL)
		g.progname = argv[0];
	else
		++g.progname;

	/* Configure the FreeBSD malloc for debugging. */
	(void)setenv("MALLOC_OPTIONS", "AJZ", 1);

	/* Set values from the "CONFIG" file, if it exists. */
	if (access("CONFIG", R_OK) == 0)
		config_file("CONFIG");

	/* Track progress unless we're re-directing output to a file. */
	g.track = isatty(STDOUT_FILENO) ? 1 : 0;

	/* Set values from the command line. */
	while ((ch = getopt(argc, argv, "1C:c:Llqrt:")) != EOF)
		switch (ch) {
		case '1':			/* One run */
			g.c_runs = 1;
			break;
		case 'C':			/* wiredtiger_open config */
			g.config_open = optarg;
			break;
		case 'c':			/* Configuration from a file */
			config_file(optarg);
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
			g.c_runs = 1;
			break;
		default:
			usage();
		}

	argc -= optind;
	argv += optind;
	for (; *argv != NULL; ++argv)
		config_single(*argv, 1);

	/* Use line buffering on stdout so status updates aren't buffered. */
	(void)setvbuf(stdout, NULL, _IOLBF, 0);

	/* Clean up on signal. */
	(void)signal(SIGINT, onint);

	printf("%s: process %" PRIdMAX "\n", g.progname, (intmax_t)getpid());
	while (++g.run_cnt <= g.c_runs || g.c_runs == 0 ) {
		startup();			/* Start a run */

		config_setup();			/* Run configuration */
		config_print(0);		/* Dump run configuration */
		key_len_setup();		/* Setup keys */

		if (SINGLETHREADED)
			bdb_open();		/* Initial file config */
		wts_open();

		wts_load();			/* Load initial records */
		wts_verify("post-bulk verify");	/* Verify */

						/* Loop reading & operations */
		for (reps = 0; reps < 3; ++reps) {
			wts_read_scan();	/* Read scan */

			if (g.c_ops != 0)	/* Random operations */
				wts_ops();

						/* Statistics */
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

		if (SINGLETHREADED) {
			track("shutting down BDB", 0ULL, NULL);
			bdb_close();

			wts_close();			/* Dump the file */
			wts_dump("standard", 1);
			wts_open();
		}

		/*
		 * If we don't delete any records, we can salvage the file.  The
		 * problem with deleting records is that salvage will restore
		 * deleted records if a page fragments leaving a deleted record
		 * on one side of the split.
		 *
		 * Save a copy, salvage, verify, dump.
		 */
		if (g.c_delete_pct == 0) {
			wts_salvage();			/* Salvage & verify */
			wts_verify("post-salvage verify");

			wts_close();			/* Dump the file */
			wts_dump("salvage", 0);
			wts_open();
		}

		wts_close();			/* Close */

		/* Overwrite the progress line with a completion line. */
		printf("\r%78s\r%4d: %s %s\n",
		    " ", g.run_cnt, g.c_file_type, g.c_data_source);
	}

	/* Flush/close any logging information. */
	if (g.logfp != NULL)
		(void)fclose(g.logfp);
	if (g.rand_log != NULL)
		(void)fclose(g.rand_log);

	config_print(0);
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
	/* Close the logging file. */
	if (g.logfp != NULL) {
		(void)fclose(g.logfp);
		g.logfp = NULL;
	}

	/* Close the random number file. */
	if (g.rand_log != NULL) {
		(void)fclose(g.rand_log);
		g.rand_log = NULL;
	}

	/* Create RUNDIR if it doesn't yet exist. */
	if (access(RUNDIR, X_OK) != 0 && mkdir(RUNDIR, 0777) != 0)
		die(errno, "mkdir: %s", RUNDIR);

	/* Remove the run's files except for rand. */
	(void)system("cd RUNDIR && rm -rf `ls | sed /rand/d`");

	/* Create the data-source directory. */
	if (mkdir(RUNDIR_KVS, 0777) != 0)
		die(errno, "mkdir: %s", RUNDIR_KVS);

	/* Open/truncate the logging file. */
	if (g.logging != 0) {
		if ((g.logfp = fopen("RUNDIR/log", "w")) == NULL)
			die(errno, "fopen: RUNDIR/log");
		(void)setvbuf(g.logfp, NULL, _IOLBF, 0);
	}
}

/*
 * onint --
 *	Interrupt signal handler.
 */
static void
onint(int signo)
{
	UNUSED(signo);

	/* Remove the run's files except for rand. */
	(void)system("cd RUNDIR && rm -rf `ls | sed /rand/d`");

	fprintf(stderr, "\n");
	exit(EXIT_FAILURE);
}

/*
 * syserr --
 *	Die on a system error.
 */
void
syserr(const char *f)
{
	fprintf(stderr, "%s: %s: %s\n", g.progname, f, strerror(errno));
	exit(EXIT_FAILURE);
}

/*
 * die --
 *	Report an error and quit.
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
	    "usage: %s [-1Llqr]\n    "
	    "[-C wiredtiger-config] [-c config-file] "
	    "[name=value ...]\n",
	    g.progname);
	fprintf(stderr, "%s",
	    "\t-1 run once\n"
	    "\t-C specify wiredtiger_open configuration arguments\n"
	    "\t-c read test program configuration from a file\n"
	    "\t-L output to a log file\n"
	    "\t-l log operations (implies -L)\n"
	    "\t-q run quietly\n"
	    "\t-r replay the last run\n");

	fprintf(stderr, "\n");

	config_error();
	exit(EXIT_FAILURE);
}
