/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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
#include <vector>

namespace mongo {

/**
 * Generalized version of OperationLatencyHistogram that can track latencies for any operation type
 * with custom lower bounds. For some provided lower bounds {x,y} a number z will be counted in the
 * x-y bucket if z ∈ [x, y). in the y-inf bucket if z ∈ [y, inf). and in the (-inf, x) bucket if z <
 * x.
 */
template <std::size_t numLowerBounds>
class IntegerHistogram {
public:
    const std::string kKey;

    IntegerHistogram(std::string key, std::array<int64_t, numLowerBounds> lowerBounds)
        : kKey(std::move(key)) {
        invariant(!lowerBounds.empty(), "Lower bounds must not be empty");
        _lowerBoundBuckets[0].lowerBound = std::numeric_limits<int64_t>::min();
        int64_t prevVal = std::numeric_limits<int64_t>::min();
        for (size_t i = 1; i < _lowerBoundBuckets.size(); ++i) {
            auto lowerBoundVal = lowerBounds.at(i - 1);
            invariant(lowerBoundVal > prevVal,
                      "Lower bounds must be strictly monotonically increasing");

            _lowerBoundBuckets[i].lowerBound = lowerBoundVal;
            prevVal = lowerBoundVal;
        }
    }

    void append(BSONObjBuilder& builder, bool shouldAppendAdditionalInfo) const {
        BSONObjBuilder histogramBuilder(builder.subobjStart(kKey));
        auto offsetToString = [this](size_t offset) {
            if (offset == 0)
                return std::string("-inf");
            if (offset < _lowerBoundBuckets.size())
                return std::to_string(_lowerBoundBuckets[offset].lowerBound);
            return std::string("inf");
        };

        for (size_t i = 0; i < _lowerBoundBuckets.size(); i++) {
            auto count = _lowerBoundBuckets[i].count.load();
            if (count == 0)
                continue;

            auto key = fmt::format("{} - {}", offsetToString(i), offsetToString(i + 1));
            BSONObjBuilder entryBuilder(histogramBuilder.subobjStart(key));
            entryBuilder.append("count", (long long)(count));
            entryBuilder.doneFast();
        }

        auto totalCount = _entryCount.load();
        histogramBuilder.append("ops", (long long)totalCount);
        if (shouldAppendAdditionalInfo && totalCount != 0) {
            auto sum = _sum.load();
            histogramBuilder.append("sum", (long long)sum);
            histogramBuilder.append("mean", static_cast<double>(sum) / totalCount);
        }
        histogramBuilder.doneFast();
    }

    void increment(int64_t data) {
        auto insertionIndex = std::upper_bound(_lowerBoundBuckets.begin(),
                                               _lowerBoundBuckets.end(),
                                               data,
                                               [](const int64_t a, const LowerBoundBucket& b) {
                                                   return a < b.lowerBound;
                                               }) -
            1;

        insertionIndex->count.addAndFetch(1);
        _entryCount.addAndFetch(1);
        _sum.addAndFetch(data);
    }

private:
    struct LowerBoundBucket {
        int64_t lowerBound;
        AtomicWord<int64_t> count;
    };

    std::array<LowerBoundBucket, numLowerBounds + 1> _lowerBoundBuckets;
    AtomicWord<int64_t> _entryCount;
    AtomicWord<int64_t> _sum;
};
}  // namespace mongo
