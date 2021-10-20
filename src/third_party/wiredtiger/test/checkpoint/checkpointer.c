/*-
 * Public Domain 2014-present MongoDB, Inc.
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

static WT_THREAD_RET checkpointer(void *);
static int compare_cursors(WT_CURSOR *, const char *, WT_CURSOR *, const char *);
static int diagnose_key_error(WT_CURSOR *, int, WT_CURSOR *, int);
static int real_checkpointer(void);
static int verify_checkpoint(WT_SESSION *);

/*
 * start_checkpoints --
 *     Responsible for creating the checkpoint thread.
 */
void
start_checkpoints(void)
{
    testutil_check(__wt_thread_create(NULL, &g.checkpoint_thread, checkpointer, NULL));
}

/*
 * end_checkpoints --
 *     Responsible for cleanly shutting down the checkpoint thread.
 */
void
end_checkpoints(void)
{
    testutil_check(__wt_thread_join(NULL, &g.checkpoint_thread));
}

/*
 * checkpointer --
 *     Checkpoint thread start function.
 */
static WT_THREAD_RET
checkpointer(void *arg)
{
    char tid[128];

    WT_UNUSED(arg);

    testutil_check(__wt_thread_str(tid, sizeof(tid)));
    printf("checkpointer thread starting: tid: %s\n", tid);

    (void)real_checkpointer();
    return (WT_THREAD_RET_VALUE);
}

/*
 * real_checkpointer --
 *     Do the work of creating checkpoints and then verifying them. Also responsible for finishing
 *     in a timely fashion.
 */
static int
real_checkpointer(void)
{
    WT_SESSION *session;
    int ret;
    char buf[128], *checkpoint_config;

    if (g.running == 0)
        return (log_print_err("Checkpoint thread started stopped\n", EINVAL, 1));

    while (g.ntables > g.ntables_created)
        __wt_yield();

    if ((ret = g.conn->open_session(g.conn, NULL, NULL, &session)) != 0)
        return (log_print_err("conn.open_session", ret, 1));

    if (WT_PREFIX_MATCH(g.checkpoint_name, "WiredTigerCheckpoint"))
        checkpoint_config = NULL;
    else {
        testutil_check(__wt_snprintf(buf, sizeof(buf), "name=%s", g.checkpoint_name));
        checkpoint_config = buf;
    }

    while (g.running) {
        /* Execute a checkpoint */
        if ((ret = session->checkpoint(session, checkpoint_config)) != 0)
            return (log_print_err("session.checkpoint", ret, 1));
        printf("Finished a checkpoint\n");

        if (!g.running)
            goto done;

        /* Verify the content of the checkpoint. */
        if ((ret = verify_checkpoint(session)) != 0)
            return (log_print_err("verify_checkpoint", ret, 1));
    }

done:
    if ((ret = session->close(session, NULL)) != 0)
        return (log_print_err("session.close", ret, 1));

    return (0);
}

/*
 * verify_checkpoint --
 *     Open a cursor on each table at the last checkpoint and walk through the tables in parallel.
 *     The key/values should match across all tables.
 */
