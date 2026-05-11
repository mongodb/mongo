/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */
#pragma once

/*
 * Futex API is intended for building other synchronization mechanisms, and is not suitable for
 * general use.
 */

/*
 * Linux limits the futex size to 32 bits irrespective of architecture word size.
 */
typedef uint32_t WT_FUTEX_WORD;

/*
 * Number of waiting threads to wake.
 */
typedef enum __wt_futex_wake { WT_FUTEX_WAKE_ONE, WT_FUTEX_WAKE_ALL } WT_FUTEX_WAKE;
