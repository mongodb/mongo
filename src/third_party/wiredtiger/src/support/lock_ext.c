/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_ext_spin_init --
 *     Allocate and initialize a spinlock.
 */
int
__wt_ext_spin_init(WT_EXTENSION_API *wt_api, WT_EXTENSION_SPINLOCK *ext_spinlock, const char *name)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *default_session;
    WT_SPINLOCK *lock;

    ext_spinlock->spinlock = NULL;
    default_session = ((WT_CONNECTION_IMPL *)wt_api->conn)->default_session;
    if ((ret = __wt_calloc_one(default_session, &lock)) != 0)
        return (ret);
    if ((ret = __wt_spin_init(default_session, lock, name)) != 0) {
        __wt_free(default_session, lock);
        return (ret);
    }
    ext_spinlock->spinlock = lock;
    return (0);
}

/*
 * __wt_ext_spin_lock --
 *     Lock the spinlock.
 */
void
__wt_ext_spin_lock(
  WT_EXTENSION_API *wt_api, WT_SESSION *session, WT_EXTENSION_SPINLOCK *ext_spinlock)
{
    WT_SPINLOCK *lock;

    WT_UNUSED(wt_api); /* Unused parameters */
    lock = ((WT_SPINLOCK *)ext_spinlock->spinlock);
    __wt_spin_lock((WT_SESSION_IMPL *)session, lock);
}

/*
 * __wt_ext_spin_unlock --
 *     Unlock the spinlock.
 */
void
__wt_ext_spin_unlock(
  WT_EXTENSION_API *wt_api, WT_SESSION *session, WT_EXTENSION_SPINLOCK *ext_spinlock)
{
    WT_SPINLOCK *lock;

    WT_UNUSED(wt_api); /* Unused parameters */
    lock = ((WT_SPINLOCK *)ext_spinlock->spinlock);
    __wt_spin_unlock((WT_SESSION_IMPL *)session, lock);
}

/*
 * __wt_ext_spin_destroy --
 *     Destroy the spinlock.
 */
void
__wt_ext_spin_destroy(WT_EXTENSION_API *wt_api, WT_EXTENSION_SPINLOCK *ext_spinlock)
{
    WT_SESSION_IMPL *default_session;
    WT_SPINLOCK *lock;

    lock = ((WT_SPINLOCK *)ext_spinlock->spinlock);

    /* Default session is used to comply with the lock initialization. */
    default_session = ((WT_CONNECTION_IMPL *)wt_api->conn)->default_session;
    __wt_spin_destroy(default_session, lock);
    __wt_free(default_session, lock);
    ext_spinlock->spinlock = NULL;
}
