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
#pragma once

#include <array>

#include "mongo/db/commands.h"

namespace mongo {

class BSONObjBuilder;

/**
 * Stores statistics for latencies of read, write, and command operations.
 *
 * Note: This class is not thread-safe.
 */
class OperationLatencyHistogram {
public:
    static const int kMaxBuckets = 51;

    // Inclusive lower bounds of the histogram buckets.
    static const std::array<uint64_t, kMaxBuckets> kLowerBounds;

    /**
     * Increments the bucket of the histogram based on the operation type.
     */
    void increment(uint64_t latency, Command::ReadWriteType type);

    /**
     * Appends the three histograms with latency totals and operation counts.
     */
    void append(BSONObjBuilder* builder) const;

private:
    struct HistogramData {
        std::array<uint64_t, kMaxBuckets> buckets{};
        uint64_t entryCount = 0;
        uint64_t sum = 0;
    };

    static int _getBucket(uint64_t latency);

    static uint64_t _getBucketMicros(int bucket);

    void _append(const HistogramData& data, const char* key, BSONObjBuilder* builder) const;

    void _incrementData(uint64_t latency, int bucket, HistogramData* data);

    HistogramData _reads, _writes, _commands;
};
}  // namespace mongo
