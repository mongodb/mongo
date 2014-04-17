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

static int real_worker(void);
static void *worker(void *);

/*
 * create_table --
 *     Create a WiredTiger table of the configured type for this cookie.
 */
static int
create_table(WT_SESSION *session, COOKIE *cookie)
{
	int ret;
	char *p, *end, config[128];

	p = config;
	end = config + sizeof(config);
	p += snprintf(p, (size_t)(end - p),
	    "key_format=%s,value_format=S",
	    cookie->type == COL ? "r" : "u");
	if (cookie->type == LSM)
		(void)snprintf(p, (size_t)(end - p), ",type=lsm");

	if ((ret = session->create(session, cookie->uri, config)) != 0)
		if (ret != EEXIST)
			return (log_print_err("session.create", ret, 1));
	++g.ntables_created;
	return (0);
}

/*
 * start_workers --
 *     Setup the configuration for the tables being populated, then start
 *     the worker thread(s) and wait for them to finish.
 */
int
start_workers(table_type type)
{
	COOKIE *cookies;
	WT_SESSION *session;
	struct timeval start, stop;
	double seconds;
	pthread_t *tids;
	int i, ret;
	void *thread_ret;

	/* Create statistics and thread structures. */
	if ((cookies = calloc(
	    (size_t)(g.ntables), sizeof(COOKIE))) == NULL ||
	    (tids = calloc((size_t)(g.nworkers), sizeof(*tids))) == NULL)
		return (log_print_err("calloc", errno, 1));

	if ((ret = g.conn->open_session(g.conn, NULL, NULL, &session)) != 0)
		return (log_print_err("conn.open_session", ret, 1));
	/* Setup the cookies */
	for (i = 0; i < g.ntables; ++i) {
		cookies[i].id = i;
		if (type == MIX)
			cookies[i].type = (i % MAX_TABLE_TYPE) + 1;
		else
			cookies[i].type = type;
		snprintf(cookies[i].uri, 128,
		    "%s%04d", URI_BASE, cookies[i].id);

		/* Should probably be atomic to avoid races. */
		if ((ret = create_table(session, &cookies[i])) != 0)
			return (ret);
	}

	/*
	 * Install the cookies in the global array.
	 */
	g.cookies = cookies;

	(void)gettimeofday(&start, NULL);

	/* Create threads. */
	for (i = 0; i < g.nworkers; ++i) {
		if ((ret = pthread_create(
		    &tids[i], NULL, worker, &cookies[i])) != 0)
			return (log_print_err("pthread_create", ret, 1));
	}

	/* Wait for the threads. */
	for (i = 0; i < g.nworkers; ++i)
		(void)pthread_join(tids[i], &thread_ret);

	(void)gettimeofday(&stop, NULL);
	seconds = (stop.tv_sec - start.tv_sec) +
	    (stop.tv_usec - start.tv_usec) * 1e-6;
	printf("Ran workers for: %f seconds\n", seconds);

	free(tids);

	return (0);
}

/*
 * worker_op --
 *	Write operation.
 */
static inline int
worker_op(WT_CURSOR *cursor, COOKIE *cookie, u_int keyno)
{
	WT_ITEM *key, _key, *value, _value;
	u_int new_val;
	int ret;
	char *old_val;
	char keybuf[64], valuebuf[64];

	key = &_key;
	value = &_value;

	if (cookie->type == COL)
		cursor->set_key(cursor, (uint32_t)keyno);
	else {
		key->data = keybuf;
		key->size = (uint32_t)
		    snprintf(keybuf, sizeof(keybuf), "%017u", keyno);
		cursor->set_key(cursor, key);
	}
	new_val = keyno;
	if (cursor->search(cursor) == 0) {
		cursor->get_value(cursor, &old_val);
		new_val = (u_int)atol(old_val) + 1;
	}
	/*
	 * The search cleared the key from our cursor - set it again. It would
	 * be nice if we didn't need to.
	 */
	if (cookie->type == COL)
		cursor->set_key(cursor, (uint32_t)keyno);
	else
		cursor->set_key(cursor, key);

	value->data = valuebuf;
	value->size = (uint32_t)snprintf(
	    valuebuf, sizeof(valuebuf), "%037u", new_val);
	cursor->set_value(cursor, valuebuf);
	if ((ret = cursor->insert(cursor)) != 0) {
		if (ret == WT_DEADLOCK)
			return (WT_DEADLOCK);
		return (log_print_err("cursor.insert", ret, 1));
	}
	return (0);
}

/*
 * worker --
 *	Worker thread start function.
 */
static void *
worker(void *arg)
{
	pthread_t tid;

	WT_UNUSED(arg);
	tid = pthread_self();
	printf("worker thread starting: tid: %p\n", (void *)tid);
	(void)real_worker();
	return (NULL);
}

/*
 * real_worker --
 *     A single worker thread that transactionally updates all tables with
 *     consistent values.
 */
static int
real_worker()
{
	WT_CURSOR **cursors;
	WT_SESSION *session;
	u_int i, keyno;
	int j, ret;

	if ((cursors = calloc(
	    (size_t)(g.ntables), sizeof(WT_CURSOR *))) == NULL)
		return (log_print_err("malloc", ENOMEM, 1));

	if ((ret = g.conn->open_session(
	    g.conn, NULL, "isolation=snapshot", &session)) != 0)
		return (log_print_err("conn.open_session", ret, 1));

	for (j = 0; j < g.ntables; j++)
		if ((ret = session->open_cursor(
		    session, g.cookies[j].uri, NULL, NULL, &cursors[j])) != 0)
			return (log_print_err("session.open_cursor", ret, 1));

	for (i = 0; i < g.nops && g.running; ++i, sched_yield()) {
		session->begin_transaction(session, NULL);
		keyno = __wt_random() % g.nkeys + 1;
		for (j = 0; j < g.ntables; j++) {
			if ((ret = worker_op(
			    cursors[j], &g.cookies[j], keyno)) != 0)
				break;
		}
		if (ret == 0)
			session->commit_transaction(session, NULL);
		else if (ret == WT_DEADLOCK)
			session->rollback_transaction(session, NULL);
		else {
			(void)log_print_err("worker op failed", ret, 1);
			break;
		}
	}
	free(cursors);
	if ((ret = session->close(session, NULL)) != 0)
		return (log_print_err("session.close", ret, 1));

	return (0);
}
