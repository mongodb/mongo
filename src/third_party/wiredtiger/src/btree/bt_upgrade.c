/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_upgrade --
 *     Upgrade a file.
 */
int
__wt_upgrade(WT_SESSION_IMPL *session, const char *cfg[])
{
    WT_UNUSED(cfg);

    WT_ASSERT_SPINLOCK_OWNED(session, &S2C(session)->checkpoint_lock);
    WT_ASSERT_SPINLOCK_OWNED(session, &S2C(session)->schema_lock);

    /* There's nothing to upgrade, yet. */
    WT_RET(__wt_progress(session, NULL, 1));
    return (0);
}
