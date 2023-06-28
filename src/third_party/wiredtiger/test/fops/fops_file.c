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

#include "thread.h"

static u_int uid = 1;

/*
 * obj_bulk --
 *     TODO: Add a comment describing this function.
 */
void
obj_bulk(void)
{
    WT_CURSOR *c;
    WT_SESSION *session;
    int ret;
    bool create;

    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    if (use_txn)
        testutil_check(session->begin_transaction(session, NULL));
    create = false;
    if ((ret = session->create(session, uri, config)) != 0)
        if (ret != EEXIST && ret != EBUSY)
            testutil_die(ret, "session.create");

    if (ret == 0) {
        create = true;
        __wt_yield();
        if ((ret = session->open_cursor(session, uri, NULL, "bulk", &c)) == 0) {
            testutil_check(c->close(c));
        } else if (ret != ENOENT && ret != EBUSY && ret != EINVAL)
            testutil_die(ret, "session.open_cursor bulk");
    }

    if (use_txn) {
        /* If create fails, rollback else will commit.*/
        if (!create)
            ret = session->rollback_transaction(session, NULL);
        else
            ret = session->commit_transaction(session, NULL);

        if (ret == EINVAL)
            testutil_die(ret, "session.commit bulk");
    }
    testutil_check(session->close(session, NULL));
}

/*
 * obj_bulk_unique --
 *     TODO: Add a comment describing this function.
 */
void
obj_bulk_unique(int force)
{
    WT_CURSOR *c;
    WT_SESSION *session;
    int ret;
    char new_uri[64];

    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    /* Generate a unique object name. */
    testutil_check(pthread_rwlock_wrlock(&single));
    testutil_check(__wt_snprintf(new_uri, sizeof(new_uri), "%s.%u", uri, ++uid));
    testutil_check(pthread_rwlock_unlock(&single));

    if (use_txn)
        testutil_check(session->begin_transaction(session, NULL));
    testutil_check(session->create(session, new_uri, config));

    __wt_yield();
    /*
     * Opening a bulk cursor may have raced with a forced checkpoint which created a checkpoint of
     * the empty file, and triggers an EINVAL
     */
    if ((ret = session->open_cursor(session, new_uri, NULL, "bulk", &c)) == 0)
        testutil_check(c->close(c));
    else if (ret != EINVAL)
        testutil_die(ret, "session.open_cursor bulk unique: %s, new_uri");

    while ((ret = session->drop(session, new_uri, force ? "force" : NULL)) != 0)
        if (ret != EBUSY)
            testutil_die(ret, "session.drop: %s", new_uri);

    if (use_txn && (ret = session->commit_transaction(session, NULL)) != 0 && ret != EINVAL)
        testutil_die(ret, "session.commit bulk unique");
    testutil_check(session->close(session, NULL));
}

/*
 * obj_cursor --
 *     TODO: Add a comment describing this function.
 */
void
obj_cursor(void)
{
    WT_CURSOR *cursor;
    WT_SESSION *session;
    int ret;

    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    if (use_txn)
        testutil_check(session->begin_transaction(session, NULL));
    if ((ret = session->open_cursor(session, uri, NULL, NULL, &cursor)) != 0) {
        if (ret != ENOENT && ret != EBUSY)
            testutil_die(ret, "session.open_cursor");
    } else
        testutil_check(cursor->close(cursor));

    if (use_txn && (ret = session->commit_transaction(session, NULL)) != 0 && ret != EINVAL)
        testutil_die(ret, "session.commit cursor");
    testutil_check(session->close(session, NULL));
}

/*
 * obj_create --
 *     TODO: Add a comment describing this function.
 */
