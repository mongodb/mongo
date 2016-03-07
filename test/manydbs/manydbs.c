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

#include <sys/wait.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#include <wiredtiger.h>

#include "test_util.i"

#define	HOME_SIZE	512
#define	HOME_BASE	"WT_HOME"
static char home[HOME_SIZE];		/* Base home directory */
static char hometmp[HOME_SIZE];		/* Each conn home directory */
static const char *progname;		/* Program name */

#define	ENV_CONFIG						\
    "create,log=(file_max=10M,archive=false,enabled),"

#define	MAX_CPU	1.0
#define	MAX_DBS	10

static void
usage(void)
{
	fprintf(stderr, "usage: %s [-h dir]\n", progname);
	exit(EXIT_FAILURE);
}

extern int __wt_optind;
extern char *__wt_optarg;

void (*custom_die)(void) = NULL;

int
main(int argc, char *argv[])
{
	FILE *fp;
	WT_CONNECTION **conn;
	float cpu;
	int ch;
	u_int dbs, i;
	const char *working_dir;
	char cmd[128];

	if ((progname = strrchr(argv[0], DIR_DELIM)) == NULL)
		progname = argv[0];
	else
		++progname;
	dbs = MAX_DBS;
	working_dir = HOME_BASE;
	while ((ch = __wt_getopt(progname, argc, argv, "D:h:")) != EOF)
		switch (ch) {
		case 'D':
			dbs = (u_int)atoi(__wt_optarg);
			break;
		case 'h':
			working_dir = __wt_optarg;
			break;
		default:
			usage();
		}
	argc -= __wt_optind;
	argv += __wt_optind;
	if (argc != 0)
		usage();

	if ((conn = calloc(sizeof(WT_CONNECTION *), dbs)) == NULL)
		testutil_die(ENOMEM, "connection array malloc");
	memset(cmd, 0, sizeof(cmd));
	/*
	 * Set up all the directory names.
	 */
	testutil_work_dir_from_path(home, HOME_SIZE, working_dir);
	testutil_make_work_dir(home);
	for (i = 0; i < dbs; ++i) {
		snprintf(hometmp, HOME_SIZE, "%s/%s.%d", home, HOME_BASE, i);
		testutil_make_work_dir(hometmp);
		testutil_check(wiredtiger_open(
		    hometmp, NULL, ENV_CONFIG, &conn[i]));
	}

	sleep(10);
	(void)snprintf(cmd, sizeof(cmd),
	    "ps -p %lu -o pcpu=", (unsigned long)getpid());
	if ((fp = popen(cmd, "r")) == NULL)
		testutil_die(errno, "popen");
	fscanf(fp, "%f", &cpu);
	if (cpu > MAX_CPU) {
		fprintf(stderr, "CPU usage: %f, max %f\n", cpu, MAX_CPU);
		testutil_die(ERANGE, "CPU Usage");
	}
	if (pclose(fp) != 0)
		testutil_die(errno, "pclose");

	for (i = 0; i < dbs; ++i)
		testutil_check(conn[i]->close(conn[i], NULL));

	return (EXIT_SUCCESS);
}
