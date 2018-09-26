/*-
 * Public Domain 2014-2018 MongoDB, Inc.
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

#include <signal.h>

#define	MAXKEY		100000
#define	MAXURI		1
#define	PERIOD		60
#define	UTHREADS	1

static WT_CONNECTION *conn;
static uint64_t update, update_busy, verify, verify_busy;
static char const *uri[] = { "file:1", "file:2", "file:3", "file:4", "file:5" };
static bool done;

static void
uri_init(void)
{
	WT_CURSOR *cursor;
	WT_SESSION *session;
	u_int i, key;
	char buf[128];

	printf("initializing ");
	testutil_check(conn->open_session(conn, NULL, NULL, &session));
	for (i = 0; i < MAXURI; ++i) {
		printf("%s ", uri[i]);
		fflush(stdout);
		testutil_check(__wt_snprintf(buf, sizeof(buf),
		    "key_format=S,value_format=S,"
		    "allocation_size=4K,leaf_page_max=32KB,"));
		testutil_check(session->create(session, uri[i], buf));
		testutil_check(session->open_cursor(
		    session, uri[i], NULL, NULL, &cursor));
		for (key = 1; key < MAXKEY; ++key) {
			testutil_check(__wt_snprintf(
			    buf, sizeof(buf), "key:%020u", key));
			cursor->set_key(cursor, buf);
			cursor->set_value(cursor, buf);
			testutil_check(cursor->insert(cursor));
		}
		testutil_check(cursor->close(cursor));
	}
	testutil_check(session->close(session, NULL));
	printf("\n");
}

static void *
uthread(void *arg)
{
	WT_CURSOR *cursor;
	WT_DECL_RET;
	WT_SESSION *session;
	u_int i, key;
	char buf[128];

	(void)arg;

	testutil_check(conn->open_session(conn, NULL, NULL, &session));

	while (!done)
		for (i = 0; i < MAXURI; ++i) {
			for (;;) {
				__wt_yield();
				ret = session->open_cursor(
				    session, uri[i], NULL, NULL, &cursor);
				if (ret != EBUSY) {
					testutil_check(ret);
					break;
				}
				(void)__wt_atomic_add64(&update_busy, 1);
			}
			for (key = 1; key < MAXKEY; ++key) {
				testutil_check(__wt_snprintf(
				    buf, sizeof(buf), "key:%020u", key));
				cursor->set_key(cursor, buf);
				cursor->set_value(cursor, buf);
				testutil_check(cursor->insert(cursor));
				__wt_yield();
			}
			testutil_check(cursor->close(cursor));
			(void)__wt_atomic_add64(&update, 1);
		}
	return (NULL);
}

static void *
vthread(void *arg)
{
	WT_DECL_RET;
	WT_SESSION *session;
	u_int i;

	(void)arg;

	testutil_check(conn->open_session(conn, NULL, NULL, &session));
	while (!done)
		for (i = 0; i < MAXURI; ++i) {
			__wt_yield();
			ret = session->verify(session, uri[i], NULL);
			if (ret == EBUSY) {
				(void)__wt_atomic_add64(&verify_busy, 1);
				continue;
			}
			testutil_check(ret);
			(void)__wt_atomic_add64(&verify, 1);
			(void)sleep(1);
		}
	return (NULL);
}

static void
on_alarm(int signo)
{
	(void)signo;				/* Unused parameter */

	done = true;
}

static void
run(bool config_cache)
{
	pthread_t idlist[1000];
	u_int i, j;
	char buf[256], home[256];

	testutil_work_dir_from_path(
	    home, sizeof(home), "WT_TEST.wt4333_handle_locks");
	testutil_make_work_dir(home);

	testutil_check(__wt_snprintf(buf, sizeof(buf),
	    "create,"
	    "cache_size=5GB,"
	    "cache_cursors=%s,"
	    "eviction=(threads_max=5),",
	    config_cache ? "true" : "false"));
	testutil_check(wiredtiger_open(home, NULL, buf, &conn));

	printf("%s: running for %d seconds, cache_cursors=%s\n",
	    progname, PERIOD, config_cache ? "true" : "false");

	uri_init();

	printf("starting update threads\n");
	for (i = 0; i < UTHREADS; ++i)
		testutil_check(pthread_create(&idlist[i], NULL, uthread, NULL));

	printf("starting verify thread\n");
	testutil_check(pthread_create(&idlist[i], NULL, vthread, NULL));
	++i;

	alarm(PERIOD);

	for (j = 0; j < i; ++j)
		testutil_check(pthread_join(idlist[j], NULL));

	printf(
	    "update %" PRIu64
	    ", update_busy %" PRIu64
	    ", verify %" PRIu64
	    ", verify_busy %" PRIu64
	    "\n",
	    update, update_busy, verify, verify_busy);

	testutil_check(conn->close(conn, NULL));
}

int
main(int argc, char *argv[])
{
	(void)argc;					/* Unused variable */

	/* Ignore unless requested */
	if (!testutil_is_flag_set("TESTUTIL_ENABLE_LONG_TESTS"))
		return (EXIT_SUCCESS);

	(void)testutil_set_progname(argv);

	(void)signal(SIGALRM, on_alarm);

	done = false;
	run(true);
	done = false;
	run(false);

	return (EXIT_SUCCESS);
}
