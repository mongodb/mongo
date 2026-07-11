// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/modules.h"

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

struct OperationLatencyHistogramOptions {
    /*
     * If false, buckets with no counts will be excluded when calling `append`. This is useful when
     * the structure of the appended latency histograms should be consistent over time.
     */
    bool includeEmptyBuckets = false;

    /*
     * The log of the amount to increment each bucket size. By default, buckets are (mostly) powers
     * of 2, however this can be increased to make bucket sizes larger. This can be used to reduce
     * the number of fields added when calling `append`. This should always be at least 1.
     */
    int logBucketScalingFactor = 1;
};

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
    using Options = OperationLatencyHistogramOptions;

    explicit OperationLatencyHistogram(const Options& options = {});

    /**
     * Increments the bucket of the histogram based on the operation type.
     */
    void increment(uint64_t latency, Command::ReadWriteType type, bool isQueryableEncryptionOp);

    /**
     * Appends the four histograms with latency totals and operation counts. If `slowMSBucketsOnly`
     * is true, values above `slowMSBucketsOnly` are aggregated into a single bucket. The recorded
     * value of this bucket won't be exactly `slowMSBucketsOnly` but will be the smallest available
     * bucket threshold above it.
     */
    void append(bool includeHistograms, bool slowMSBucketsOnly, BSONObjBuilder* builder) const;

private:
    bool _includeEmptyBuckets;
    int _logBucketScalingFactor;
    std::array<HistogramType, operation_latency_histogram_details::kHistogramsCount> _histograms;
};

class AtomicOperationLatencyHistogram {
public:
    using HistogramType = operation_latency_histogram_details::HistogramData<Atomic<uint64_t>>;
    using Options = OperationLatencyHistogramOptions;

    explicit AtomicOperationLatencyHistogram(const Options& options = {});

    void increment(uint64_t latency, Command::ReadWriteType type, bool isQueryableEncryptionOp);
    void append(bool includeHistograms, bool slowMSBucketsOnly, BSONObjBuilder* builder) const;

private:
    bool _includeEmptyBuckets;
    int _logBucketScalingFactor;
    std::array<HistogramType, operation_latency_histogram_details::kHistogramsCount> _histograms;
};
}  // namespace mongo
