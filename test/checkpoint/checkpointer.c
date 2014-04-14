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
static int verify_checkpoint(WT_SESSION *, const char *);

void
start_checkpoints()
{
	int ret;

	g.checkpoint_phase = 1;
	if ((ret = pthread_create(
	    &g.checkpoint_thread, NULL, checkpointer, NULL)) != 0)
		die("pthread_create", ret);

}

void
end_checkpoints()
{
	void *thread_ret;

	g.checkpoint_phase = 2;
	(void)pthread_join(g.checkpoint_thread, &thread_ret);

}

/*
 * checkpointer --
 *	Checkpoint thread start function.
 */
static void *
checkpointer(void *arg)
{
	WT_SESSION *session;
	pthread_t tid;
	int ret;

	WT_UNUSED(arg);
	tid = pthread_self();
	printf("checkpointer thread starting: tid: %p\n", (void *)tid);

	while (g.ntables > g.ntables_created || g.checkpoint_phase == 0)
		sched_yield();

	if ((ret = g.conn->open_session(g.conn, NULL, NULL, &session)) != 0)
		die("conn.open_session", ret);

	if (g.checkpoint_phase == 2)
		goto done;

	/* Execute a checkpoint */
	if ((ret = session->checkpoint(session, NULL)) != 0)
		die("session.checkpoint", ret);

	if (g.checkpoint_phase == 2)
		goto done;

	/* Verify the content of the checkpoint. */
	if ((ret = verify_checkpoint(session, "WiredTigerCheckpoint")) != 0)
		die("verify_checkpoint", ret);

done:	if ((ret = session->close(session, NULL)) != 0)
		die("session.close", ret);

	return (NULL);
}

static int
verify_checkpoint(WT_SESSION *session, const char *name)
{
	WT_CURSOR **cursors;
	char next_uri[128], ckpt[128];
	char first_key[64], first_value[64];
	char curr_key[64], curr_value[64];
	int i, ret, t_ret;

	snprintf(ckpt, 128, "checkpoint=%s", name);
	cursors = calloc(g.ntables, sizeof(*cursors));
	for (i = 0; i < g.ntables; i++) {
		snprintf(next_uri, 128, "table:__wt%04d", i);
		if ((ret = session->open_cursor(
		    session, next_uri, NULL, ckpt, &cursors[i])) != 0)
			die("verify_checkpoint:session.open_cursor", ret);
	}

	while (ret == 0) {
		ret = cursors[0]->next(cursors[0]);
		if (ret == 0) {
			cursors[0]->get_key(cursors[0], &first_key);
			cursors[0]->get_value(cursors[0], &first_value);
		} else if (ret != WT_NOTFOUND)
			die("cursor->next", ret);
		/*
		 * Check to see that all remaining cursors have the 
		 * same key/value pair.
		 */
		for (i = 1; i < g.ntables; i++) {
			t_ret = cursors[i]->next(cursors[i]);
			if (t_ret != 0 && t_ret != WT_NOTFOUND)
				die("cursor->next", ret);

			if (ret == WT_NOTFOUND && t_ret == WT_NOTFOUND)
				continue;
			else if (ret == WT_NOTFOUND || t_ret == WT_NOTFOUND)
				die("verify_checkpoint tables with different"
				    " amount of data", EFAULT);

			/* Normal case - match the data */
			cursors[i]->get_key(cursors[i], &curr_key);
			cursors[i]->get_value(cursors[i], &curr_value);
			if (strcmp(first_key, curr_key) != 0 ||
			    strcmp(first_value, curr_value) != 0)
				die("verify_checkpoint - mismatching values",
				    EFAULT);
		}
	}
	return (0);
}
