/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/platform/atomic_word.h"

#include <cstdint>

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
    AtomicWord<long long> _counter;
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
    AtomicWord<value_type> _value;
};
}  // namespace mongo