static int
verify_checkpoint(WT_SESSION *session)
{
    WT_CURSOR **cursors;
    uint64_t key_count;
    int i, ret, t_ret;
    char ckpt[128], next_uri[128];
    const char *type0, *typei;

    ret = t_ret = 0;
    key_count = 0;
    testutil_check(__wt_snprintf(ckpt, sizeof(ckpt), "checkpoint=%s", g.checkpoint_name));
    cursors = calloc((size_t)g.ntables, sizeof(*cursors));
    if (cursors == NULL)
        return (log_print_err("verify_checkpoint", ENOMEM, 1));

    for (i = 0; i < g.ntables; i++) {
        /*
         * TODO: LSM doesn't currently support reading from checkpoints.
         */
        if (g.cookies[i].type == LSM)
            continue;
        testutil_check(__wt_snprintf(next_uri, sizeof(next_uri), "table:__wt%04d", i));
        if ((ret = session->open_cursor(session, next_uri, NULL, ckpt, &cursors[i])) != 0) {
            (void)log_print_err("verify_checkpoint:session.open_cursor", ret, 1);
            goto err;
        }
    }

    /* There's no way to verify LSM-only runs. */
    if (cursors[0] == NULL) {
        printf("LSM-only, skipping checkpoint verification\n");
        goto err;
    }

    while (ret == 0) {
        ret = cursors[0]->next(cursors[0]);
        if (ret == 0)
            ++key_count;
        else if (ret != WT_NOTFOUND) {
            (void)log_print_err("cursor->next", ret, 1);
            goto err;
        }
        /*
         * Check to see that all remaining cursors have the same key/value pair.
         */
        for (i = 1; i < g.ntables; i++) {
            /*
             * TODO: LSM doesn't currently support reading from checkpoints.
             */
            if (g.cookies[i].type == LSM)
                continue;
            t_ret = cursors[i]->next(cursors[i]);
            if (t_ret != 0 && t_ret != WT_NOTFOUND) {
                (void)log_print_err("cursor->next", t_ret, 1);
                goto err;
            }

            if (ret == WT_NOTFOUND && t_ret == WT_NOTFOUND)
                continue;
            else if (ret == WT_NOTFOUND || t_ret == WT_NOTFOUND) {
                (void)log_print_err(
                  "verify_checkpoint tables with different"
                  " amount of data",
                  EFAULT, 1);
                goto err;
            }

            type0 = type_to_string(g.cookies[0].type);
            typei = type_to_string(g.cookies[i].type);
            if ((ret = compare_cursors(cursors[0], type0, cursors[i], typei)) != 0) {
                (void)diagnose_key_error(cursors[0], 0, cursors[i], i);
                (void)log_print_err("verify_checkpoint - mismatching data", EFAULT, 1);
                goto err;
            }
        }
    }
    printf("Finished verifying a checkpoint with %d tables and %" PRIu64 " keys\n", g.ntables,
      key_count);

err:
    for (i = 0; i < g.ntables; i++) {
        if (cursors[i] != NULL && (ret = cursors[i]->close(cursors[i])) != 0)
            (void)log_print_err("verify_checkpoint:cursor close", ret, 1);
    }
    free(cursors);
    return (ret);
}

/*
 * compare_cursors --
 *     Compare the key/value pairs from two cursors.
 */
static int
compare_cursors(WT_CURSOR *cursor1, const char *type1, WT_CURSOR *cursor2, const char *type2)
{
    uint64_t key1, key2;
    int ret;
    char buf[128], *val1, *val2;

    ret = 0;
    memset(buf, 0, 128);

    if (cursor1->get_key(cursor1, &key1) != 0 || cursor2->get_key(cursor2, &key2) != 0)
        return (log_print_err("Error getting keys", EINVAL, 1));

    if (cursor1->get_value(cursor1, &val1) != 0 || cursor2->get_value(cursor2, &val2) != 0)
        return (log_print_err("Error getting values", EINVAL, 1));

    if (g.logfp != NULL)
        fprintf(
          g.logfp, "k1: %" PRIu64 " k2: %" PRIu64 " val1: %s val2: %s \n", key1, key2, val1, val2);

    if (key1 != key2)
        ret = ERR_KEY_MISMATCH;
    else if (strlen(val1) != strlen(val2) || strcmp(val1, val2) != 0)
        ret = ERR_DATA_MISMATCH;
    else
        return (0);

    printf("Key/value mismatch: %" PRIu64 "/%s from a %s table is not %" PRIu64
           "/%s from a %s table\n",
      key1, val1, type1, key2, val2, type2);

    return (ret);
}

/*
 * diagnose_key_error --
 *     Dig a bit deeper on failure. Continue after some failures here to extract as much information
 *     as we can.
 */
