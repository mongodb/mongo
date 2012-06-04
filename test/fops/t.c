/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "thread.h"

WT_CONNECTION *conn;				/* WiredTiger connection */
u_int nops;					/* Operations */
const char *uri;				/* Object */

static char *progname;				/* Program name */
static FILE *logfp;				/* Log file */

static int  handle_error(WT_EVENT_HANDLER *, int, const char *);
static int  handle_message(WT_EVENT_HANDLER *, const char *);
static void onint(int);
static void shutdown(void);
static int  usage(void);
static void wt_startup(char *);
static void wt_shutdown(void);

int
main(int argc, char *argv[])
{
	u_int nthreads;
	int ch, cnt, runs;
	char *config_open;

	if ((progname = strrchr(argv[0], '/')) == NULL)
		progname = argv[0];
	else
		++progname;

	config_open = NULL;
	nops = 1000;
	nthreads = 10;
	runs = 1;

	while ((ch = getopt(argc, argv, "C:l:n:r:t:")) != EOF)
		switch (ch) {
		case 'C':			/* wiredtiger_open config */
			config_open = optarg;
			break;
		case 'l':			/* log */
			if ((logfp = fopen(optarg, "w")) == NULL) {
				fprintf(stderr,
				    "%s: %s\n", optarg, strerror(errno));
				return (EXIT_FAILURE);
			}
			break;
		case 'n':			/* operations */
			nops = (u_int)atoi(optarg);
			break;
		case 'r':			/* runs */
			runs = atoi(optarg);
			break;
		case 't':
			nthreads = (u_int)atoi(optarg);
			break;
		default:
			return (usage());
		}

	argc -= optind;
	argv += optind;
	if (argc != 0)
		return (usage());

	/* Clean up on signal. */
	(void)signal(SIGINT, onint);

	printf("%s: process %" PRIu64 "\n", progname, (uint64_t)getpid());
	for (cnt = 1; runs == 0 || cnt <= runs; ++cnt) {
		shutdown();			/* Clean up previous runs */

		uri = "file:__wt";
		printf("    %d: %u threads on %s\n", cnt, nthreads, uri);
		wt_startup(config_open);
		if (fop_start(nthreads))
			return (EXIT_FAILURE);
		wt_shutdown();
		printf("\n");

		uri = "table:__wt";
		printf("    %d: %u threads on %s\n", cnt, nthreads, uri);
		wt_startup(config_open);
		if (fop_start(nthreads))
			return (EXIT_FAILURE);
		wt_shutdown();
		printf("\n");
	}
	return (0);
}

/*
 * wt_startup --
 *	Configure the WiredTiger connection.
 */
static void
wt_startup(char *config_open)
{
	static WT_EVENT_HANDLER event_handler = {
		handle_error,
		handle_message,
		NULL
	};
	int ret;
	char config[128];

	snprintf(config, sizeof(config),
	    "create,error_prefix=\"%s\",cache_size=5MB%s%s",
	    progname,
	    config_open == NULL ? "" : ",",
	    config_open == NULL ? "" : config_open);

	if ((ret = wiredtiger_open(NULL, &event_handler, config, &conn)) != 0)
		die("wiredtiger_open", ret);
}

/*
 * wt_shutdown --
 *	Flush the file to disk and shut down the WiredTiger connection.
 */
static void
wt_shutdown(void)
{
	int ret;

	if ((ret = conn->close(conn, NULL)) != 0)
		die("conn.close", ret);
}

/*
 * shutdown --
 *	Clean up from previous runs.
 */
static void
shutdown(void)
{
	(void)system("rm -f WildTiger WiredTiger.* __wt*");
}

static int
handle_error(WT_EVENT_HANDLER *handler, int error, const char *errmsg)
{
	UNUSED(handler);
	UNUSED(error);

	/* Ignore complaints about missing files. */
	if (error == ENOENT)
		return (0);
	return (fprintf(stderr, "%s\n", errmsg) < 0 ? -1 : 0);
}

static int
handle_message(WT_EVENT_HANDLER *handler, const char *message)
{
	UNUSED(handler);

	if (logfp != NULL)
		return (fprintf(logfp, "%s\n", message) < 0 ? -1 : 0);

	return (printf("%s\n", message) < 0 ? -1 : 0);
}

/*
 * onint --
 *	Interrupt signal handler.
 */
static void
onint(int signo)
{
	UNUSED(signo);

	shutdown();

	fprintf(stderr, "\n");
	exit(EXIT_FAILURE);
}

/*
 * die --
 *	Report an error and quit.
 */
void
die(const char *m, int e)
{
	fprintf(stderr, "%s: %s: %s\n", progname, m, wiredtiger_strerror(e));
	exit(EXIT_FAILURE);
}

/*
 * usage --
 *	Display usage statement and exit failure.
 */
static int
usage(void)
{
	fprintf(stderr,
	    "usage: %s "
	    "[-S] [-C wiredtiger-config] [-k keys] [-l log]\n\t"
	    "[-n ops] [-R readers] [-r runs] [-t f|r|v] [-W writers]\n",
	    progname);
	fprintf(stderr, "%s",
	    "\t-C specify wiredtiger_open configuration arguments\n"
	    "\t-l specify a log file\n"
	    "\t-n set number of operations each thread does\n"
	    "\t-r set number of runs\n"
	    "\t-t set number of threads\n");
	return (EXIT_FAILURE);
}
