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

/*
 * JIRA ticket reference: WT-2853
 *
 * Test case description: create two threads: one is populating/updating
 * records in a table with a few indices, the other is reading from table and
 * indices.  The test is adapted from one that uses cursor joins, this test
 * does not, but simulates some of the access patterns.
 *
 * Failure mode: after a second or two of progress by both threads, they both
 * appear to slow dramatically, almost locking up.  After some time (I've
 * observed from a half minute to a few minutes), the lock up ends and both
 * threads seem to be inserting and reading at a normal fast pace.  That
 * continues until the test ends (~30 seconds).
 */

void (*custom_die)(void) = NULL;

static void *thread_insert(void *);
static void *thread_get(void *);

#define	BLOOM		false
#define	N_RECORDS	10000
#define	N_INSERT	1000000
#define	N_INSERT_THREAD	1
#define	N_GET_THREAD	1
#define	S64 "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789::"
#define	S1024 (S64 S64 S64 S64 S64 S64 S64 S64 S64 S64 S64 S64 S64 S64 S64 S64)

typedef struct {
	char posturi[256];
	char baluri[256];
	char flaguri[256];
	bool bloom;
} SHARED_OPTS;

typedef struct {
	TEST_OPTS *testopts;
	SHARED_OPTS *sharedopts;
	int threadnum;
	int nthread;
	int done;
	int njoins;
	int nfail;
} THREAD_ARGS;

int
main(int argc, char *argv[])
{
	SHARED_OPTS *sharedopts, _sharedopts;
	TEST_OPTS *opts, _opts;
	THREAD_ARGS get_args[N_GET_THREAD], insert_args[N_INSERT_THREAD];
	WT_CURSOR *maincur;
	WT_SESSION *session;
	pthread_t get_tid[N_GET_THREAD], insert_tid[N_INSERT_THREAD];
	int i, nfail;
	const char *tablename;

	opts = &_opts;
	sharedopts = &_sharedopts;

	if (testutil_disable_long_tests())
		return (0);
	memset(opts, 0, sizeof(*opts));
	memset(sharedopts, 0, sizeof(*sharedopts));
	memset(insert_args, 0, sizeof(insert_args));
	memset(get_args, 0, sizeof(get_args));
	nfail = 0;

	sharedopts->bloom = BLOOM;
	testutil_check(testutil_parse_opts(argc, argv, opts));
	testutil_make_work_dir(opts->home);

	testutil_check(wiredtiger_open(opts->home, NULL,
	    "create,cache_size=1G", &opts->conn));

	testutil_check(
	    opts->conn->open_session(opts->conn, NULL, NULL, &session));

	/*
	 * Note: id is repeated as id2.  This makes it easier to
	 * identify the primary key in dumps of the index files.
	 */
	testutil_check(session->create(session, opts->uri,
	    "key_format=i,value_format=iiSii,"
	    "columns=(id,post,bal,extra,flag,id2)"));

	tablename = strchr(opts->uri, ':');
	testutil_assert(tablename != NULL);
	tablename++;
	snprintf(sharedopts->posturi, sizeof(sharedopts->posturi),
	    "index:%s:post", tablename);
	snprintf(sharedopts->baluri, sizeof(sharedopts->baluri),
	    "index:%s:bal", tablename);
	snprintf(sharedopts->flaguri, sizeof(sharedopts->flaguri),
	    "index:%s:flag", tablename);

	testutil_check(session->create(session, sharedopts->posturi,
	    "columns=(post)"));
	testutil_check(session->create(session, sharedopts->baluri,
	    "columns=(bal)"));
	testutil_check(session->create(session, sharedopts->flaguri,
	    "columns=(flag)"));

	/*
	 * Insert a single record with all items we need to
	 * call search() on, this makes our join logic easier.
	 */
	testutil_check(session->open_cursor(session, opts->uri, NULL, NULL,
	    &maincur));
	maincur->set_key(maincur, N_RECORDS);
	maincur->set_value(maincur, 54321, 0, "", 0, N_RECORDS);
	testutil_check(maincur->insert(maincur));
	testutil_check(maincur->close(maincur));
	testutil_check(session->close(session, NULL));

	for (i = 0; i < N_INSERT_THREAD; ++i) {
		insert_args[i].threadnum = i;
		insert_args[i].nthread = N_INSERT_THREAD;
		insert_args[i].testopts = opts;
		insert_args[i].sharedopts = sharedopts;
		testutil_check(pthread_create(&insert_tid[i], NULL,
		    thread_insert, (void *)&insert_args[i]));
	}

	for (i = 0; i < N_GET_THREAD; ++i) {
		get_args[i].threadnum = i;
		get_args[i].nthread = N_GET_THREAD;
		get_args[i].testopts = opts;
		get_args[i].sharedopts = sharedopts;
		testutil_check(pthread_create(&get_tid[i], NULL,
		    thread_get, (void *)&get_args[i]));
	}

	/*
	 * Wait for insert threads to finish.  When they
	 * are done, signal get threads to complete.
	 */
	for (i = 0; i < N_INSERT_THREAD; ++i)
		testutil_check(pthread_join(insert_tid[i], NULL));

	for (i = 0; i < N_GET_THREAD; ++i)
		get_args[i].done = 1;

	for (i = 0; i < N_GET_THREAD; ++i)
		testutil_check(pthread_join(get_tid[i], NULL));

	fprintf(stderr, "\n");
	for (i = 0; i < N_GET_THREAD; ++i) {
		fprintf(stderr, "  thread %d did %d joins (%d fails)\n", i,
		    get_args[i].njoins, get_args[i].nfail);
		nfail += get_args[i].nfail;
	}

	testutil_assert(nfail == 0);
	testutil_cleanup(opts);

	return (0);
}

