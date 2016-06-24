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

#include "thread.h"

WT_CONNECTION *conn;				/* WiredTiger connection */
pthread_rwlock_t single;			/* Single thread */
u_int nops;					/* Operations */
const char *uri;				/* Object */
const char *config;				/* Object config */

static char *progname;				/* Program name */
static FILE *logfp;				/* Log file */

static char home[512];

static int  handle_error(WT_EVENT_HANDLER *, WT_SESSION *, int, const char *);
static int  handle_message(WT_EVENT_HANDLER *, WT_SESSION *, const char *);
static void onint(int)
    WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));
static void shutdown(void);
static int  usage(void);
static void wt_startup(char *);
static void wt_shutdown(void);

extern int __wt_optind;
extern char *__wt_optarg;

void (*custom_die)(void) = NULL;

int
main(int argc, char *argv[])
{
	static struct config {
		const char *uri;
		const char *desc;
		const char *config;
	} *cp, configs[] = {
		{ "file:wt",	NULL, NULL },
		{ "table:wt",	NULL, NULL },
/* Configure for a modest cache size. */
#define	LSM_CONFIG	"lsm=(chunk_size=1m,merge_max=2),leaf_page_max=4k"
		{ "lsm:wt",	NULL, LSM_CONFIG },
		{ "table:wt",	" [lsm]", "type=lsm," LSM_CONFIG },
		{ NULL,		NULL, NULL }
	};
	u_int nthreads;
	int ch, cnt, ret, runs;
	char *config_open, *working_dir;

	working_dir = NULL;

	/* Remove directories */
	if ((progname = strrchr(argv[0], DIR_DELIM)) == NULL)
		progname = argv[0];
	else
		++progname;

	if ((ret = pthread_rwlock_init(&single, NULL)) != 0)
		testutil_die(ret, "pthread_rwlock_init: single");

	config_open = NULL;
	nops = 1000;
	nthreads = 10;
	runs = 1;

	while ((ch = __wt_getopt(progname, argc, argv, "C:h:l:n:r:t:")) != EOF)
		switch (ch) {
		case 'C':			/* wiredtiger_open config */
			config_open = __wt_optarg;
			break;
		case 'h':
			working_dir = __wt_optarg;
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
		case 'r':			/* runs */
			runs = atoi(__wt_optarg);
			break;
		case 't':
			nthreads = (u_int)atoi(__wt_optarg);
			break;
		default:
			return (usage());
		}

	argc -= __wt_optind;
	argv += __wt_optind;
	if (argc != 0)
		return (usage());

	testutil_work_dir_from_path(home, 512, working_dir);

	/* Clean up on signal. */
	(void)signal(SIGINT, onint);

	printf("%s: process %" PRIu64 "\n", progname, (uint64_t)getpid());
	for (cnt = 1; runs == 0 || cnt <= runs; ++cnt) {
		shutdown();			/* Clean up previous runs */

		for (cp = configs; cp->uri != NULL; ++cp) {
			uri = cp->uri;
			config = cp->config;
			printf("%5d: %u threads on %s%s\n", cnt, nthreads, uri,
			    cp->desc == NULL ? "" : cp->desc);

			wt_startup(config_open);

			if (fop_start(nthreads))
				return (EXIT_FAILURE);

			wt_shutdown();
			printf("\n");
		}
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
		NULL,
		NULL	/* Close handler. */
	};
	int ret;
	char config_buf[128];

	testutil_make_work_dir(home);

	snprintf(config_buf, sizeof(config_buf),
	    "create,error_prefix=\"%s\",cache_size=5MB%s%s",
	    progname,
	    config_open == NULL ? "" : ",",
	    config_open == NULL ? "" : config_open);
	if ((ret = wiredtiger_open(
	    home, &event_handler, config_buf, &conn)) != 0)
		testutil_die(ret, "wiredtiger_open");
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
		testutil_die(ret, "conn.close");
}

/*
 * shutdown --
 *	Clean up from previous runs.
 */
static void
shutdown(void)
{
	testutil_clean_work_dir(home);
}

static int
handle_error(WT_EVENT_HANDLER *handler,
    WT_SESSION *session, int error, const char *errmsg)
{
	(void)(handler);
	(void)(session);
	(void)(error);

	/* Ignore complaints about missing files. */
	if (error == ENOENT)
		return (0);

	/* Ignore complaints about failure to open bulk cursors. */
	if (strstr(
	    errmsg, "bulk-load is only supported on newly created") != NULL)
		return (0);

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
 * usage --
 *	Display usage statement and exit failure.
 */
static int
usage(void)
{
	fprintf(stderr,
	    "usage: %s "
	    "[-C wiredtiger-config] [-l log] [-n ops] [-r runs] [-t threads]\n",
	    progname);
	fprintf(stderr, "%s",
	    "\t-C specify wiredtiger_open configuration arguments\n"
	    "\t-h home (default 'WT_TEST')\n"
	    "\t-l specify a log file\n"
	    "\t-n set number of operations each thread does\n"
	    "\t-r set number of runs\n"
	    "\t-t set number of threads\n");
	return (EXIT_FAILURE);
}
