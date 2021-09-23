/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __meta_btree_apply --
 *     Apply a function to all files listed in the metadata, apart from the metadata file.
 */
static inline int
__meta_btree_apply(WT_SESSION_IMPL *session, WT_CURSOR *cursor,
  int (*file_func)(WT_SESSION_IMPL *, const char *[]),
  int (*name_func)(WT_SESSION_IMPL *, const char *, bool *), const char *cfg[])
{
    WT_DECL_RET;
    int t_ret;
    const char *uri;
    bool skip;

    /*
     * Accumulate errors but continue through to the end of the metadata.
     */
    while ((t_ret = cursor->next(cursor)) == 0) {
        if ((t_ret = cursor->get_key(cursor, &uri)) != 0 || strcmp(uri, WT_METAFILE_URI) == 0) {
            WT_TRET(t_ret);
            continue;
        }

        skip = false;
        if (name_func != NULL && (t_ret = name_func(session, uri, &skip)) != 0) {
            WT_TRET(t_ret);
            continue;
        }

        if (file_func == NULL || skip || !WT_BTREE_PREFIX(uri))
            continue;

        /*
         * We need to pull the handle into the session handle cache and make sure it's referenced to
         * stop other internal code dropping the handle (e.g in LSM when cleaning up obsolete
         * chunks). Holding the schema lock isn't enough.
         *
         * Handles that are busy are skipped without the whole operation failing. This deals among
         * other cases with checkpoint encountering handles that are locked (e.g., for bulk loads or
         * verify operations).
         */
        if ((t_ret = __wt_session_get_dhandle(session, uri, NULL, NULL, 0)) != 0) {
            WT_TRET_BUSY_OK(t_ret);
            continue;
        }

        WT_SAVE_DHANDLE(session, WT_TRET(file_func(session, cfg)));
        WT_TRET(__wt_session_release_dhandle(session));
    }
    WT_TRET_NOTFOUND_OK(t_ret);

    return (ret);
}

/*
 * __wt_meta_apply_all --
 *     Apply a function to all files listed in the metadata, apart from the metadata file.
 */
int
__wt_meta_apply_all(WT_SESSION_IMPL *session, int (*file_func)(WT_SESSION_IMPL *, const char *[]),
  int (*name_func)(WT_SESSION_IMPL *, const char *, bool *), const char *cfg[])
{
    WT_CURSOR *cursor;
    WT_DECL_RET;

    WT_ASSERT(session, FLD_ISSET(session->lock_flags, WT_SESSION_LOCKED_SCHEMA));
    WT_RET(__wt_metadata_cursor(session, &cursor));
    WT_SAVE_DHANDLE(session, ret = __meta_btree_apply(session, cursor, file_func, name_func, cfg));
    WT_TRET(__wt_metadata_cursor_release(session, &cursor));

    return (ret);
}
