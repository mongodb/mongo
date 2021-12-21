/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __metadata_turtle --
 *     Return if a key's value should be taken from the turtle file.
 */
static bool
__metadata_turtle(const char *key)
{
    switch (key[0]) {
    case 'C':
        if (strcmp(key, WT_METADATA_COMPAT) == 0)
            return (true);
        break;
    case 'f':
        if (strcmp(key, WT_METAFILE_URI) == 0)
            return (true);
        break;
    case 'W':
        if (strcmp(key, WT_METADATA_VERSION) == 0)
            return (true);
        if (strcmp(key, WT_METADATA_VERSION_STR) == 0)
            return (true);
        break;
    }
    return (false);
}

/*
 * __wt_metadata_turtle_rewrite --
 *     Rewrite the turtle file. We wrap this because the lower functions expect a URI key and config
 *     value pair for the metadata. This function exists to push out the other contents to the
 *     turtle file such as a change in compatibility information.
 */
int
__wt_metadata_turtle_rewrite(WT_SESSION_IMPL *session)
{
    WT_DECL_RET;
    char *value;

    WT_RET(__wt_metadata_search(session, WT_METAFILE_URI, &value));
    ret = __wt_metadata_update(session, WT_METAFILE_URI, value);
    __wt_free(session, value);
    return (ret);
}

/*
 * __wt_metadata_cursor_open --
 *     Opens a cursor on the metadata.
 */
int
__wt_metadata_cursor_open(WT_SESSION_IMPL *session, const char *config, WT_CURSOR **cursorp)
{
    WT_BTREE *btree;
    WT_DECL_RET;
    const char *open_cursor_cfg[] = {WT_CONFIG_BASE(session, WT_SESSION_open_cursor), config, NULL};

    WT_WITHOUT_DHANDLE(
      session, ret = __wt_open_cursor(session, WT_METAFILE_URI, NULL, open_cursor_cfg, cursorp));
    WT_RET(ret);

    /*
     * Retrieve the btree from the cursor, rather than the session because we don't always switch
     * the metadata handle in to the session before entering this function.
     */
    btree = CUR2BT(*cursorp);

/*
 * Special settings for metadata: skew eviction so metadata almost always stays in cache and make
 * sure metadata is logged if possible.
 *
 * Test before setting so updates can't race in subsequent opens (the first update is safe because
 * it's single-threaded from wiredtiger_open).
 */
#define WT_EVICT_META_SKEW 10000
    if (btree->evict_priority == 0)
        WT_WITH_BTREE(session, btree, __wt_evict_priority_set(session, WT_EVICT_META_SKEW));
    if (F_ISSET(btree, WT_BTREE_NO_LOGGING))
        F_CLR(btree, WT_BTREE_NO_LOGGING);

    return (0);
}

/*
 * __wt_metadata_cursor --
 *     Returns the session's cached metadata cursor, unless it's in use, in which case it opens and
 *     returns another metadata cursor.
 */
int
__wt_metadata_cursor(WT_SESSION_IMPL *session, WT_CURSOR **cursorp)
{
    WT_CURSOR *cursor;

    /*
     * If we don't have a cached metadata cursor, or it's already in use, we'll need to open a new
     * one.
     */
    cursor = NULL;
    if (session->meta_cursor == NULL || F_ISSET(session->meta_cursor, WT_CURSTD_META_INUSE)) {
        WT_RET(__wt_metadata_cursor_open(session, NULL, &cursor));
        if (session->meta_cursor == NULL) {
            session->meta_cursor = cursor;
            cursor = NULL;
        }
    }

    /*
     * If there's no cursor return, we're done, our caller should have just been triggering the
     * creation of the session's cached cursor. There should not be an open local cursor in that
     * case, but caution doesn't cost anything.
     */
    if (cursorp == NULL)
        return (cursor == NULL ? 0 : cursor->close(cursor));

    /*
     * If the cached cursor is in use, return the newly opened cursor, else mark the cached cursor
     * in use and return it.
     */
    if (F_ISSET(session->meta_cursor, WT_CURSTD_META_INUSE))
        *cursorp = cursor;
    else {
        *cursorp = session->meta_cursor;
        F_SET(session->meta_cursor, WT_CURSTD_META_INUSE);
    }
    return (0);
}

/*
 * __wt_metadata_cursor_close --
 *     Close a metadata cursor.
 */
int
__wt_metadata_cursor_close(WT_SESSION_IMPL *session)
{
    WT_DECL_RET;

    if (session->meta_cursor != NULL)
        ret = session->meta_cursor->close(session->meta_cursor);
    session->meta_cursor = NULL;
    return (ret);
}

/*
 * __wt_metadata_cursor_release --
 *     Release a metadata cursor.
 */
int
__wt_metadata_cursor_release(WT_SESSION_IMPL *session, WT_CURSOR **cursorp)
{
    WT_CURSOR *cursor;

    WT_UNUSED(session);

    if ((cursor = *cursorp) == NULL)
        return (0);
    *cursorp = NULL;

    /*
     * If using the session's cached metadata cursor, clear the in-use flag and reset it, otherwise,
     * discard the cursor.
     */
    if (F_ISSET(cursor, WT_CURSTD_META_INUSE)) {
        WT_ASSERT(session, cursor == session->meta_cursor);

        F_CLR(cursor, WT_CURSTD_META_INUSE);
        return (cursor->reset(cursor));
    }
    return (cursor->close(cursor));
}

