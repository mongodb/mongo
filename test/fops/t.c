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

#include "thread.h"

WT_CONNECTION *conn;				/* WiredTiger connection */
u_int nops;					/* Operations */
const char *uri;				/* Object */
const char *config;				/* Object config */

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
	static struct config {
		const char *uri;
		const char *desc;
		const char *config;
	} *cp, configs[] = {
		{ "file:__wt",	NULL, NULL },
		{ "table:__wt",	NULL, NULL },
/* Configure for a modest cache size. */
#define	LSM_CONFIG	"lsm_chunk_size=1m,lsm_merge_max=2,leaf_page_max=4k"
		{ "lsm:__wt",	NULL, LSM_CONFIG },
		{ "table:__wt",	" [lsm]", "type=lsm," LSM_CONFIG },
		{ NULL,		NULL, NULL }
	};
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

	/* Use line buffering on stdout so status updates aren't buffered. */
	(void)setvbuf(stdout, NULL, _IOLBF, 0);

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
		NULL
	};
	int ret;
	char config_buf[128];

	snprintf(config_buf, sizeof(config_buf),
	    "create,error_prefix=\"%s\",cache_size=5MB%s%s",
	    progname,
	    config_open == NULL ? "" : ",",
	    config_open == NULL ? "" : config_open);

	if ((ret = wiredtiger_open(
	    NULL, &event_handler, config_buf, &conn)) != 0)
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
	(void)system("rm -f WiredTiger* __wt*");
}

static int
handle_error(WT_EVENT_HANDLER *handler, int error, const char *errmsg)
{
	UNUSED(handler);
	UNUSED(error);

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
	    "[-C wiredtiger-config] [-l log] [-n ops] [-r runs] [-t threads]\n",
	    progname);
	fprintf(stderr, "%s",
	    "\t-C specify wiredtiger_open configuration arguments\n"
	    "\t-l specify a log file\n"
	    "\t-n set number of operations each thread does\n"
	    "\t-r set number of runs\n"
	    "\t-t set number of threads\n");
	return (EXIT_FAILURE);
}
