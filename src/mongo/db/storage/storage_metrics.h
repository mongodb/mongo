/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/stats/counter_ops.h"
#include "mongo/platform/atomic_word.h"

namespace mongo {

/**
 * Statistics that are accumulated and tracked within mongodb as opposed to retrieved directly from
 * the storage engine.
 */
template <typename CounterType>
class StorageMetrics {
public:
    StorageMetrics() = default;

    StorageMetrics(const StorageMetrics<CounterType>& other) {
        this->set(other);
    };

    template <typename OtherType>
    StorageMetrics(const StorageMetrics<OtherType>& other) {
        this->set(other);
    };

    StorageMetrics& operator=(const StorageMetrics<CounterType>& other) {
        return this->set(other);
    }

    template <typename OtherType>
    StorageMetrics& operator+=(const StorageMetrics<OtherType>& other) {
        return add(other, /*positive*/ true);
    }

    template <typename OtherType>
    StorageMetrics& operator-=(const StorageMetrics<OtherType>& other) {
        return add(other, /*positive*/ false);
    }

    /**
     * Increments the time taken to respond to an interrupt.
     */
    void incrementInterruptResponseNs(int64_t n) {
        counter_ops::add(interruptResponseNs, n);
    }

    /**
     * Time taken for the storage engine to acknowledge an interrupt. Not set if an interrupt was
     * set but not acknowledged by the storage engine.
     */
    CounterType interruptResponseNs{0};

    /**
     * Increments writeConflicts by n.
     */
    void incrementWriteConflicts(int64_t n) {
        counter_ops::add(writeConflicts, n);
    }

    /**
     * Number of write conflicts.
     */
    CounterType writeConflicts{0};

    /**
     * Increments temporarilyUnavailableErrors by n.
     */
    void incrementTemporarilyUnavailableErrors(int64_t n) {
        counter_ops::add(temporarilyUnavailableErrors, n);
    }

    /**
     * Number of write conflicts.
     */
    CounterType temporarilyUnavailableErrors{0};

    template <typename LhsType, typename RhsType>
    friend bool operator==(const StorageMetrics<LhsType>& lhs, const StorageMetrics<RhsType>& rhs);

private:
    template <typename OtherType>
    StorageMetrics& set(const StorageMetrics<OtherType>& other) {
        counter_ops::set(interruptResponseNs, other.interruptResponseNs);
        counter_ops::set(writeConflicts, other.writeConflicts);
        counter_ops::set(temporarilyUnavailableErrors, other.temporarilyUnavailableErrors);
        return *this;
    }

    /**
     * Helper to add all metrics in a different StorageMetrics to this instance's metrics. Can be
     * used for subtraction by negating the metrics.
     */
    template <typename OtherType>
    StorageMetrics& add(const StorageMetrics<OtherType>& other, bool positive) {
        incrementInterruptResponseNs(positive ? counter_ops::get(other.interruptResponseNs)
                                              : -counter_ops::get(other.interruptResponseNs));

        incrementWriteConflicts(positive ? counter_ops::get(other.writeConflicts)
                                         : -counter_ops::get(other.writeConflicts));

        incrementTemporarilyUnavailableErrors(
            positive ? counter_ops::get(other.temporarilyUnavailableErrors)
                     : -counter_ops::get(other.temporarilyUnavailableErrors));
        return *this;
    }
};

typedef StorageMetrics<int64_t> SingleThreadedStorageMetrics;
typedef StorageMetrics<AtomicWord<long long>> AtomicStorageMetrics;

template <typename LhsType, typename RhsType>
bool operator==(const StorageMetrics<LhsType>& lhs, const StorageMetrics<RhsType>& rhs) {
    return (counter_ops::get(lhs.interruptResponseNs) ==
                counter_ops::get(rhs.interruptResponseNs) &&
            counter_ops::get(lhs.writeConflicts) == counter_ops::get(rhs.writeConflicts) &&
            counter_ops::get(lhs.temporarilyUnavailableErrors) ==
                counter_ops::get(rhs.temporarilyUnavailableErrors));
}
}  // namespace mongo
