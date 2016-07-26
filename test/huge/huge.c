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

#include "test_util.h"

static char home[512];				/* Program working dir */
static const char *progname;			/* Program name */
static uint8_t *big;				/* Big key/value buffer */

#define	GIGABYTE	(1073741824)
#define	MEGABYTE	(1048576)

/*
 * List of configurations we test.
 */
typedef	struct {
	const char *uri;			/* Object URI */
	const char *config;			/* Object configuration */
	int recno;				/* Column-store key */
} CONFIG;

static CONFIG config[] = {
	{ "file:xxx", "key_format=S,value_format=S", 0 },
	{ "file:xxx", "key_format=r,value_format=S", 1 },
	{ "lsm:xxx", "key_format=S,value_format=S", 0 },
	{ "table:xxx", "key_format=S,value_format=S", 0 },
	{ "table:xxx", "key_format=r,value_format=S", 1 },
	{ NULL, NULL, 0 }
};

#define	SMALL_MAX	MEGABYTE
static size_t lengths[] = {
    20,					/* Check configuration */
    (size_t)1 * MEGABYTE,		/* 1MB (largest -s configuration) */
    (size_t)250 * MEGABYTE,		/* 250MB */
    (size_t)1 * GIGABYTE,		/* 1GB */
    (size_t)2 * GIGABYTE,		/* 2GB */
    (size_t)3 * GIGABYTE,		/* 3GB */
    ((size_t)4 * GIGABYTE) - MEGABYTE,	/* Roughly the max we can handle */
    0
};

static void usage(void)
    WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));
static void
usage(void)
{
	fprintf(stderr, "usage: %s [-s]\n", progname);
	fprintf(stderr, "%s", "\t-s small run, only test up to 1GB\n");
	exit(EXIT_FAILURE);
}

#ifndef _WIN32
#define	SIZET_FMT	"%zu"			/* size_t format string */
#else
#define	SIZET_FMT	"%Iu"			/* size_t format string */
#endif

static void
run(CONFIG *cp, int bigkey, size_t bytes)
{
	WT_CONNECTION *conn;
	WT_SESSION *session;
	WT_CURSOR *cursor;
	uint64_t keyno;
	int ret;
	void *p;

	big[bytes - 1] = '\0';

	printf(SIZET_FMT "%s%s: %s %s big %s\n",
	    bytes < MEGABYTE ? bytes :
	    (bytes < GIGABYTE ? bytes / MEGABYTE : bytes / GIGABYTE),
	    bytes < MEGABYTE ? "" :
	    (bytes < GIGABYTE ?
	    (bytes % MEGABYTE == 0 ? "" : "+") :
	    (bytes % GIGABYTE == 0 ? "" : "+")),
	    bytes < MEGABYTE ? "B" : (bytes < GIGABYTE ? "MB" : "GB"),
	    cp->uri, cp->config, bigkey ? "key" : "value");

	testutil_make_work_dir(home);

	/*
	 * Open/create the database, connection, session and cursor; set the
	 * cache size large, we don't want to try and evict anything.
	 */
	if ((ret = wiredtiger_open(
	    home, NULL, "create,cache_size=10GB", &conn)) != 0)
		testutil_die(ret, "wiredtiger_open");
	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		testutil_die(ret, "WT_CONNECTION.open_session");
	if ((ret = session->create(session, cp->uri, cp->config)) != 0)
		testutil_die(ret,
		    "WT_SESSION.create: %s %s", cp->uri, cp->config);
	if ((ret =
	    session->open_cursor(session, cp->uri, NULL, NULL, &cursor)) != 0)
		testutil_die(ret, "WT_SESSION.open_cursor: %s", cp->uri);

	/* Set the key/value. */
	if (bigkey)
		cursor->set_key(cursor, big);
	else if (cp->recno) {
		keyno = 1;
		cursor->set_key(cursor, keyno);
	} else
		cursor->set_key(cursor, "key001");
	cursor->set_value(cursor, big);

	/* Insert the record (use update, insert discards the key). */
	if ((ret = cursor->update(cursor)) != 0)
		testutil_die(ret, "WT_CURSOR.insert");

	/* Retrieve the record and check it. */
	if ((ret = cursor->search(cursor)) != 0)
		testutil_die(ret, "WT_CURSOR.search");
	if (bigkey && (ret = cursor->get_key(cursor, &p)) != 0)
		testutil_die(ret, "WT_CURSOR.get_key");
	if ((ret = cursor->get_value(cursor, &p)) != 0)
		testutil_die(ret, "WT_CURSOR.get_value");
	if (memcmp(p, big, bytes) != 0)
		testutil_die(0,
		    "retrieved big key/value item did not match original");

	/* Remove the record. */
	if ((ret = cursor->remove(cursor)) != 0)
		testutil_die(ret, "WT_CURSOR.remove");

	if ((ret = conn->close(conn, NULL)) != 0)
		testutil_die(ret, "WT_CONNECTION.close");

	big[bytes - 1] = 'a';
}

extern int __wt_optind;
extern char *__wt_optarg;

void (*custom_die)(void) = NULL;

int
main(int argc, char *argv[])
{
	CONFIG *cp;
	size_t len, *lp;
	int ch, small;
	char *working_dir;

	if ((progname = strrchr(argv[0], DIR_DELIM)) == NULL)
		progname = argv[0];
	else
		++progname;

	small = 0;
	working_dir = NULL;

	while ((ch = __wt_getopt(progname, argc, argv, "h:s")) != EOF)
		switch (ch) {
		case 'h':
			working_dir = __wt_optarg;
			break;
		case 's':			/* Gigabytes */
			small = 1;
			break;
		default:
			usage();
		}
	argc -= __wt_optind;
	argv += __wt_optind;
	if (argc != 0)
		usage();

	testutil_work_dir_from_path(home, 512, working_dir);

	/* Allocate a buffer to use. */
	len = small ? ((size_t)SMALL_MAX) : ((size_t)4 * GIGABYTE);
	big = dmalloc(len);
	memset(big, 'a', len);

	/* Make sure the configurations all work. */
	for (lp = lengths; *lp != 0; ++lp) {
		if (small && *lp > SMALL_MAX)
			break;
		for (cp = config; cp->uri != NULL; ++cp) {
			if (!cp->recno)		/* Big key on row-store */
				run(cp, 1, *lp);
			run(cp, 0, *lp);	/* Big value */
		}
	}
	free(big);

	testutil_clean_work_dir(home);

	return (EXIT_SUCCESS);
}