static int
diagnose_key_error(WT_CURSOR *cursor1, int index1, WT_CURSOR *cursor2, int index2)
{
    WT_CURSOR *c;
    WT_SESSION *session;
    uint64_t key1, key1_orig, key2, key2_orig;
    int ret;
    char ckpt[128], next_uri[128];

    /* Hack to avoid passing session as parameter. */
    session = cursor1->session;
    key1_orig = key2_orig = 0;

    testutil_check(__wt_snprintf(ckpt, sizeof(ckpt), "checkpoint=%s", g.checkpoint_name));

    /* Save the failed keys. */
    if (cursor1->get_key(cursor1, &key1_orig) != 0 || cursor2->get_key(cursor2, &key2_orig) != 0) {
        (void)log_print_err("Error retrieving key.", EINVAL, 0);
        goto live_check;
    }

    if (key1_orig == key2_orig)
        goto live_check;

    /* See if previous values are still valid. */
    if (cursor1->prev(cursor1) != 0 || cursor2->prev(cursor2) != 0)
        return (1);
    if (cursor1->get_key(cursor1, &key1) != 0 || cursor2->get_key(cursor2, &key2) != 0)
        (void)log_print_err("Error decoding key", EINVAL, 1);
    else if (key1 != key2)
        (void)log_print_err("Now previous keys don't match", EINVAL, 0);

    if (cursor1->next(cursor1) != 0 || cursor2->next(cursor2) != 0)
        return (1);
    if (cursor1->get_key(cursor1, &key1) != 0 || cursor2->get_key(cursor2, &key2) != 0)
        (void)log_print_err("Error decoding key", EINVAL, 1);
    else if (key1 == key2)
        (void)log_print_err("After prev/next keys match", EINVAL, 0);

    if (cursor1->next(cursor1) != 0 || cursor2->next(cursor2) != 0)
        return (1);
    if (cursor1->get_key(cursor1, &key1) != 0 || cursor2->get_key(cursor2, &key2) != 0)
        (void)log_print_err("Error decoding key", EINVAL, 1);
    else if (key1 == key2)
        (void)log_print_err("After prev/next/next keys match", EINVAL, 0);

    /*
     * Now try opening new cursors on the checkpoints and see if we get the same missing key via
     * searching.
     */
    testutil_check(__wt_snprintf(next_uri, sizeof(next_uri), "table:__wt%04d", index1));
    if (session->open_cursor(session, next_uri, NULL, ckpt, &c) != 0)
        return (1);
    c->set_key(c, key1_orig);
    if ((ret = c->search(c)) != 0)
        (void)log_print_err("1st cursor didn't find 1st key", ret, 0);
    c->set_key(c, key2_orig);
    if ((ret = c->search(c)) != 0)
        (void)log_print_err("1st cursor didn't find 2nd key", ret, 0);
    if (c->close(c) != 0)
        return (1);

    testutil_check(__wt_snprintf(next_uri, sizeof(next_uri), "table:__wt%04d", index2));
    if (session->open_cursor(session, next_uri, NULL, ckpt, &c) != 0)
        return (1);
    c->set_key(c, key1_orig);
    if ((ret = c->search(c)) != 0)
        (void)log_print_err("2nd cursor didn't find 1st key", ret, 0);
    c->set_key(c, key2_orig);
    if ((ret = c->search(c)) != 0)
        (void)log_print_err("2nd cursor didn't find 2nd key", ret, 0);
    if (c->close(c) != 0)
        return (1);

live_check:
    /*
     * Now try opening cursors on the live checkpoint to see if we get the same missing key via
     * searching.
     */
    testutil_check(__wt_snprintf(next_uri, sizeof(next_uri), "table:__wt%04d", index1));
    if (session->open_cursor(session, next_uri, NULL, NULL, &c) != 0)
        return (1);
    c->set_key(c, key1_orig);
    if ((ret = c->search(c)) != 0)
        (void)log_print_err("1st cursor didn't find 1st key", ret, 0);
    if (c->close(c) != 0)
        return (1);

    testutil_check(__wt_snprintf(next_uri, sizeof(next_uri), "table:__wt%04d", index2));
    if (session->open_cursor(session, next_uri, NULL, NULL, &c) != 0)
        return (1);
    c->set_key(c, key2_orig);
    if ((ret = c->search(c)) != 0)
        (void)log_print_err("2nd cursor didn't find 2nd key", ret, 0);
    if (c->close(c) != 0)
        return (1);

    return (0);
}
