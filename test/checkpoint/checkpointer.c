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
static int compare_cursors(WT_CURSOR *, table_type, WT_CURSOR *, table_type);
static int real_checkpointer(void);
static int verify_checkpoint(WT_SESSION *, const char *);

int
start_checkpoints()
{
	int ret;

	if ((ret = pthread_create(
	    &g.checkpoint_thread, NULL, checkpointer, NULL)) != 0)
		return (log_print_err("pthread_create", ret, 1));
	return (0);
}

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
	

static int
real_checkpointer()
{
	WT_SESSION *session;
	int ret;


	if (g.running == 0)
		return (log_print_err(
		    "Checkpoint thread started stopped\n", EINVAL, 1));

	while (g.ntables > g.ntables_created)
		sched_yield();

	if ((ret = g.conn->open_session(g.conn, NULL, NULL, &session)) != 0)
		return (log_print_err("conn.open_session", ret, 1));

	sleep(1);
	while (g.running) {

		/* Execute a checkpoint */
		if ((ret = session->checkpoint(session, NULL)) != 0)
			return (log_print_err("session.checkpoint", ret, 1));
		printf("Finished a checkpoint\n");

		if (!g.running)
			goto done;

		/* Verify the content of the checkpoint. */
		if ((ret = verify_checkpoint(
		    session, "WiredTigerCheckpoint")) != 0)
			return (log_print_err("verify_checkpoint", ret, 1));
	}

done:	if ((ret = session->close(session, NULL)) != 0)
		return (log_print_err("session.close", ret, 1));

	return (0);
}

static int
verify_checkpoint(WT_SESSION *session, const char *name)
{
	WT_CURSOR **cursors;
	table_type zero_type;
	char next_uri[128], ckpt[128];
	int i, ret, t_ret;
	uint64_t key_count;

	key_count = 0;
	zero_type = g.cookies[0].type;
	snprintf(ckpt, 128, "checkpoint=%s", name);
	cursors = calloc(g.ntables, sizeof(*cursors));

	for (i = 0; i < g.ntables; i++) {
		snprintf(next_uri, 128, "table:__wt%04d", i);
		if ((ret = session->open_cursor(
		    session, next_uri, NULL, ckpt, &cursors[i])) != 0)
			return (log_print_err(
			    "verify_checkpoint:session.open_cursor", ret, 1));
	}

	while (ret == 0) {
		++key_count;
		ret = cursors[0]->next(cursors[0]);
		if (ret != 0 && ret != WT_NOTFOUND)
			return (log_print_err("cursor->next", ret, 1));
		/*
		 * Check to see that all remaining cursors have the 
		 * same key/value pair.
		 */
		for (i = 1; i < g.ntables; i++) {
			t_ret = cursors[i]->next(cursors[i]);
			if (t_ret != 0 && t_ret != WT_NOTFOUND)
				return (log_print_err("cursor->next", ret, 1));

			if (ret == WT_NOTFOUND && t_ret == WT_NOTFOUND)
				continue;
			else if (ret == WT_NOTFOUND || t_ret == WT_NOTFOUND)
				return (log_print_err(
				    "verify_checkpoint tables with different"
				    " amount of data", EFAULT, 1));

			if (compare_cursors(cursors[0], zero_type,
			    cursors[i], g.cookies[i].type) != 0)
				return (log_print_err(
				    "verify_checkpoint - mismatching data",
				    EFAULT, 1));
		}
	}
	printf("Finished verifying a checkpoint with %d tables and %" PRIu64
	    " keys\n", g.ntables, key_count);
	return (0);
}

int
compare_cursors(
    WT_CURSOR *first, table_type first_type,
    WT_CURSOR *second, table_type second_type)
{
	WT_ITEM first_key, second_key;
	WT_ITEM first_value, second_value;
	u_int first_key_int, second_key_int;
	char buf[128];

	memset(buf, 0, 128);

	/*
	 * Column stores have a different format than all others, but the
	 * underlying value should still match.
	 * Copy the string out of a non-column store in that case to
	 * ensure that it's nul terminated.
	 */
	if (first_type == COL)
		first->get_key(first, &first_key_int);
	else {
		first->get_key(first, &first_key);
		memcpy(buf, first_key.data, first_key.size);
		first_key_int = atol(buf);
	}
	if (second_type == COL)
		first->get_key(first, &second_key_int);
	else {
		second->get_key(second, &second_key);
		memcpy(buf, second_key.data, second_key.size);
		second_key_int = atol(buf);
	}
	if (first_key_int != second_key_int) {
		printf("Key mismatch %" PRIu32 " from an %s table "
		    "is not %" PRIu32 " from a %s table\n",
		    first_key_int, type_to_string(first_type),
		    second_key_int, type_to_string(second_type));
		return (1);
	}

	/* Now check the values. */
	first->get_value(first, &first_value);
	second->get_value(second, &second_value);
	if (first_value.size != second_value.size)
		return (1);
	return (memcmp(
	    first_value.data, second_value.data, first_value.size));
}
