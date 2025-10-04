/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */
#pragma once

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

typedef uint64_t atomic_t;
typedef uint64_t value_t;

#define ATOMIC_DEFINE(var, value) static atomic_t var = (value)

static void atomic_store_release(atomic_t* var, value_t value) {
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(var, value, __ATOMIC_RELAXED);
}

static value_t atomic_load_acquire(atomic_t* var) {
    value_t result = __atomic_load_n(var, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return result;
}

static const char *get_mode(void) {
    return "GCC acq/rel barriers";
}
