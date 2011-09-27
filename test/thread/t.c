/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "thread.h"

WT_CONNECTION *conn;				/* WiredTiger connection */
__ftype ftype;					/* File type */
u_int nkeys, nops;				/* Keys, Operations */

static char *progname;				/* Program name */
static FILE *logfp;				/* Log file */

static int  handle_message(WT_EVENT_HANDLER *, const char *);
static void onint(int);
static void shutdown(void);
static int  usage(void);
static void wt_connect(char *);
static void wt_shutdown(void);

int
main(int argc, char *argv[])
{
	int ch, cnt, readers, runs, writers;
	char *config_open;

	if ((progname = strrchr(argv[0], '/')) == NULL)
		progname = argv[0];
	else
		++progname;

	config_open = NULL;
	ftype = ROW;
	nkeys = 1000;
	nops = 10000;
	readers = 10;
	runs = 0;
	writers = 10;

	while ((ch = getopt(argc, argv, "1C:k:l:n:r:t:w:")) != EOF)
		switch (ch) {
		case '1':			/* One run */
			runs = 1;
			break;
		case 'C':			/* wiredtiger_open config */
			config_open = optarg;
			break;
		case 'k':
			nkeys = (u_int)atoi(optarg);
			break;
		case 'l':
			if ((logfp = fopen(optarg, "w")) == NULL) {
				fprintf(stderr,
				    "%s: %s\n", optarg, strerror(errno));
				return (EXIT_FAILURE);
			}
			break;
		case 'n':
			nops = (u_int)atoi(optarg);
			break;
		case 'r':
			readers = atoi(optarg);
			break;
		case 't':
			switch (optarg[0]) {
			case 'f':
				ftype = FIX;
				break;
			case 'r':
				ftype = ROW;
				break;
			case 'v':
				ftype = VAR;
				break;
			default:
				return (usage());
			}
			break;
		case 'w':
			writers = atoi(optarg);
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
		printf("    %d: %d readers, %d writers\n",
		    cnt, readers, writers);

		shutdown();			/* Clean up previous runs */

		wt_connect(config_open);	/* WiredTiger connection */

		load();				/* Load initial records */
						/* Loop operations */
		if (run(readers, writers))
			return (EXIT_FAILURE);

		stats();			/* Statistics */

		wt_shutdown();			/* WiredTiger shut down */
	}
	return (0);
}

/*
 * wt_connect --
 *	Configure the WiredTiger connection.
 */
static void
wt_connect(char *config_open)
{
	static WT_EVENT_HANDLER event_handler = {
		NULL,
		handle_message,
		NULL
	};
	int ret;
	char config[128];

	snprintf(config, sizeof(config),
	    "error_prefix=\"%s\",multithread,cache_size=5MB%s%s",
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
	WT_SESSION *session;
	int ret;

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		die("conn.session", ret);

	if ((ret = session->sync(session, FNAME, NULL)) != 0)
		die("session.sync", ret);

	if ((ret = session->verify(session, FNAME, NULL)) != 0)
		die("session.sync", ret);

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
	(void)system("rm -f __schema.wt __wt*");
}

static int
handle_message(WT_EVENT_HANDLER *handler, const char *message)
{
	UNUSED(handler);

	if (logfp == NULL)
		printf("%s\n", message);
	else
		fprintf(logfp, "%s\n", message);
	return (0);
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
	exit (EXIT_FAILURE);
}

/*
 * die --
 *	Report an error and quit.
 */
void
die(const char *m, int e)
{
	fprintf(stderr, "%s: %s: %s\n", progname, m, wiredtiger_strerror(e));
	exit (EXIT_FAILURE);
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
	    "[-1] [-C wiredtiger-config] [-l log] [-r readers] [-t f|r|v] "
	    "[-w writers]\n",
	    progname);
	fprintf(stderr, "%s",
	    "\t-1 run once\n"
	    "\t-C specify wiredtiger_open configuration arguments\n"
	    "\t-k set number of keys to load\n"
	    "\t-l specify a log file\n"
	    "\t-n set number of operations each thread does\n"
	    "\t-r set number of reading threads\n"
	    "\t-t set a file type (fix | row | var)\n"
	    "\t-w set number of writing threads\n");
	return (EXIT_FAILURE);
}
