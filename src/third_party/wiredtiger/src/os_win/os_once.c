/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_init_once_callback --
 *     Global initialization, run once.
 */
BOOL CALLBACK
__wt_init_once_callback(
  _Inout_ PINIT_ONCE InitOnce, _Inout_opt_ PVOID Parameter, _Out_opt_ PVOID *Context)
{
    void (*init_routine)(void);
    WT_UNUSED(InitOnce);
    WT_UNUSED(Context);

    init_routine = Parameter;
    init_routine();

    return (TRUE);
}

/*
 * __wt_once --
 *     One-time initialization per process.
 */
int
__wt_once(void (*init_routine)(void))
{
    static INIT_ONCE once_control = INIT_ONCE_STATIC_INIT;
    PVOID lpContext = NULL;

    return (!InitOnceExecuteOnce(&once_control, &__wt_init_once_callback, init_routine, lpContext));
}
