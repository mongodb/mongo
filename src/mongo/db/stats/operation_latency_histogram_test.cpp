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

#include <array>
#include <iostream>
#include <numeric>
#include <vector>

#include "mongo/db/commands.h"
#include "mongo/db/jsobj.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {
const int kMaxBuckets = OperationLatencyHistogram::kMaxBuckets;
const std::array<uint64_t, kMaxBuckets>& kLowerBounds = OperationLatencyHistogram::kLowerBounds;
}  // namespace

TEST(OperationLatencyHistogram, EnsureIncrementsStored) {
    OperationLatencyHistogram hist;
    for (int i = 0; i < kMaxBuckets; i++) {
        hist.increment(i, Command::ReadWriteType::kRead);
        hist.increment(i, Command::ReadWriteType::kWrite);
        hist.increment(i, Command::ReadWriteType::kCommand);
    }
    BSONObjBuilder outBuilder;
    hist.append(&outBuilder);
    BSONObj out = outBuilder.done();
    ASSERT_EQUALS(out["reads"]["ops"].Long(), kMaxBuckets);
    ASSERT_EQUALS(out["writes"]["ops"].Long(), kMaxBuckets);
    ASSERT_EQUALS(out["commands"]["ops"].Long(), kMaxBuckets);
}

TEST(OperationLatencyHistogram, CheckBucketCountsAndTotalLatency) {
    OperationLatencyHistogram hist;
    // Increment at the boundary, boundary+1, and boundary-1.
    for (int i = 0; i < kMaxBuckets; i++) {
        hist.increment(kLowerBounds[i], Command::ReadWriteType::kRead);
        hist.increment(kLowerBounds[i] + 1, Command::ReadWriteType::kRead);
        if (i > 0) {
            hist.increment(kLowerBounds[i] - 1, Command::ReadWriteType::kRead);
        }
    }

    // The additional +1 because of the first boundary.
    uint64_t expectedSum = 3 * std::accumulate(kLowerBounds.begin(), kLowerBounds.end(), 0ULL) + 1;
    BSONObjBuilder outBuilder;
    hist.append(&outBuilder);
    BSONObj out = outBuilder.done();
    ASSERT_EQUALS(static_cast<uint64_t>(out["reads"]["latency"].Long()), expectedSum);

    // Each bucket has three counts with the exception of the last bucket, which has two.
    ASSERT_EQUALS(out["reads"]["ops"].Long(), 3 * kMaxBuckets - 1);
    std::vector<BSONElement> readBuckets = out["reads"]["histogram"].Array();
    ASSERT_EQUALS(readBuckets.size(), static_cast<unsigned int>(kMaxBuckets));
    for (int i = 0; i < kMaxBuckets; i++) {
        BSONObj bucket = readBuckets[i].Obj();
        ASSERT_EQUALS(static_cast<uint64_t>(bucket["micros"].Long()), kLowerBounds[i]);
        ASSERT_EQUALS(bucket["count"].Long(), (i < kMaxBuckets - 1) ? 3 : 2);
    }
}
}  // namespace mongo
