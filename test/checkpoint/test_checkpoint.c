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

#include "test_checkpoint.h"

GLOBAL g;

static int  handle_error(WT_EVENT_HANDLER *, WT_SESSION *, int, const char *);
static int  handle_message(WT_EVENT_HANDLER *, WT_SESSION *, const char *);
static void onint(int);
static void path_setup(const char *);
static void cleanup(void);
static int  usage(void);
static void wt_connect(char *);
static void wt_shutdown(void);

int
main(int argc, char *argv[])
{
	u_int workers;
	table_type ttype;
	int ch, cnt, runs;
	char *config_open, *home;

	if ((g.progname = strrchr(argv[0], '/')) == NULL)
		g.progname = argv[0];
	else
		++g.progname;

	config_open = NULL;
	home = NULL;
	ttype = MIX;
	g.nkeys = 1000;
	g.nops = 10000;
	runs = 1;
	workers = 10;

	while ((ch = getopt(argc, argv, "C:h:k:l:n:r:t:W:")) != EOF)
		switch (ch) {
		case 'C':			/* wiredtiger_open config */
			config_open = optarg;
			break;
		case 'h':			/* wiredtiger_open config */
			home = optarg;
			break;
		case 'k':			/* rows */
			g.nkeys = (u_int)atoi(optarg);
			break;
		case 'l':			/* log */
			if ((g.logfp = fopen(optarg, "w")) == NULL) {
				fprintf(stderr,
				    "%s: %s\n", optarg, strerror(errno));
				return (EXIT_FAILURE);
			}
			break;
		case 'n':			/* operations */
			g.nops = (u_int)atoi(optarg);
			break;
		case 'r':			/* runs */
			runs = atoi(optarg);
			break;
		case 't':
			switch (optarg[0]) {
			case 'c':
				ttype = COL;
				break;
			case 'l':
				ttype = LSM;
				break;
			case 'm':
				ttype = MIX;
				break;
			case 'r':
				ttype = ROW;
				break;
			default:
				return (usage());
			}
			break;
		case 'W':
			workers = (u_int)atoi(optarg);
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

	path_setup(home);

	printf("%s: process %" PRIu64 "\n", g.progname, (uint64_t)getpid());
	for (cnt = 1; runs == 0 || cnt <= runs; ++cnt) {
		printf(
		    "    %d: %u workers\n", cnt, workers);

		cleanup();			/* Clean up previous runs */

		wt_connect(config_open);	/* WiredTiger connection */

						/* Loop operations */
		if (start_workers(workers, ttype))
			return (EXIT_FAILURE);

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
	    "create,statistics=(fast),error_prefix=\"%s\",cache_size=1GB%s%s",
	    g.progname,
	    config_open == NULL ? "" : ",",
	    config_open == NULL ? "" : config_open);

	if ((ret = wiredtiger_open(
	    g.home, &event_handler, config, &g.conn)) != 0)
		die("wiredtiger_open", ret);
}

/*
 * wt_shutdown --
 *	Shut down the WiredTiger connection.
 */
static void
wt_shutdown(void)
{
	int ret;

	printf("Closing connection\n");
	if ((ret = g.conn->close(g.conn, NULL)) != 0)
		die("conn.close", ret);
}

/*
 * cleanup --
 *	Clean up from previous runs.
 */
static void
cleanup(void)
{
	WT_UNUSED_RET(system(g.home_init));
}

static int
handle_error(WT_EVENT_HANDLER *handler,
    WT_SESSION *session, int error, const char *errmsg)
{
	WT_UNUSED(handler);
	WT_UNUSED(session);
	WT_UNUSED(error);

	return (fprintf(stderr, "%s\n", errmsg) < 0 ? -1 : 0);
}

static int
handle_message(WT_EVENT_HANDLER *handler,
    WT_SESSION *session, const char *message)
{
	WT_UNUSED(handler);
	WT_UNUSED(session);

	if (g.logfp != NULL)
		return (fprintf(g.logfp, "%s\n", message) < 0 ? -1 : 0);

	return (printf("%s\n", message) < 0 ? -1 : 0);
}

/*
 * onint --
 *	Interrupt signal handler.
 */
static void
onint(int signo)
{
	WT_UNUSED(signo);

	cleanup();

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
	fprintf(stderr, "%s: %s: %s\n", g.progname, m, wiredtiger_strerror(e));
	exit(EXIT_FAILURE);
}

/*
 * path_setup --
 *	Build the standard paths and shell commands we use.
 */
void
path_setup(const char *home)
{
	size_t len;

	/* Home directory. */
	if ((g.home = strdup(home == NULL ? "WT_TEST" : home)) == NULL)
		die("malloc", ENOMEM);

	/* Home directory initialize command: remove everything */
#undef	CMD
#define	CMD	"mkdir -p %s && cd %s && rm -rf `ls`"
	len = (strlen(g.home) * 2) + strlen(CMD) + 1;
	if ((g.home_init = malloc(len)) == NULL)
		die("malloc", ENOMEM);
	snprintf(g.home_init, len, CMD, g.home, g.home);
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
	    "[-n ops] [-r runs] [-t f|r|v] [-W workers]\n",
	    g.progname);
	fprintf(stderr, "%s",
	    "\t-C specify wiredtiger_open configuration arguments\n"
	    "\t-k set number of keys to load\n"
	    "\t-l specify a log file\n"
	    "\t-n set number of operations each thread does\n"
	    "\t-r set number of runs (0 for continuous)\n"
	    "\t-t set a file type ( col | mix | row | lsm )\n"
	    "\t-W set number of worker threads\n");
	return (EXIT_FAILURE);
}
