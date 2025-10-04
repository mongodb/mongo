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

typedef atomic_uint_fast64_t atomic_t;
typedef uint64_t value_t;

#define ATOMIC_DEFINE(var, value) static atomic_t var = (value)

static void atomic_store_release(atomic_t* var, value_t value) {
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(var, value, memory_order_relaxed);
}

static value_t atomic_load_acquire(atomic_t* var) {
    value_t result = atomic_load_explicit(var, memory_order_relaxed);
    atomic_thread_fence(memory_order_acquire);
    return result;
}

static const char *get_mode(void) {
    return "C11 acq/rel barriers";
}
