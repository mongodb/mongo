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

static void *checkpointer(void *);
static int compare_cursors(WT_CURSOR *, int, WT_CURSOR *, int);
static int diagnose_key_error(WT_CURSOR *, int, WT_CURSOR *, int);
static int get_key_int(WT_CURSOR *, int, u_int *);
static int real_checkpointer(void);
static int verify_checkpoint(WT_SESSION *);

/*
 * start_checkpoints --
 *     Responsible for creating the checkpoint thread.
 */
int
start_checkpoints()
{
	int ret;

	if ((ret = pthread_create(
	    &g.checkpoint_thread, NULL, checkpointer, NULL)) != 0)
		return (log_print_err("pthread_create", ret, 1));
	return (0);
}

/*
 * end_checkpoints --
 *     Responsible for cleanly shutting down the checkpoint thread.
 */
int
end_checkpoints()
{
	void *thread_ret;

	return (pthread_join(g.checkpoint_thread, &thread_ret));

}

/*
 * checkpointer --
 *	Checkpoint thread start function.
 */
static void *
checkpointer(void *arg)
{
	pthread_t tid;

	WT_UNUSED(arg);
	tid = pthread_self();
	printf("checkpointer thread starting: tid: %p\n", (void *)tid);

	(void)real_checkpointer();
	return (NULL);
}

/*
 * real_checkpointer --
 *     Do the work of creating checkpoints and then verifying them. Also
 *     responsible for finishing in a timely fashion.
 */
static int
real_checkpointer()
{
	WT_SESSION *session;
	char *checkpoint_config, _buf[128];
	int ret;

	if (g.running == 0)
		return (log_print_err(
		    "Checkpoint thread started stopped\n", EINVAL, 1));

	while (g.ntables > g.ntables_created)
		sched_yield();

	if ((ret = g.conn->open_session(g.conn, NULL, NULL, &session)) != 0)
		return (log_print_err("conn.open_session", ret, 1));

	if (strncmp(g.checkpoint_name,
	    "WiredTigerCheckpoint", strlen("WiredTigerCheckpoint")) == 0)
		checkpoint_config = NULL;
	else {
		checkpoint_config = _buf;
		snprintf(checkpoint_config, 128, "name=%s", g.checkpoint_name);
	}
	while (g.running) {
		/* Execute a checkpoint */
		if ((ret = session->checkpoint(
		    session, checkpoint_config)) != 0)
			return (log_print_err("session.checkpoint", ret, 1));
		printf("Finished a checkpoint\n");

		if (!g.running)
			goto done;

		/* Verify the content of the checkpoint. */
		if ((ret = verify_checkpoint(session)) != 0)
			return (log_print_err("verify_checkpoint", ret, 1));
	}

done:	if ((ret = session->close(session, NULL)) != 0)
		return (log_print_err("session.close", ret, 1));

	return (0);
}

/*
 * verify_checkpoint --
 *     Open a cursor on each table at the last checkpoint and walk through
 *     the tables in parallel. The key/values should match across all
 *     tables.
 */
static int
verify_checkpoint(WT_SESSION *session)
{
	WT_CURSOR **cursors;
	char next_uri[128], ckpt[128];
	int i, ret, t_ret;
	uint64_t key_count;

	ret = t_ret = 0;
	key_count = 0;
	snprintf(ckpt, 128, "checkpoint=%s", g.checkpoint_name);
	cursors = calloc((size_t)g.ntables, sizeof(*cursors));

	for (i = 0; i < g.ntables; i++) {
		/*
		 * TODO: LSM doesn't currently support reading from
		 * checkpoints.
		 */
		if (g.cookies[i].type == LSM)
			continue;
		snprintf(next_uri, 128, "table:__wt%04d", i);
		if ((ret = session->open_cursor(
		    session, next_uri, NULL, ckpt, &cursors[i])) != 0)
			return (log_print_err(
			    "verify_checkpoint:session.open_cursor", ret, 1));
	}

	while (ret == 0) {
		ret = cursors[0]->next(cursors[0]);
		if (ret == 0)
			++key_count;
		else if (ret != WT_NOTFOUND)
			return (log_print_err("cursor->next", ret, 1));
		/*
		 * Check to see that all remaining cursors have the
		 * same key/value pair.
		 */
		for (i = 1; i < g.ntables; i++) {
			/*
			 * TODO: LSM doesn't currently support reading from
			 * checkpoints.
			 */
			if (g.cookies[i].type == LSM)
				continue;
			t_ret = cursors[i]->next(cursors[i]);
			if (t_ret != 0 && t_ret != WT_NOTFOUND)
				return (log_print_err("cursor->next", ret, 1));

			if (ret == WT_NOTFOUND && t_ret == WT_NOTFOUND)
				continue;
			else if (ret == WT_NOTFOUND || t_ret == WT_NOTFOUND)
				return (log_print_err(
				    "verify_checkpoint tables with different"
				    " amount of data", EFAULT, 1));

			if ((ret = compare_cursors(
			    cursors[0], 0, cursors[i], i)) != 0) {
				if (ret == ERR_KEY_MISMATCH)
					(void)diagnose_key_error(
					    cursors[0], 0, cursors[i], i);
				return (log_print_err(
				    "verify_checkpoint - mismatching data",
				    EFAULT, 1));
			}
		}
	}
	for (i = 0; i < g.ntables; i++) {
		if (cursors[i] != NULL &&
		    (ret = cursors[i]->close(cursors[i])) != 0)
			return (log_print_err(
			    "verify_checkpoint:cursor close", ret, 1));
	}
	printf("Finished verifying a checkpoint with %d tables and %" PRIu64
	    " keys\n", g.ntables, key_count);
	return (0);
}

