/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "thread.h"

WT_CONNECTION *conn;				/* WiredTiger connection */
__ftype ftype;					/* File type */
u_int nkeys, nops;				/* Keys, Operations */
int session_per_op;				/* New session per operation */

static char *progname;				/* Program name */
static FILE *logfp;				/* Log file */

static int  handle_error(WT_EVENT_HANDLER *, int, const char *);
static int  handle_message(WT_EVENT_HANDLER *, const char *);
static void onint(int);
static void shutdown(void);
static int  usage(void);
static void wt_connect(char *);
static void wt_shutdown(void);

int
main(int argc, char *argv[])
{
	u_int readers, writers;
	int ch, cnt, fileops, runs;
	char *config_open;

	if ((progname = strrchr(argv[0], '/')) == NULL)
		progname = argv[0];
	else
		++progname;

	config_open = NULL;
	fileops = 0;
	ftype = ROW;
	nkeys = 1000;
	nops = 10000;
	readers = 10;
	runs = 0;
	session_per_op = 0;
	writers = 10;

	while ((ch = getopt(argc, argv, "1C:fk:l:n:R:r:St:W:")) != EOF)
		switch (ch) {
		case '1':			/* One run */
			runs = 1;
			break;
		case 'C':			/* wiredtiger_open config */
			config_open = optarg;
			break;
		case 'f':			/* file operations */
			fileops = 1;
			nops = 1000;
			break;
		case 'k':			/* rows */
			nkeys = (u_int)atoi(optarg);
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
		case 'R':
			readers = (u_int)atoi(optarg);
			break;
		case 'r':			/* runs */
			runs = atoi(optarg);
			break;
		case 'S':			/* new session per operation */
			session_per_op = 1;
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
		case 'W':
			writers = (u_int)atoi(optarg);
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
		printf(
		    "    %d: %u readers, %u writers\n", cnt, readers, writers);

		shutdown();			/* Clean up previous runs */

		wt_connect(config_open);	/* WiredTiger connection */

		if (fileops) {
			if (fop_start(readers + writers))
				return (EXIT_FAILURE);
		} else {
			load();			/* Load initial records */
						/* Loop operations */
			if (rw_start(readers, writers))
				return (EXIT_FAILURE);

			stats();		/* Statistics */
		}

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
	WT_SESSION *session;
	int ret;

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		die("conn.session", ret);

	if ((ret = session->verify(session, FNAME, NULL)) != 0)
		die("session.verify", ret);

	if ((ret = session->sync(session, FNAME, NULL)) != 0)
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
	(void)system("rm -f WildTiger WiredTiger.* __wt*");
}

static int
handle_error(WT_EVENT_HANDLER *handler, int error, const char *errmsg)
{
	UNUSED(handler);
	UNUSED(error);

	/* Ignore complaints about truncation of missing files. */
	if (strcmp(errmsg,
	    "session.truncate: __wt: No such file or directory") == 0)
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
	    "[-1S] [-C wiredtiger-config] [-k keys] [-l log]\n\t"
	    "[-n ops] [-R readers] [-r runs] [-t f|r|v] [-W writers]\n",
	    progname);
	fprintf(stderr, "%s",
	    "\t-1 run once\n"
	    "\t-C specify wiredtiger_open configuration arguments\n"
	    "\t-f file operations instead of read/write operations\n"
	    "\t-k set number of keys to load\n"
	    "\t-l specify a log file\n"
	    "\t-n set number of operations each thread does\n"
	    "\t-R set number of reading threads\n"
	    "\t-r set number of runs\n"
	    "\t-S open/close a session on every operation\n"
	    "\t-t set a file type (fix | row | var)\n"
	    "\t-W set number of writing threads\n");
	return (EXIT_FAILURE);
}
