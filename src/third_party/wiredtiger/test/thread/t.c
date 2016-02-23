/*-
 * Public Domain 2014-2015 MongoDB, Inc.
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

#include "thread.h"

WT_CONNECTION *conn;				/* WiredTiger connection */
__ftype ftype;					/* File type */
u_int nkeys, nops;				/* Keys, Operations */
int log_print;					/* Log print per operation */
int multiple_files;				/* File per thread */
int session_per_op;				/* New session per operation */

static char *progname;				/* Program name */
static FILE *logfp;				/* Log file */

static int  handle_error(WT_EVENT_HANDLER *, WT_SESSION *, int, const char *);
static int  handle_message(WT_EVENT_HANDLER *, WT_SESSION *, const char *);
static void onint(int);
static void shutdown(void);
static int  usage(void);
static void wt_connect(char *);
static void wt_shutdown(void);

extern int __wt_optind;
extern char *__wt_optarg;

int
main(int argc, char *argv[])
{
	u_int readers, writers;
	int ch, cnt, runs;
	char *config_open;

	if ((progname = strrchr(argv[0], '/')) == NULL)
		progname = argv[0];
	else
		++progname;

	config_open = NULL;
	ftype = ROW;
	log_print = 0;
	multiple_files = 0;
	nkeys = 1000;
	nops = 10000;
	readers = 10;
	runs = 1;
	session_per_op = 0;
	writers = 10;

	while ((ch = __wt_getopt(
	    progname, argc, argv, "C:Fk:Ll:n:R:r:St:W:")) != EOF)
		switch (ch) {
		case 'C':			/* wiredtiger_open config */
			config_open = __wt_optarg;
			break;
		case 'F':			/* multiple files */
			multiple_files = 1;
			break;
		case 'k':			/* rows */
			nkeys = (u_int)atoi(__wt_optarg);
			break;
		case 'L':			/* log print per operation */
			log_print = 1;
			break;
		case 'l':			/* log */
			if ((logfp = fopen(__wt_optarg, "w")) == NULL) {
				fprintf(stderr,
				    "%s: %s\n", __wt_optarg, strerror(errno));
				return (EXIT_FAILURE);
			}
			break;
		case 'n':			/* operations */
			nops = (u_int)atoi(__wt_optarg);
			break;
		case 'R':
			readers = (u_int)atoi(__wt_optarg);
			break;
		case 'r':			/* runs */
			runs = atoi(__wt_optarg);
			break;
		case 'S':			/* new session per operation */
			session_per_op = 1;
			break;
		case 't':
			switch (__wt_optarg[0]) {
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
			writers = (u_int)atoi(__wt_optarg);
			break;
		default:
			return (usage());
		}

	argc -= __wt_optind;
	argv += __wt_optind;
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

		if (rw_start(readers, writers))	/* Loop operations */
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
		handle_error,
		handle_message,
		NULL,
		NULL	/* Close handler. */
	};
	int ret;
	char config[128];

	snprintf(config, sizeof(config),
	    "create,statistics=(all),error_prefix=\"%s\",%s%s",
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

	if ((ret = session->checkpoint(session, NULL)) != 0)
		die("session.checkpoint", ret);

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
	int ret;

	if ((ret = system("rm -f WiredTiger* wt.*")) != 0)
		die("system cleanup call failed", ret);
}

static int
handle_error(WT_EVENT_HANDLER *handler,
    WT_SESSION *session, int error, const char *errmsg)
{
	(void)(handler);
	(void)(session);
	(void)(error);

	return (fprintf(stderr, "%s\n", errmsg) < 0 ? -1 : 0);
}

static int
handle_message(WT_EVENT_HANDLER *handler,
    WT_SESSION *session, const char *message)
{
	(void)(handler);
	(void)(session);

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
	(void)(signo);

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
	    "[-FLS] [-C wiredtiger-config] [-k keys] [-l log]\n\t"
	    "[-n ops] [-R readers] [-r runs] [-t f|r|v] [-W writers]\n",
	    progname);
	fprintf(stderr, "%s",
	    "\t-C specify wiredtiger_open configuration arguments\n"
	    "\t-F create a file per thread\n"
	    "\t-k set number of keys to load\n"
	    "\t-L log print per operation\n"
	    "\t-l specify a log file\n"
	    "\t-n set number of operations each thread does\n"
	    "\t-R set number of reading threads\n"
	    "\t-r set number of runs (0 for continuous)\n"
	    "\t-S open/close a session on every operation\n"
	    "\t-t set a file type (fix | row | var)\n"
	    "\t-W set number of writing threads\n");
	return (EXIT_FAILURE);
}
