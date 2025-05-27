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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands.h"
#include "mongo/platform/atomic.h"

#include <array>
#include <cstdint>

namespace mongo {
namespace operation_latency_histogram_details {
// The number of buckets in the histogram.
constexpr int kMaxBuckets = 51;

constexpr int kHistogramsCount = static_cast<int>(Command::ReadWriteType::kLast);

// Retuns the inclusive lower bounds of the histogram buckets.
std::array<uint64_t, kMaxBuckets> getLowerBounds();

template <typename DataType>
struct HistogramData {
    std::array<DataType, kMaxBuckets> buckets{};
    DataType entryCount{0};
    DataType sum{0};
    // Sum of latency time spent doing Queryable Encryption operations.
    DataType sumQueryableEncryption{0};
};
}  // namespace operation_latency_histogram_details

/**
 * Stores statistics for latencies of read, write, command, and multi-document transaction
 * operations. There are two flavors to this type:
 * `OperationLatencyHistogram` is not thread-safe and requires callers to enforce synchronization.
 * `AtomicOperationLatencyHistogram` is thread-safe but doesn't offer atomic APIs. For example,
 * partial updates to individual counters and histogram buckets may be observed via calling
 * `append`. That is considered safe since the data stored by this class is solely used for
 * diagnostics.
 */
class OperationLatencyHistogram {
public:
    using HistogramType = operation_latency_histogram_details::HistogramData<uint64_t>;

    /**
     * Increments the bucket of the histogram based on the operation type.
     */
    void increment(uint64_t latency, Command::ReadWriteType type, bool isQueryableEncryptionOp);

    /**
     * Appends the four histograms with latency totals and operation counts.
     */
    void append(bool includeHistograms, bool slowMSBucketsOnly, BSONObjBuilder* builder) const;

private:
    std::array<HistogramType, operation_latency_histogram_details::kHistogramsCount> _histograms;
};

class AtomicOperationLatencyHistogram {
public:
    using HistogramType = operation_latency_histogram_details::HistogramData<Atomic<uint64_t>>;

    void increment(uint64_t latency, Command::ReadWriteType type, bool isQueryableEncryptionOp);
    void append(bool includeHistograms, bool slowMSBucketsOnly, BSONObjBuilder* builder) const;

private:
    std::array<HistogramType, operation_latency_histogram_details::kHistogramsCount> _histograms;
};
}  // namespace mongo