static void *
thread_insert(void *arg)
{
	TEST_OPTS *opts;
	THREAD_ARGS *threadargs;
	WT_CURSOR *maincur;
	WT_RAND_STATE rnd;
	WT_SESSION *session;
	double elapsed;
	time_t prevtime, curtime; /* 1 second resolution is okay */
	int bal, i, flag, key, post;
	const char *extra = S1024;

	threadargs = (THREAD_ARGS *)arg;
	opts = threadargs->testopts;
	testutil_check(__wt_random_init_seed(NULL, &rnd));
	(void)time(&prevtime);

	testutil_check(opts->conn->open_session(
	    opts->conn, NULL, NULL, &session));

	testutil_check(session->open_cursor(session, opts->uri, NULL, NULL,
	    &maincur));

	for (i = 0; i < N_INSERT; i++) {
		/*
		 * Insert threads may stomp on each other's records;
		 * that's okay.
		 */
		key = (int)(__wt_random(&rnd) % N_RECORDS);
		testutil_check(session->begin_transaction(session, NULL));
		maincur->set_key(maincur, key);
		if (__wt_random(&rnd) % 2 == 0)
			post = 54321;
		else
			post = i % 100000;
		if (__wt_random(&rnd) % 2 == 0) {
			bal = -100;
			flag = 1;
		} else {
			bal = 100 * (i + 1);
			flag = 0;
		}
		maincur->set_value(maincur, post, bal, extra, flag, key);
		testutil_check(maincur->insert(maincur));
		testutil_check(maincur->reset(maincur));
		testutil_check(session->commit_transaction(session, NULL));
		if (i % 1000 == 0 && i != 0) {
			if (i % 10000 == 0)
				fprintf(stderr, "*");
			else
				fprintf(stderr, ".");
			(void)time(&curtime);
			if ((elapsed = difftime(curtime, prevtime)) > 5.0) {
				fprintf(stderr, "\n"
				    "GAP: %.0f secs after %d inserts\n",
				    elapsed, i);
				threadargs->nfail++;
			}
			prevtime = curtime;
		}
	}
	testutil_check(maincur->close(maincur));
	testutil_check(session->close(session, NULL));
	return (NULL);
}

static void *
thread_get(void *arg)
{
	SHARED_OPTS *sharedopts;
	TEST_OPTS *opts;
	THREAD_ARGS *threadargs;
	WT_CURSOR *maincur, *postcur;
	WT_SESSION *session;
	double elapsed;
	time_t prevtime, curtime; /* 1 second resolution is okay */
	int bal, flag, key, key2, post, bal2, flag2, post2;
	char *extra;

	threadargs = (THREAD_ARGS *)arg;
	opts = threadargs->testopts;
	sharedopts = threadargs->sharedopts;
	(void)time(&prevtime);

	testutil_check(opts->conn->open_session(
	    opts->conn, NULL, NULL, &session));
	testutil_check(session->open_cursor(session, opts->uri, NULL, NULL,
	    &maincur));

	testutil_check(session->open_cursor(
	    session, sharedopts->posturi, NULL, NULL, &postcur));

	for (threadargs->njoins = 0; threadargs->done == 0;
	     threadargs->njoins++) {
		testutil_check(session->begin_transaction(session, NULL));
		postcur->set_key(postcur, 54321);
		testutil_check(postcur->search(postcur));
		while (postcur->next(postcur) == 0) {
			testutil_check(postcur->get_key(postcur, &post));
			testutil_check(postcur->get_value(postcur, &post2,
			    &bal, &extra, &flag, &key));
			testutil_assert(post == post2);
			if (post != 54321)
				break;

			maincur->set_key(maincur, key);
			testutil_check(maincur->search(maincur));
			testutil_check(maincur->get_value(maincur, &post2,
			    &bal2, &extra, &flag2, &key2));
			testutil_check(maincur->reset(maincur));
			testutil_assert(key == key2);
			testutil_assert(post == post2);
			testutil_assert(bal == bal2);
			testutil_assert(flag == flag2);

			testutil_assert((flag2 > 0 && bal2 < 0) ||
			    (flag2 == 0 && bal2 >= 0));
		}
		/*
		 * Reset the cursors, potentially allowing the insert
		 * threads to proceed.
		 */
		testutil_check(postcur->reset(postcur));
		if (threadargs->njoins % 100 == 0)
			fprintf(stderr, "G");
		testutil_check(session->rollback_transaction(session, NULL));

		(void)time(&curtime);
		if ((elapsed = difftime(curtime, prevtime)) > 5.0) {
			fprintf(stderr, "\n"
			    "GAP: %.0f secs after %d gets\n",
			    elapsed, threadargs->njoins);
			threadargs->nfail++;
		}
		prevtime = curtime;
	}
	testutil_check(postcur->close(postcur));
	testutil_check(maincur->close(maincur));
	testutil_check(session->close(session, NULL));
	return (NULL);
}
