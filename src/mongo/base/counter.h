// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/platform/atomic.h"
#include "mongo/util/modules.h"

#include <cstdint>

[[MONGO_MOD_PUBLIC]];

namespace mongo {
/**
 * A 64bit (atomic) counter.
 *
 * The constructor allows setting the start value, and increment([int]) is used to change it.
 *
 * The value can be returned using get() or the (long long) function operator.
 */
class Counter64 {
public:
    /** Atomically increment. */
    void increment(uint64_t n = 1) {
        _counter.fetchAndAdd(n);
    }

    /** Atomically increment with a relaxed memory order. */
    void incrementRelaxed(uint64_t n = 1) {
        _counter.fetchAndAddRelaxed(n);
    }

    /** Atomically decrement. */
    void decrement(uint64_t n = 1) {
        _counter.fetchAndSubtract(n);
    }

    /** Atomically decrement with a relaxed memory order. */
    void decrementRelaxed(uint64_t n = 1) {
        _counter.fetchAndSubtractRelaxed(n);
    }

    /** Atomically store the exact value 0. */
    void setToZero() {
        _counter.store(0);
    }

    /** Return the current value */
    long long get() const {
        return _counter.load();
    }

private:
    Atomic<long long> _counter;
};

/**
 * Atomic wrapper for Metrics. This is for values which are set rather than just
 * incremented or decremented; if you want a counter, use Counter64 above.
 */
class Atomic64Metric {
public:
    using value_type = int64_t;

    /** Sets value to the max of the current value and newValue. */
    void setToMax(value_type newValue) {
        auto current = _value.load();
        while (current < newValue)
            _value.compareAndSwap(&current, newValue);
    }

    void set(value_type val) {
        _value.storeRelaxed(val);
    }

    value_type get() const {
        return _value.loadRelaxed();
    }

private:
    Atomic<value_type> _value;
};
}  // namespace mongo