void
obj_create(void)
{
    WT_SESSION *session;
    int ret;

    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    if (use_txn)
        testutil_check(session->begin_transaction(session, NULL));
    if ((ret = session->create(session, uri, config)) != 0)
        if (ret != EEXIST && ret != EBUSY)
            testutil_die(ret, "session.create");

    if (use_txn && (ret = session->commit_transaction(session, NULL)) != 0 && ret != EINVAL)
        testutil_die(ret, "session.commit create");
    testutil_check(session->close(session, NULL));
}

/*
 * obj_create_unique --
 *     TODO: Add a comment describing this function.
 */
void
obj_create_unique(int force)
{
    WT_SESSION *session;
    int ret;
    char new_uri[64];

    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    /* Generate a unique object name. */
    testutil_check(pthread_rwlock_wrlock(&single));
    testutil_check(__wt_snprintf(new_uri, sizeof(new_uri), "%s.%u", uri, ++uid));
    testutil_check(pthread_rwlock_unlock(&single));

    if (use_txn)
        testutil_check(session->begin_transaction(session, NULL));
    testutil_check(session->create(session, new_uri, config));
    if (use_txn && (ret = session->commit_transaction(session, NULL)) != 0 && ret != EINVAL)
        testutil_die(ret, "session.commit create unique");

    __wt_yield();
    if (use_txn)
        testutil_check(session->begin_transaction(session, NULL));
    while ((ret = session->drop(session, new_uri, force ? "force" : NULL)) != 0)
        if (ret != EBUSY)
            testutil_die(ret, "session.drop: %s", new_uri);
    if (use_txn && (ret = session->commit_transaction(session, NULL)) != 0 && ret != EINVAL)
        testutil_die(ret, "session.commit create unique");

    testutil_check(session->close(session, NULL));
}

/*
 * obj_drop --
 *     TODO: Add a comment describing this function.
 */
void
obj_drop(int force)
{
    WT_SESSION *session;
    int ret;

    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    if (use_txn)
        testutil_check(session->begin_transaction(session, NULL));
    if ((ret = session->drop(session, uri, force ? "force" : NULL)) != 0)
        if (ret != ENOENT && ret != EBUSY)
            testutil_die(ret, "session.drop");

    if (use_txn) {
        /*
         * As the operations are being performed concurrently, return value can be ENOENT or EBUSY
         * will set error to transaction opened by session. In these cases the transaction has to be
         * aborted.
         */
        if (ret != ENOENT && ret != EBUSY)
            ret = session->commit_transaction(session, NULL);
        else
            ret = session->rollback_transaction(session, NULL);
        if (ret == EINVAL)
            testutil_die(ret, "session.commit drop");
    }
    testutil_check(session->close(session, NULL));
}

/*
 * obj_checkpoint --
 *     TODO: Add a comment describing this function.
 */
void
obj_checkpoint(void)
{
    WT_SESSION *session;
    int ret;

    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    /*
     * Force the checkpoint so it has to be taken. Forced checkpoints can race with other metadata
     * operations and return EBUSY - we'd expect applications using forced checkpoints to retry on
     * EBUSY.
     */
    if ((ret = session->checkpoint(session, "force")) != 0)
        if (ret != EBUSY && ret != ENOENT)
            testutil_die(ret, "session.checkpoint");

    testutil_check(session->close(session, NULL));
}

/*
 * obj_upgrade --
 *     TODO: Add a comment describing this function.
 */
void
obj_upgrade(void)
{
    WT_SESSION *session;
    int ret;

    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    if ((ret = session->upgrade(session, uri, NULL)) != 0)
        if (ret != ENOENT && ret != EBUSY)
            testutil_die(ret, "session.upgrade");

    testutil_check(session->close(session, NULL));
}

/*
 * obj_verify --
 *     TODO: Add a comment describing this function.
 */
void
obj_verify(void)
{
    WT_SESSION *session;
    int ret;

    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    if ((ret = session->verify(session, uri, NULL)) != 0)
        if (ret != ENOENT && ret != EBUSY)
            testutil_die(ret, "session.verify");

    testutil_check(session->close(session, NULL));
}