/*
 * __wt_metadata_insert --
 *     Insert a row into the metadata.
 */
int
__wt_metadata_insert(WT_SESSION_IMPL *session, const char *key, const char *value)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;

    __wt_verbose(session, WT_VERB_METADATA,
      "Insert: key: %s, value: %s, tracking: %s, %s"
      "turtle",
      key, value, WT_META_TRACKING(session) ? "true" : "false",
      __metadata_turtle(key) ? "" : "not ");

    if (__metadata_turtle(key))
        WT_RET_MSG(session, EINVAL, "%s: insert not supported on the turtle file", key);

    WT_RET(__wt_metadata_cursor(session, &cursor));
    cursor->set_key(cursor, key);
    cursor->set_value(cursor, value);
    WT_ERR(cursor->insert(cursor));
    if (WT_META_TRACKING(session))
        WT_ERR(__wt_meta_track_insert(session, key));
err:
    WT_TRET(__wt_metadata_cursor_release(session, &cursor));
    return (ret);
}

/*
 * __wt_metadata_update --
 *     Update a row in the metadata.
 */
int
__wt_metadata_update(WT_SESSION_IMPL *session, const char *key, const char *value)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;

    __wt_verbose(session, WT_VERB_METADATA,
      "Update: key: %s, value: %s, tracking: %s, %s"
      "turtle",
      key, value, WT_META_TRACKING(session) ? "true" : "false",
      __metadata_turtle(key) ? "" : "not ");

    if (__metadata_turtle(key)) {
        WT_WITH_TURTLE_LOCK(session, ret = __wt_turtle_update(session, key, value));
        return (ret);
    }

    if (WT_META_TRACKING(session))
        WT_RET(__wt_meta_track_update(session, key));

    WT_RET(__wt_metadata_cursor(session, &cursor));
    /* This cursor needs to have overwrite semantics. */
    WT_ASSERT(session, F_ISSET(cursor, WT_CURSTD_OVERWRITE));

    cursor->set_key(cursor, key);
    cursor->set_value(cursor, value);
    WT_ERR(cursor->insert(cursor));
err:
    WT_TRET(__wt_metadata_cursor_release(session, &cursor));
    return (ret);
}

/*
 * __wt_metadata_remove --
 *     Remove a row from the metadata.
 */
int
__wt_metadata_remove(WT_SESSION_IMPL *session, const char *key)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;

    __wt_verbose(session, WT_VERB_METADATA,
      "Remove: key: %s, tracking: %s, %s"
      "turtle",
      key, WT_META_TRACKING(session) ? "true" : "false", __metadata_turtle(key) ? "" : "not ");

    if (__metadata_turtle(key))
        WT_RET_MSG(session, EINVAL, "%s: remove not supported on the turtle file", key);

    /*
     * Take, release, and reacquire the metadata cursor. It's complicated, but that way the
     * underlying meta-tracking function doesn't have to open a second metadata cursor, it can use
     * the session's cached one.
     */
    WT_RET(__wt_metadata_cursor(session, &cursor));
    cursor->set_key(cursor, key);
    WT_ERR(cursor->search(cursor));
    WT_ERR(__wt_metadata_cursor_release(session, &cursor));

    if (WT_META_TRACKING(session))
        WT_ERR(__wt_meta_track_update(session, key));

    WT_ERR(__wt_metadata_cursor(session, &cursor));
    cursor->set_key(cursor, key);
    ret = cursor->remove(cursor);

err:
    WT_TRET(__wt_metadata_cursor_release(session, &cursor));
    return (ret);
}

/*
 * __wt_metadata_search --
 *     Return a copied row from the metadata. The caller is responsible for freeing the allocated
 *     memory.
 */
int
__wt_metadata_search(WT_SESSION_IMPL *session, const char *key, char **valuep)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;
    const char *value;

    *valuep = NULL;

    __wt_verbose(session, WT_VERB_METADATA,
      "Search: key: %s, tracking: %s, %s"
      "turtle",
      key, WT_META_TRACKING(session) ? "true" : "false", __metadata_turtle(key) ? "" : "not ");

    if (__metadata_turtle(key)) {
        /*
         * The returned value should only be set if ret is non-zero, but Coverity is convinced
         * otherwise. The code path is used enough that Coverity complains a lot, add an error check
         * to get some peace and quiet.
         */
        WT_WITH_TURTLE_LOCK(session, ret = __wt_turtle_read(session, key, valuep));
        if (ret != 0)
            __wt_free(session, *valuep);
        return (ret);
    }

    /*
     * All metadata reads are at read-uncommitted isolation. That's because once a schema-level
     * operation completes, subsequent operations must see the current version of checkpoint
     * metadata, or they may try to read blocks that may have been freed from a file. Metadata
     * updates use non-transactional techniques (such as the schema and metadata locks) to protect
     * access to in-flight updates.
     */
    WT_RET(__wt_metadata_cursor(session, &cursor));
    cursor->set_key(cursor, key);
    WT_WITH_TXN_ISOLATION(session, WT_ISO_READ_UNCOMMITTED, ret = cursor->search(cursor));
    WT_ERR(ret);

    WT_ERR(cursor->get_value(cursor, &value));
    WT_ERR(__wt_strdup(session, value, valuep));

err:
    WT_TRET(__wt_metadata_cursor_release(session, &cursor));

    if (ret != 0)
        __wt_free(session, *valuep);
    return (ret);
}
