/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_once --
 *     One-time initialization per process.
 */
int
__wt_once(void (*init_routine)(void))
{
    static pthread_once_t once_control = PTHREAD_ONCE_INIT;

    return (pthread_once(&once_control, init_routine));
}
