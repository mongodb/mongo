// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/platform/atomic.h"
#include "mongo/util/modules.h"

#include <cstdint>

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * Helper functions to operate on Atomic<long long> and int64_t interchangibly.
 */
namespace counter_ops {
static int64_t get(const int64_t& counter) {
    return counter;
}

static int64_t get(const Atomic<long long>& counter) {
    return counter.load();
}

static void set(int64_t& counter, int64_t value) {
    counter = value;
}

static void set(int64_t& counter, const Atomic<long long>& value) {
    counter = value.load();
}

static void set(Atomic<long long>& counter, int64_t value) {
    counter.store(value);
}

static void set(Atomic<long long>& counter, const Atomic<long long>& value) {
    counter.store(value.load());
}

static void add(int64_t& counter, int64_t value) {
    counter += value;
}

static void add(int64_t& counter, const Atomic<long long>& value) {
    counter += value.load();
}

static void add(Atomic<long long>& counter, int64_t value) {
    counter.addAndFetch(value);
}

};  // namespace counter_ops
}  // namespace mongo
