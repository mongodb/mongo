/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */
#pragma once

#include "wt_internal.h"
#include <stdio.h>
#include <stdint.h>

typedef uint64_t atomic_t;
typedef uint64_t value_t;

#define ATOMIC_DEFINE(var, value) static atomic_t var = value

static void atomic_store_release(atomic_t* var, value_t value) {
    WT_RELEASE_WRITE(*var, value);
}

static value_t atomic_load_acquire(atomic_t* var) {
    value_t result;
    WT_ACQUIRE_READ(result, *var);
    return result;
}

static const char *get_mode(void) {
    return "WT atomics";
}