/*
 * get_key_int --
 *     Column stores have a different format than all others, but the
 *     underlying value should still match. Copy the string out of a
 *     non-column store in that case to ensure that it's nul terminated.
 */
static int
get_key_int(WT_CURSOR *cursor, int table_index, u_int *rval)
{
	WT_ITEM key;
	u_int val;
	char buf[128];

	if (g.cookies[table_index].type == COL)
		cursor->get_key(cursor, &val);
	else {
		cursor->get_key(cursor, &key);
		memset(buf, 0, 128);
		memcpy(buf, key.data, key.size);
		val = (u_int)atol(buf);
	}

	*rval = val;
	return (0);
}

/*
 * compare_cursors --
 *     Compare the key/value pairs from two cursors.
 */
static int
compare_cursors(
    WT_CURSOR *first, int first_index,
    WT_CURSOR *second, int second_index)
{
	u_int first_key_int, second_key_int;
	char *first_value, *second_value;
	char buf[128];

	memset(buf, 0, 128);

	if (get_key_int(first, first_index, &first_key_int) != 0 ||
	    get_key_int(second, second_index, &second_key_int) != 0)
		return (log_print_err("Error decoding key", EINVAL, 1));

	if (first_key_int != second_key_int) {
		printf("Key mismatch %" PRIu32 " from a %s table "
		    "is not %" PRIu32 " from a %s table\n",
		    first_key_int,
		    type_to_string(g.cookies[first_index].type),
		    second_key_int,
		    type_to_string(g.cookies[second_index].type));

		return (ERR_KEY_MISMATCH);
	}

	/* Now check the values. */
	first->get_value(first, &first_value);
	second->get_value(second, &second_value);
	if (g.logfp != NULL)
		fprintf(g.logfp, "k1: %" PRIu32 " k2: %" PRIu32
		    " val1: %s val2: %s \n",
		    first_key_int, second_key_int,
		    first_value, second_value);
	if (strlen(first_value) != strlen(second_value) ||
	    strcmp(first_value, second_value) != 0) {
		printf("Value mismatch %s from a %s table "
		    "is not %s from a %s table\n",
		    first_value,
		    type_to_string(g.cookies[first_index].type),
		    second_value,
		    type_to_string(g.cookies[second_index].type));
		return (ERR_DATA_MISMATCH);
	}

	return (0);
}

/*
 * diagnose_key_error --
 *     Dig a bit deeper on failure. Continue after some failures here to
 *     extract as much information as we can.
 */
static int
diagnose_key_error(
    WT_CURSOR *first, int first_index,
    WT_CURSOR *second, int second_index)
{
	WT_CURSOR *c;
	WT_ITEM first_key, second_key;
	WT_SESSION *session;
	u_int key1i, key2i;
	char next_uri[128], ckpt[128];
	int ret;

	/* Hack to avoid passing session as parameter. */
	session = first->session;

	snprintf(ckpt, 128, "checkpoint=%s", g.checkpoint_name);

	/* Save the failed keys. */
	first->get_key(first, &first_key);
	second->get_key(second, &second_key);

	/* See if previous values are still valid. */
	if (first->prev(first) != 0 || second->prev(second) != 0)
		return (1);
	if (get_key_int(first, first_index, &key1i) != 0 ||
	    get_key_int(second, second_index, &key2i) != 0)
		log_print_err("Error decoding key", EINVAL, 1);
	else if (key1i != key2i)
		log_print_err("Now previous keys don't match", EINVAL, 0);

	if (first->next(first) != 0 || second->next(second) != 0)
		return (1);
	if (get_key_int(first, first_index, &key1i) != 0 ||
	    get_key_int(second, second_index, &key2i) != 0)
		log_print_err("Error decoding key", EINVAL, 1);
	else if (key1i == key2i)
		log_print_err("After prev/next keys match", EINVAL, 0);

	if (first->next(first) != 0 || second->next(second) != 0)
		return (1);
	if (get_key_int(first, first_index, &key1i) != 0 ||
	    get_key_int(second, second_index, &key2i) != 0)
		log_print_err("Error decoding key", EINVAL, 1);
	else if (key1i == key2i)
		log_print_err("After prev/next/next keys match", EINVAL, 0);

	/*
	 * Now try opening new cursors on the checkpoints and see if we
	 * get the same missing key via searching.
	 */
	snprintf(next_uri, 128, "table:__wt%04d", first_index);
	if (session->open_cursor(session, next_uri, NULL, ckpt, &c) != 0)
		return (1);
	c->set_key(c, &first_key);
	if ((ret = c->search(c)) != 0)
		log_print_err("1st cursor didn't find 1st key\n", ret, 0);
	if (g.cookies[first_index].type == g.cookies[second_index].type) {
		c->set_key(c, &second_key);
		if ((ret = c->search(c)) != 0)
			log_print_err(
			    "1st cursor didn't find 2nd key\n", ret, 0);
	}
	c->close(c);
	snprintf(next_uri, 128, "table:__wt%04d", second_index);
	ret = session->open_cursor(session, next_uri, NULL, ckpt, &c);
	if (g.cookies[first_index].type == g.cookies[second_index].type) {
		c->set_key(c, &first_key);
		if ((ret = c->search(c)) != 0)
			log_print_err(
			    "2nd cursor didn't find 1st key\n", ret, 0);
	}
	c->set_key(c, &second_key);
	if ((ret = c->search(c)) != 0)
		log_print_err("2nd cursor didn't find 2nd key\n", ret, 0);
	c->close(c);

	return (0);
}
