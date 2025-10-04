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

#include "mongo/db/stats/operation_latency_histogram.h"

#include "mongo/base/string_data.h"
#include "mongo/db/server_options.h"
#include "mongo/platform/bits.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace mongo {
namespace {
const std::array<uint64_t, operation_latency_histogram_details::kMaxBuckets> kLowerBounds = {
    0,             // 0x00000000000
    2,             // 0x00000000002
    4,             // 0x00000000004
    8,             // 0x00000000008
    16,            // 0x00000000010
    32,            // 0x00000000020
    64,            // 0x00000000040
    128,           // 0x00000000080
    256,           // 0x00000000100
    512,           // 0x00000000200
    1024,          // 0x00000000400
    2048,          // 0x00000000800 = 2^11
    3072,          // 0x00000000C00
    4096,          // 0x00000001000
    6144,          // 0x00000001800
    8192,          // 0x00000002000
    12288,         // 0x00000003000
    16384,         // 0x00000004000
    24576,         // 0x00000006000
    32768,         // 0x00000008000
    49152,         // 0x0000000C000
    65536,         // 0x00000010000
    98304,         // 0x00000018000
    131072,        // 0x00000020000
    196608,        // 0x00000030000
    262144,        // 0x00000040000
    393216,        // 0x00000060000
    524288,        // 0x00000080000
    786432,        // 0x000000C0000
    1048576,       // 0x00000100000
    1572864,       // 0x00000180000
    2097152,       // 0x00000200000 = 2^21
    4194304,       // 0x00000400000
    8388608,       // 0x00000800000
    16777216,      // 0x00001000000
    33554432,      // 0x00002000000
    67108864,      // 0x00004000000
    134217728,     // 0x00008000000
    268435456,     // 0x00010000000
    536870912,     // 0x00020000000
    1073741824,    // 0x00040000000
    2147483648,    // 0x00080000000
    4294967296,    // 0x00100000000
    8589934592,    // 0x00200000000
    17179869184,   // 0x00400000000
    34359738368,   // 0x00800000000
    68719476736,   // 0x01000000000
    137438953472,  // 0x02000000000
    274877906944,  // 0x04000000000
    549755813888,  // 0x08000000000
    1099511627776  // 0x10000000000
};

// Computes the log base 2 of value, and checks for cases of split buckets.
size_t getBucket(uint64_t value) {
    if (value == 0) {
        // Zero is a special case since log(0) is undefined.
        return 0;
    }

    int log2 = 63 - countLeadingZeros64(value);
    if (log2 < 11) {
        return log2;
    } else if (log2 < 21) {
        // Half splits occur in range [2^11, 2^21) giving 10 extra buckets.
        int extra = log2 - 11;
        // Split value boundary is at (2^n + 2^(n+1))/2 = 2^n + 2^(n-1).
        // Which corresponds to (1ULL << log2) | (1ULL << (log2 - 1))
        // Which is equivalent to the following:
        uint64_t splitBoundary = 3ULL << (log2 - 1);
        if (value >= splitBoundary) {
            extra++;
        }
        return log2 + extra;
    } else {
        // Add all of the extra 10 buckets.
        return std::min(log2 + 10, operation_latency_histogram_details::kMaxBuckets - 1);
    }
}

template <typename DataType>
void updateHistogram(operation_latency_histogram_details::HistogramData<DataType>& histogram,
                     size_t bucket,
                     uint64_t latency,
                     bool isQueryableEncryptionOperation) {
    histogram.buckets[bucket]++;
    histogram.entryCount++;
    histogram.sum += latency;

    if (isQueryableEncryptionOperation) {
        histogram.sumQueryableEncryption += latency;
    }
}

template <>
void updateHistogram(AtomicOperationLatencyHistogram::HistogramType& histogram,
                     size_t bucket,
                     uint64_t latency,
                     bool isQueryableEncryptionOperation) {
    histogram.buckets[bucket].fetchAndAddRelaxed(1);
    histogram.entryCount.fetchAndAddRelaxed(1);
    histogram.sum.fetchAndAddRelaxed(latency);

    if (isQueryableEncryptionOperation) {
        histogram.sumQueryableEncryption.fetchAndAddRelaxed(latency);
    }
}

template <typename HistogramsType>
void increment(HistogramsType& histograms,
               uint64_t latency,
               Command::ReadWriteType type,
               bool isQueryableEncryptionOperation) {
    const auto index = static_cast<int>(type);
    invariant(index < operation_latency_histogram_details::kHistogramsCount);
    const auto bucket = getBucket(latency);
    updateHistogram(histograms[index], bucket, latency, isQueryableEncryptionOperation);
}

template <typename HistogramDataType, typename StringType>
void appendHistogram(const HistogramDataType& data,
                     StringType key,
                     bool includeHistograms,
                     bool slowMSBucketsOnly,
                     BSONObjBuilder& builder) {
    BSONObjBuilder histogramBuilder(builder.subobjStart(key));
    const uint64_t slowMicros = static_cast<uint64_t>(serverGlobalParams.slowMS.load()) * 1000;
    const bool filterBuckets = slowMSBucketsOnly && slowMicros >= 0;

    uint64_t filteredCount = 0;
    uint64_t lowestFilteredBound = 0;

    if (includeHistograms) {
        BSONArrayBuilder arrayBuilder(histogramBuilder.subarrayStart("histogram"));
        for (size_t i = 0; i < operation_latency_histogram_details::kMaxBuckets; i++) {
            const auto bucketValue = [&] {
                if constexpr (std::is_same_v<HistogramDataType,
                                             AtomicOperationLatencyHistogram::HistogramType>) {
                    return data.buckets[i].loadRelaxed();
                } else {
                    return data.buckets[i];
                }
            }();

            if (bucketValue == 0) {
                continue;
            }

            if (filterBuckets && kLowerBounds[i] >= slowMicros) {
                if (lowestFilteredBound == 0) {
                    lowestFilteredBound = kLowerBounds[i];
                }

                filteredCount += bucketValue;
                continue;
            }

            BSONObjBuilder entryBuilder(arrayBuilder.subobjStart());
            entryBuilder.append("micros", static_cast<long long>(kLowerBounds[i]));
            entryBuilder.append("count", static_cast<long long>(bucketValue));
            entryBuilder.doneFast();
        }

        // Append final bucket only if it contains values to minimize data in FTDC. Final bucket
        // is aggregate of all buckets >= slowMS with bucket labeled as minimum latency of
        // bucket.
        if (filterBuckets && filteredCount > 0) {
            BSONObjBuilder entryBuilder(arrayBuilder.subobjStart());
            entryBuilder.append("micros", static_cast<long long>(lowestFilteredBound + 1));
            entryBuilder.append("count", static_cast<long long>(filteredCount));
            entryBuilder.doneFast();
        }

        arrayBuilder.doneFast();
    }

    uint64_t latency, ops, queryableEncryptionLatencyMicros;
    if constexpr (std::is_same_v<HistogramDataType,
                                 AtomicOperationLatencyHistogram::HistogramType>) {
        latency = data.sum.loadRelaxed();
        ops = data.entryCount.loadRelaxed();
        queryableEncryptionLatencyMicros = data.sumQueryableEncryption.loadRelaxed();
    } else {
        latency = data.sum;
        ops = data.entryCount;
        queryableEncryptionLatencyMicros = data.sumQueryableEncryption;
    }

    histogramBuilder.append("latency", static_cast<long long>(latency));
    histogramBuilder.append("ops", static_cast<long long>(ops));
    histogramBuilder.append("queryableEncryptionLatencyMicros",
                            static_cast<long long>(queryableEncryptionLatencyMicros));
    histogramBuilder.doneFast();
}

// Note that histograms are expected to appear in the order specified by `kHistogramNames`.
template <typename HistogramsType>
void appendHistograms(HistogramsType& histograms,
                      bool includeHistograms,
                      bool slowMSBucketsOnly,
                      BSONObjBuilder& builder) {
    static_assert(static_cast<int>(Command::ReadWriteType::kCommand) == 0);
    static_assert(static_cast<int>(Command::ReadWriteType::kRead) == 1);
    static_assert(static_cast<int>(Command::ReadWriteType::kWrite) == 2);
    static_assert(static_cast<int>(Command::ReadWriteType::kTransaction) == 3);
    static constexpr std::array<StringData, operation_latency_histogram_details::kHistogramsCount>
        kNames = {"commands"_sd, "reads"_sd, "writes"_sd, "transactions"_sd};

    for (size_t i = 0; i < kNames.size(); ++i) {
        appendHistogram(histograms[i], kNames[i], includeHistograms, slowMSBucketsOnly, builder);
    }
}
}  // namespace

namespace operation_latency_histogram_details {
std::array<uint64_t, operation_latency_histogram_details::kMaxBuckets> getLowerBounds() {
    return kLowerBounds;
}
}  // namespace operation_latency_histogram_details

void OperationLatencyHistogram::increment(uint64_t latency,
                                          Command::ReadWriteType type,
                                          bool isQueryableEncryptionOperation) {
    ::mongo::increment(_histograms, latency, type, isQueryableEncryptionOperation);
}

void OperationLatencyHistogram::append(bool includeHistograms,
                                       bool slowMSBucketsOnly,
                                       BSONObjBuilder* builder) const {
    appendHistograms(_histograms, includeHistograms, slowMSBucketsOnly, *builder);
}

void AtomicOperationLatencyHistogram::increment(uint64_t latency,
                                                Command::ReadWriteType type,
                                                bool isQueryableEncryptionOperation) {
    ::mongo::increment(_histograms, latency, type, isQueryableEncryptionOperation);
}

void AtomicOperationLatencyHistogram::append(bool includeHistograms,
                                             bool slowMSBucketsOnly,
                                             BSONObjBuilder* builder) const {
    appendHistograms(_histograms, includeHistograms, slowMSBucketsOnly, *builder);
}

}  // namespace mongo
