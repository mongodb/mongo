/**
 * Copyright (C) 2016 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/stats/operation_latency_histogram.h"

#include <algorithm>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/platform/bits.h"

namespace mongo {

const std::array<uint64_t, OperationLatencyHistogram::kMaxBuckets>
    OperationLatencyHistogram::kLowerBounds = {0,
                                               2,
                                               4,
                                               8,
                                               16,
                                               32,
                                               64,
                                               128,
                                               256,
                                               512,
                                               1024,
                                               2048,
                                               3072,
                                               4096,
                                               6144,
                                               8192,
                                               12288,
                                               16384,
                                               24576,
                                               32768,
                                               49152,
                                               65536,
                                               98304,
                                               131072,
                                               196608,
                                               262144,
                                               393216,
                                               524288,
                                               786432,
                                               1048576,
                                               1572864,
                                               2097152,
                                               4194304,
                                               8388608,
                                               16777216,
                                               33554432,
                                               67108864,
                                               134217728,
                                               268435456,
                                               536870912,
                                               1073741824,
                                               2147483648,
                                               4294967296,
                                               8589934592,
                                               17179869184,
                                               34359738368,
                                               68719476736,
                                               137438953472,
                                               274877906944,
                                               549755813888,
                                               1099511627776};

void OperationLatencyHistogram::_append(const HistogramData& data,
                                        const char* key,
                                        BSONObjBuilder* builder) const {

    BSONObjBuilder histogramBuilder(builder->subobjStart(key));
    BSONArrayBuilder arrayBuilder(histogramBuilder.subarrayStart("histogram"));
    for (int i = 0; i < kMaxBuckets; i++) {
        if (data.buckets[i] == 0)
            continue;
        BSONObjBuilder entryBuilder(arrayBuilder.subobjStart());
        entryBuilder.append("micros", static_cast<long long>(kLowerBounds[i]));
        entryBuilder.append("count", static_cast<long long>(data.buckets[i]));
        entryBuilder.doneFast();
    }

    arrayBuilder.doneFast();
    histogramBuilder.append("latency", static_cast<long long>(data.sum));
    histogramBuilder.append("ops", static_cast<long long>(data.entryCount));
    histogramBuilder.doneFast();
}

void OperationLatencyHistogram::append(BSONObjBuilder* builder) const {
    _append(_reads, "reads", builder);
    _append(_writes, "writes", builder);
    _append(_commands, "commands", builder);
}

// Computes the log base 2 of value, and checks for cases of split buckets.
int OperationLatencyHistogram::_getBucket(uint64_t value) {
    // Zero is a special case since log(0) is undefined.
    if (value == 0) {
        return 0;
    }

    int log2 = 63 - countLeadingZeros64(value);
    // Half splits occur in range [2^11, 2^21) giving 10 extra buckets.
    if (log2 < 11) {
        return log2;
    } else if (log2 < 21) {
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
        return std::min(log2 + 10, kMaxBuckets - 1);
    }
}

void OperationLatencyHistogram::_incrementData(uint64_t latency, int bucket, HistogramData* data) {
    data->buckets[bucket]++;
    data->entryCount++;
    data->sum += latency;
}

void OperationLatencyHistogram::increment(uint64_t latency, Command::ReadWriteType type) {
    int bucket = _getBucket(latency);
    switch (type) {
        case Command::ReadWriteType::kRead:
            _incrementData(latency, bucket, &_reads);
            break;
        case Command::ReadWriteType::kWrite:
            _incrementData(latency, bucket, &_writes);
            break;
        case Command::ReadWriteType::kCommand:
            _incrementData(latency, bucket, &_commands);
            break;
        default:
            MONGO_UNREACHABLE;
    }
}

}  // namespace mongo
