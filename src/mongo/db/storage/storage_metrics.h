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

    bool isEmpty() const {
        return (counter_ops::get(prepareReadConflicts) == 0);
    }

    /**
     * Increments prepareReadConflicts by n.
     */
    void incrementPrepareReadConflicts(CounterType n) {
        counter_ops::add(prepareReadConflicts, n);
    }

    // Number of read conflicts caused by a prepared transaction.
    CounterType prepareReadConflicts{0};

private:
    template <typename OtherType>
    StorageMetrics& set(const StorageMetrics<OtherType>& other) {
        counter_ops::set(prepareReadConflicts, other.prepareReadConflicts);
        return *this;
    }

    /**
     * Helper to add all metrics in a different StorageMetrics to this instance's metrics. Can be
     * used for subtraction by negating the metrics.
     */
    template <typename OtherType>
    StorageMetrics& add(const StorageMetrics<OtherType>& other, bool positive) {
        incrementPrepareReadConflicts(positive ? counter_ops::get(other.prepareReadConflicts)
                                               : -counter_ops::get(other.prepareReadConflicts));
        return *this;
    }
};

typedef StorageMetrics<int64_t> SingleThreadedStorageMetrics;
typedef StorageMetrics<AtomicWord<long long>> AtomicStorageMetrics;

}  // namespace mongo
