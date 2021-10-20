/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_get_vm_pagesize --
 *     Return the default page size of a virtual memory page.
 */
int
__wt_get_vm_pagesize(void)
{
    return (getpagesize());
}
