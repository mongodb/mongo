/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __tiered_opener_open --
 *     Open an object by number.
 */
static int
__tiered_opener_open(WT_BLOCK_FILE_OPENER *opener, WT_SESSION_IMPL *session, uint32_t object_id,
  WT_FS_OPEN_FILE_TYPE type, u_int flags, WT_FH **fhp)
{
    WT_BUCKET_STORAGE *bstorage;
    WT_DECL_RET;
    WT_TIERED *tiered;
    const char *object_name, *object_uri;

    tiered = opener->cookie;
    object_uri = NULL;

    WT_ASSERT(session,
      (object_id > 0 && object_id <= tiered->current_id) || object_id == WT_TIERED_CURRENT_ID);
    /*
     * FIXME-WT-7590 we will need some kind of locking while we're looking at the tiered structure.
     * This can be called at any time, because we are opening the objects lazily.
     */
    if (object_id == tiered->current_id || object_id == WT_TIERED_CURRENT_ID) {
        bstorage = NULL;
        object_name = tiered->tiers[WT_TIERED_INDEX_LOCAL].name;
        if (!WT_PREFIX_SKIP(object_name, "file:"))
            WT_RET_MSG(session, EINVAL, "expected a 'file:' URI");
        WT_ERR(__wt_open(session, object_name, type, flags, fhp));
    } else {
        WT_ERR(
          __wt_tiered_name(session, &tiered->iface, object_id, WT_TIERED_NAME_OBJECT, &object_uri));
        object_name = object_uri;
        WT_PREFIX_SKIP_REQUIRED(session, object_name, "object:");
        bstorage = tiered->bstorage;
        flags |= WT_FS_OPEN_READONLY;
        WT_WITH_BUCKET_STORAGE(
          bstorage, session, { ret = __wt_open(session, object_name, type, flags, fhp); });
        if (ret == ENOENT) {
            /*
             * There is a window where the object may not be copied yet to the bucket. If it isn't
             * found try the local system. If it isn't found there then try the bucket one more
             * time.
             */
            ret = __wt_open(session, object_name, type, flags, fhp);
            __wt_errx(session, "OPENER: local %s ret %d", object_name, ret);
            if (ret == ENOENT)
                WT_WITH_BUCKET_STORAGE(
                  bstorage, session, { ret = __wt_open(session, object_name, type, flags, fhp); });
            WT_ERR(ret);
        }
    }
err:
    __wt_free(session, object_uri);
    return (ret);
}

/*
 * __tiered_opener_current_id --
 *     Get the current writeable object id.
 */
static uint32_t
__tiered_opener_current_id(WT_BLOCK_FILE_OPENER *opener)
{
    WT_TIERED *tiered;

    tiered = opener->cookie;

    /*
     * FIXME-WT-7590 we will need some kind of locking while we're looking at the tiered structure.
     * This can be called at any time, because we are opening the objects lazily.
     */
    return (tiered->current_id);
}

/*
 * __wt_tiered_opener --
 *     Set up an opener for a tiered handle.
 */
int
__wt_tiered_opener(WT_SESSION_IMPL *session, WT_DATA_HANDLE *dhandle,
  WT_BLOCK_FILE_OPENER **openerp, const char **filenamep)
{
    WT_TIERED *tiered;
    const char *filename;

    filename = dhandle->name;
    *openerp = NULL;

    if (dhandle->type == WT_DHANDLE_TYPE_BTREE) {
        if (!WT_PREFIX_SKIP(filename, "file:"))
            WT_RET_MSG(session, EINVAL, "expected a 'file:' URI");
        *filenamep = filename;
    } else if (dhandle->type == WT_DHANDLE_TYPE_TIERED) {
        tiered = (WT_TIERED *)dhandle;
        tiered->opener.open = __tiered_opener_open;
        tiered->opener.current_object_id = __tiered_opener_current_id;
        tiered->opener.cookie = tiered;
        *openerp = &tiered->opener;
        *filenamep = dhandle->name;
    } else
        WT_RET_MSG(session, EINVAL, "invalid URI: %s", dhandle->name);

    return (0);
}
