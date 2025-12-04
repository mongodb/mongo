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
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/commands.h"
#include "mongo/db/server_options.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/scopeguard.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numeric>
#include <vector>

#include <absl/strings/numbers.h>
#include <absl/strings/str_cat.h>
#include <absl/strings/strip.h>

namespace mongo {
namespace {

const int kMaxBuckets = operation_latency_histogram_details::kMaxBuckets;
const std::array<uint64_t, kMaxBuckets>& kLowerBounds =
    operation_latency_histogram_details::getLowerBounds();
const std::string kBucketNameSuffix = "μs_count";

// Returns the numeric value of a bucket based on parsing the field name.
double bucketFromFieldName(BSONElement field) {
    absl::string_view bucketName = field.fieldName();
    if (!absl::ConsumeSuffix(&bucketName, kBucketNameSuffix)) {
        FAIL(absl::StrCat("Bucket name: ",
                          bucketName,
                          " did not contain the proper suffix: ",
                          kBucketNameSuffix));
    }
    double parsedValue;
    if (!absl::SimpleAtod(bucketName, &parsedValue)) {
        FAIL(absl::StrCat("Could not parse the stripped bucket name as a double: ", bucketName));
    }
    return parsedValue;
}

TEST(OperationLatencyHistogram, EnsureIncrementsStored) {
    OperationLatencyHistogram hist;
    for (int i = 0; i < kMaxBuckets; i++) {
        hist.increment(i, Command::ReadWriteType::kRead, false);
        hist.increment(i, Command::ReadWriteType::kWrite, false);
        hist.increment(i, Command::ReadWriteType::kCommand, false);
        hist.increment(i, Command::ReadWriteType::kTransaction, false);
    }
    BSONObjBuilder outBuilder;
    hist.append(false, false, &outBuilder);
    BSONObj out = outBuilder.done();
    ASSERT_EQUALS(out["reads"]["ops"].Long(), kMaxBuckets);
    ASSERT_EQUALS(out["writes"]["ops"].Long(), kMaxBuckets);
    ASSERT_EQUALS(out["commands"]["ops"].Long(), kMaxBuckets);
    ASSERT_EQUALS(out["transactions"]["ops"].Long(), kMaxBuckets);
    // There should be no latencies.
    ASSERT_TRUE(out["reads"]["latencies"].eoo());
    ASSERT_TRUE(out["writes"]["latencies"].eoo());
    ASSERT_TRUE(out["commands"]["latencies"].eoo());
    ASSERT_TRUE(out["transactions"]["latencies"].eoo());
}

TEST(OperationLatencyHistogram, CheckBucketCountsAndTotalLatency) {
    OperationLatencyHistogram hist;
    // Increment at the boundary, boundary+1, and boundary-1.
    for (int i = 0; i < kMaxBuckets; i++) {
        hist.increment(kLowerBounds[i], Command::ReadWriteType::kRead, false);
        hist.increment(kLowerBounds[i] + 1, Command::ReadWriteType::kRead, false);
        if (i > 0) {
            hist.increment(kLowerBounds[i] - 1, Command::ReadWriteType::kRead, false);
        }
    }

    // The additional +1 because of the first boundary.
    uint64_t expectedSum = 3 * std::accumulate(kLowerBounds.begin(), kLowerBounds.end(), 0ULL) + 1;
    BSONObjBuilder outBuilder;
    hist.append(true, false, &outBuilder);
    BSONObj out = outBuilder.done();
    ASSERT_EQUALS(static_cast<uint64_t>(out["reads"]["latency"].Long()), expectedSum);
    ASSERT_EQUALS(static_cast<uint64_t>(out["reads"]["queryableEncryptionLatencyMicros"].Long()),
                  0);

    // Each bucket has three counts with the exception of the last bucket, which has two.
    ASSERT_EQUALS(out["reads"]["ops"].Long(), 3 * kMaxBuckets - 1);
    BSONObj readBuckets = out["reads"]["histogram"].Obj();
    ASSERT_EQUALS(readBuckets.nFields(), static_cast<unsigned int>(kMaxBuckets));
    size_t bucketIndex = 0;
    for (const BSONElement field : readBuckets) {
        // The parsed value should be close to the bucket value but may be slightly different since
        // it takes a fixed number of characters.
        ASSERT_APPROX_EQUAL(bucketFromFieldName(field),
                            static_cast<int64_t>(kLowerBounds[bucketIndex]),
                            kLowerBounds[bucketIndex] / 100);
        ASSERT_EQUALS(field.Long(), (bucketIndex < kMaxBuckets - 1) ? 3 : 2);
        ++bucketIndex;
    }
}

TEST(OperationLatencyHistogram, CheckBucketCountsAndTotalLatencyExcludeEmptyBuckets) {
    OperationLatencyHistogram hist({.includeEmptyBuckets = false});
    // Increment at the boundary for every other bucket.
    uint64_t expectedSum = 0;
    for (int i = 0; i < kMaxBuckets; i += 2) {
        hist.increment(kLowerBounds[i], Command::ReadWriteType::kRead, false);
        expectedSum += kLowerBounds[i];
    }

    BSONObjBuilder outBuilder;
    hist.append(/*includeHistograms=*/true, /*slowMSBucketsOnly=*/false, &outBuilder);
    BSONObj out = outBuilder.done();
    ASSERT_EQUALS(static_cast<uint64_t>(out["reads"]["latency"].Long()), expectedSum);

    // Every other bucket has one count. Add one to kMaxBuckets since we start with the first
    // bucket.
    BSONObj readBuckets = out["reads"]["histogram"].Obj();
    ASSERT_EQUALS(readBuckets.nFields(), static_cast<unsigned int>((kMaxBuckets + 1) / 2));
    size_t arrayIndex = 0;
    for (const BSONElement field : readBuckets) {
        size_t bucketIndex = arrayIndex * 2;
        ASSERT_APPROX_EQUAL(bucketFromFieldName(field),
                            static_cast<int64_t>(kLowerBounds[bucketIndex]),
                            kLowerBounds[bucketIndex] / 100);
        ASSERT_EQUALS(field.Long(), 1);
        ++arrayIndex;
    }
}

TEST(OperationLatencyHistogram, CheckBucketCountsAndTotalLatencyIncludeEmptyBuckets) {
    OperationLatencyHistogram hist({.includeEmptyBuckets = true});
    // Increment at the boundary for every other bucket.
    uint64_t expectedSum = 0;
    for (int i = 0; i < kMaxBuckets; i += 2) {
        hist.increment(kLowerBounds[i], Command::ReadWriteType::kRead, false);
        expectedSum += kLowerBounds[i];
    }

    BSONObjBuilder outBuilder;
    hist.append(/*includeHistograms=*/true, /*slowMSBucketsOnly=*/false, &outBuilder);
    BSONObj out = outBuilder.done();
    ASSERT_EQUALS(static_cast<uint64_t>(out["reads"]["latency"].Long()), expectedSum);

    // Every other bucket has one count. Add one to kMaxBuckets since we start with the first
    // bucket.
    ASSERT_EQUALS(out["reads"]["ops"].Long(), (kMaxBuckets + 1) / 2);
    BSONObj readBuckets = out["reads"]["histogram"].Obj();
    ASSERT_EQUALS(readBuckets.nFields(), static_cast<unsigned int>(kMaxBuckets));
    size_t bucketIndex = 0;
    for (const BSONElement field : readBuckets) {
        ASSERT_APPROX_EQUAL(bucketFromFieldName(field),
                            static_cast<int64_t>(kLowerBounds[bucketIndex]),
                            kLowerBounds[bucketIndex] / 100);
        ASSERT_EQUALS(field.Long(), (bucketIndex % 2 == 0) ? 1 : 0);
        ++bucketIndex;
    }
}

TEST(OperationLatencyHistogram, CheckBucketCountsAndTotalLatencySlowBuckets) {
    OperationLatencyHistogram hist;
    // Increment at the boundary, boundary+1, and boundary-1.
    for (int i = 0; i < kMaxBuckets; i++) {
        hist.increment(kLowerBounds[i], Command::ReadWriteType::kRead, false);
        hist.increment(kLowerBounds[i] + 1, Command::ReadWriteType::kRead, false);
        if (i > 0) {
            hist.increment(kLowerBounds[i] - 1, Command::ReadWriteType::kRead, false);
        }
    }

    auto orig = serverGlobalParams.slowMS.load();
    serverGlobalParams.slowMS.store(100);
    ScopeGuard g1 = [orig] {
        serverGlobalParams.slowMS.store(orig);
    };

    // The additional +1 because of the first boundary.
    uint64_t expectedSum = 3 * std::accumulate(kLowerBounds.begin(), kLowerBounds.end(), 0ULL) + 1;
    BSONObjBuilder outBuilder;
    hist.append(/*includeHistograms=*/true, /*slowMSBucketsOnly=*/true, &outBuilder);
    BSONObj out = outBuilder.done();
    ASSERT_EQUALS(static_cast<uint64_t>(out["reads"]["latency"].Long()), expectedSum);

    const size_t kMaxUnFilteredBuckets = 23;
    // Each bucket has three counts with the exception of the last bucket, which has two.
    ASSERT_EQUALS(out["reads"]["ops"].Long(), 3 * kMaxBuckets - 1);
    BSONObj readBuckets = out["reads"]["histogram"].Obj();
    ASSERT_EQUALS(readBuckets.nFields(), static_cast<unsigned int>(kMaxUnFilteredBuckets + 1));
    size_t bucketIndex = 0;
    for (const BSONElement field : readBuckets) {
        ASSERT_LESS_THAN_OR_EQUALS(bucketIndex, kMaxUnFilteredBuckets);
        ASSERT_APPROX_EQUAL(bucketFromFieldName(field),
                            static_cast<int64_t>(kLowerBounds[bucketIndex]),
                            kLowerBounds[bucketIndex] / 100);
        ASSERT_EQUALS(field.Long(), (bucketIndex < kMaxUnFilteredBuckets) ? 3 : 83);
        ++bucketIndex;
    }
}

TEST(OperationLatencyHistogram, CheckBucketCountsAndTotalLatencySlowBucketsExcludeEmptyBuckets) {
    OperationLatencyHistogram hist({.includeEmptyBuckets = false});
    // Increment at the boundary for every other bucket.
    uint64_t expectedSum = 0;
    for (int i = 0; i < kMaxBuckets; i += 2) {
        hist.increment(kLowerBounds[i], Command::ReadWriteType::kRead, false);
        expectedSum += kLowerBounds[i];
    }

    auto orig = serverGlobalParams.slowMS.load();
    serverGlobalParams.slowMS.store(100);
    ScopeGuard g1 = [orig] {
        serverGlobalParams.slowMS.store(orig);
    };

    BSONObjBuilder outBuilder;
    hist.append(/*includeHistograms=*/true, /*slowMSBucketsOnly=*/true, &outBuilder);
    BSONObj out = outBuilder.done();
    ASSERT_EQUALS(static_cast<uint64_t>(out["reads"]["latency"].Long()), expectedSum);

    const size_t kMaxUnFilteredBuckets = 23;
    // Every other bucket has one count. Add one to kMaxBuckets since we start with the first
    // bucket.
    ASSERT_EQUALS(out["reads"]["ops"].Long(), (kMaxBuckets + 1) / 2);
    BSONObj readBuckets = out["reads"]["histogram"].Obj();
    ASSERT_EQUALS(readBuckets.nFields(),
                  static_cast<unsigned int>((kMaxUnFilteredBuckets + 1) / 2 + 1));
    size_t bucketIndex = 0;
    size_t arrayIndex = 0;
    for (const BSONElement field : readBuckets) {
        bucketIndex = arrayIndex * 2;
        ASSERT_LESS_THAN_OR_EQUALS(bucketIndex, kMaxUnFilteredBuckets + 1);
        ASSERT_APPROX_EQUAL(bucketFromFieldName(field),
                            static_cast<int64_t>(kLowerBounds[bucketIndex]),
                            kLowerBounds[bucketIndex] / 100);
        ASSERT_EQUALS(field.Long(), bucketIndex < kMaxUnFilteredBuckets ? 1 : 14);
        ++arrayIndex;
    }
}

TEST(OperationLatencyHistogram, CheckBucketCountsAndTotalLatencySlowBucketsIncludeEmptyBuckets) {
    OperationLatencyHistogram hist({.includeEmptyBuckets = true});
    // Increment at the boundary for every other bucket.
    uint64_t expectedSum = 0;
    for (int i = 0; i < kMaxBuckets; i += 2) {
        hist.increment(kLowerBounds[i], Command::ReadWriteType::kRead, false);
        expectedSum += kLowerBounds[i];
    }

    auto orig = serverGlobalParams.slowMS.load();
    serverGlobalParams.slowMS.store(100);
    ScopeGuard g1 = [orig] {
        serverGlobalParams.slowMS.store(orig);
    };

    BSONObjBuilder outBuilder;
    hist.append(/*includeHistograms=*/true, /*slowMSBucketsOnly=*/true, &outBuilder);
    BSONObj out = outBuilder.done();
    ASSERT_EQUALS(static_cast<uint64_t>(out["reads"]["latency"].Long()), expectedSum);

    const size_t kMaxUnFilteredBuckets = 23;
    // Every other bucket has one count. Add one to kMaxBuckets since we start with the first
    // bucket.
    ASSERT_EQUALS(out["reads"]["ops"].Long(), (kMaxBuckets + 1) / 2);
    BSONObj readBuckets = out["reads"]["histogram"].Obj();
    ASSERT_EQUALS(readBuckets.nFields(), static_cast<unsigned int>(kMaxUnFilteredBuckets + 1));
    size_t bucketIndex = 0;
    for (const BSONElement field : readBuckets) {
        ASSERT_LESS_THAN_OR_EQUALS(bucketIndex, kMaxUnFilteredBuckets);
        ASSERT_APPROX_EQUAL(bucketFromFieldName(field),
                            static_cast<int64_t>(kLowerBounds[bucketIndex]),
                            kLowerBounds[bucketIndex] / 100);
        if (bucketIndex < static_cast<int>(kMaxUnFilteredBuckets)) {
            // Regular buckets
            ASSERT_EQUALS(field.Long(), bucketIndex % 2 == 0 ? 1 : 0);
        } else {
            // Aggregate bucket
            ASSERT_EQUALS(field.Long(), 14);
        }
        ++bucketIndex;
    }
}

TEST(OperationLatencyHistogram, CheckBucketCountsSlowBucketsIncludeEmptyBucketsHighSlowMs) {
    OperationLatencyHistogram hist({.includeEmptyBuckets = true});
    // We don't need any latencies, we just want to make sure the buckets are computed correctly.

    auto orig = serverGlobalParams.slowMS.load();
    ScopeGuard g1 = [orig] {
        serverGlobalParams.slowMS.store(orig);
    };
    // Set the slowMS to something higher than the highest bucket.
    const uint64_t kHighSlowMs = kLowerBounds[kLowerBounds.size() - 1] / 1000 + 10;
    ASSERT_GT(kHighSlowMs * 1000, kLowerBounds[kLowerBounds.size() - 1]);
    serverGlobalParams.slowMS.store(kHighSlowMs);

    BSONObjBuilder outBuilder;
    hist.append(/*includeHistograms=*/true, /*slowMSBucketsOnly=*/true, &outBuilder);
    BSONObj out = outBuilder.done();

    BSONObj readBuckets = out["reads"]["histogram"].Obj();
    ASSERT_EQUALS(readBuckets.nFields(), static_cast<unsigned int>(kMaxBuckets));
    size_t bucketIndex = 0;
    for (const BSONElement field : readBuckets) {
        ASSERT_APPROX_EQUAL(bucketFromFieldName(field),
                            static_cast<int64_t>(kLowerBounds[bucketIndex]),
                            kLowerBounds[bucketIndex] / 100);
        ++bucketIndex;
    }
}

TEST(OperationLatencyHistogram,
     CheckBucketCountsAndTotalLatencySlowBucketsExcludeEmptyBucketsLogBucketScalingFactor) {
    OperationLatencyHistogram hist({.includeEmptyBuckets = false, .logBucketScalingFactor = 3});
    // Increment at the boundary for the first and third buckets out of ever 6. The result should be
    // that the in the output histogram, bucket values are always 2.
    uint64_t expectedSum = 0;
    for (int i = 0; i < kMaxBuckets; ++i) {
        if (i % 6 == 0 || i % 6 == 2) {
            hist.increment(kLowerBounds[i], Command::ReadWriteType::kRead, false);
            expectedSum += kLowerBounds[i];
        }
    }

    auto orig = serverGlobalParams.slowMS.load();
    serverGlobalParams.slowMS.store(100);
    ScopeGuard g1 = [orig] {
        serverGlobalParams.slowMS.store(orig);
    };

    BSONObjBuilder outBuilder;
    hist.append(/*includeHistograms=*/true, /*slowMSBucketsOnly=*/true, &outBuilder);
    BSONObj out = outBuilder.done();
    ASSERT_EQUALS(static_cast<uint64_t>(out["reads"]["latency"].Long()), expectedSum);

    const size_t kMaxUnFilteredBuckets = 23;
    // Two out of six buckets has one count. Add one since if there were 1 bucket there would be 1
    // read (not zero), and because of the specific value of kMaxBuckets % 6.
    ASSERT_EQUALS(out["reads"]["ops"].Long(), kMaxBuckets / 3 + 1);
    BSONObj readBuckets = out["reads"]["histogram"].Obj();
    ASSERT_EQUALS(readBuckets.nFields(),
                  static_cast<unsigned int>(kMaxUnFilteredBuckets + 2) / 6 + 1);
    size_t arrayIndex = 0;
    for (const BSONElement field : readBuckets) {
        // The aggregate bucket is actually kMaxUnFilteredBuckets + 1, because there is no entry in
        // bucket with index kMaxUnFilteredBuckets (23), but one in the next bucket.
        size_t bucketIndex = std::min(arrayIndex * 6, kMaxUnFilteredBuckets + 1);
        ASSERT_APPROX_EQUAL(bucketFromFieldName(field),
                            static_cast<int64_t>(kLowerBounds[bucketIndex]),
                            kLowerBounds[bucketIndex] / 100);
        if (bucketIndex < static_cast<int>(kMaxUnFilteredBuckets)) {
            // Regular buckets. Because the pattern of counts is 1,0,1,0,0,0, the counts are always
            // 2. E.g., Buckets 0 and 2 are incremented, which means the first appended (0-2)
            // contains 2 values, while the next possible bucket (3-5) is skipped.
            ASSERT_EQUALS(field.Long(), 2);
        } else {
            // Aggregate bucket
            ASSERT_EQUALS(field.Long(), 10);
        }
        ++arrayIndex;
    }
}

TEST(OperationLatencyHistogram,
     CheckBucketCountsAndTotalLatencySlowBucketsIncludeEmptyBucketsLogBucketScalingFactor) {
    OperationLatencyHistogram hist({.includeEmptyBuckets = true, .logBucketScalingFactor = 3});
    // Increment at the boundary for the first and third buckets out of ever 6. The result should be
    // that the in the output histogram, bucket values alternate between 0 and 2.
    uint64_t expectedSum = 0;
    for (int i = 0; i < kMaxBuckets; ++i) {
        if (i % 6 == 0 || i % 6 == 2) {
            hist.increment(kLowerBounds[i], Command::ReadWriteType::kRead, false);
            expectedSum += kLowerBounds[i];
        }
    }

    auto orig = serverGlobalParams.slowMS.load();
    serverGlobalParams.slowMS.store(100);
    ScopeGuard g1 = [orig] {
        serverGlobalParams.slowMS.store(orig);
    };

    BSONObjBuilder outBuilder;
    hist.append(/*includeHistograms=*/true, /*slowMSBucketsOnly=*/true, &outBuilder);
    BSONObj out = outBuilder.done();
    ASSERT_EQUALS(static_cast<uint64_t>(out["reads"]["latency"].Long()), expectedSum);

    const size_t kMaxUnFilteredBuckets = 23;
    // Two out of six buckets has one count. Add one since if there were 1 bucket there would be 1
    // read (not zero), and because of the specific value of kMaxBuckets % 6.
    ASSERT_EQUALS(out["reads"]["ops"].Long(), kMaxBuckets / 3 + 1);
    BSONObj readBuckets = out["reads"]["histogram"].Obj();
    ASSERT_EQUALS(readBuckets.nFields(),
                  static_cast<unsigned int>(kMaxUnFilteredBuckets + 2) / 3 + 1);
    size_t arrayIndex = 0;
    for (const BSONElement field : readBuckets) {
        size_t bucketIndex = std::min(arrayIndex * 3, kMaxUnFilteredBuckets);
        ASSERT_APPROX_EQUAL(bucketFromFieldName(field),
                            static_cast<int64_t>(kLowerBounds[bucketIndex]),
                            kLowerBounds[bucketIndex] / 100);
        if (bucketIndex < static_cast<int>(kMaxUnFilteredBuckets)) {
            // Regular buckets. Because the pattern of counts is 1,0,1,0,0,0, the counts alternate
            // between 0 and 2. E.g., Buckets 0 and 2 are incremented, which means the first
            // appended (0-2) contains 2 values, while the next appended (3-5) contains none.
            ASSERT_EQUALS(field.Long(), bucketIndex % 6 == 0 ? 2 : 0);
        } else {
            // Aggregate bucket
            ASSERT_EQUALS(field.Long(), 10);
        }
        ++arrayIndex;
    }
}

// Returns the last field of `obj`.
BSONElement getLastElement(BSONObj& obj) {
    std::vector<BSONElement> elems;
    obj.elems(elems);
    return elems[elems.size() - 1];
}

TEST(OperationLatencyHistogram, WhichBucketIsSlowLogBucketScalingFactorExcludeEmptyBuckets) {
    auto orig = serverGlobalParams.slowMS.load();
    ScopeGuard g1 = [orig] {
        serverGlobalParams.slowMS.store(orig);
    };

    OperationLatencyHistogram hist({.includeEmptyBuckets = false, .logBucketScalingFactor = 3});
    // Add a reading above each slowMS that we'll test. Each value crosses a bucket threshold when
    // logBucketScalingFactor is 1, but not always when it is 3.
    hist.increment(4 * 1000, Command::ReadWriteType::kRead, false);
    hist.increment(5 * 1000, Command::ReadWriteType::kRead, false);
    hist.increment(7 * 1000, Command::ReadWriteType::kRead, false);

    // Start with slowMS of 3.
    serverGlobalParams.slowMS.store(3);
    constexpr int kSlowBucket3Ms = 12;
    // Sanity check
    ASSERT_LT(3 * 1000, kLowerBounds[kSlowBucket3Ms]);
    ASSERT_GT(3 * 1000, kLowerBounds[kSlowBucket3Ms - 1]);
    BSONObjBuilder outBuilder1;
    hist.append(/*includeHistograms=*/true, /*slowMSBucketsOnly=*/true, &outBuilder1);
    BSONObj out = outBuilder1.done();
    BSONObj readBuckets = out["reads"]["histogram"].Obj();
    // All points are above the slowMS threshold, so we just have the last bucket.
    ASSERT_EQUALS(readBuckets.nFields(), 1);
    BSONElement lastElement = getLastElement(readBuckets);
    ASSERT_APPROX_EQUAL(bucketFromFieldName(lastElement),
                        static_cast<int64_t>(kLowerBounds[kSlowBucket3Ms]),
                        kLowerBounds[kSlowBucket3Ms] / 100);
    ASSERT_EQ(lastElement.Long(), 3);

    // With slowMS of 4, one more bucket is added because there is now a point below the slowMS
    // threshold.
    serverGlobalParams.slowMS.store(4);
    constexpr int kSlowBucket4Ms = kSlowBucket3Ms + 1;
    // Sanity check
    ASSERT_LT(4 * 1000, kLowerBounds[kSlowBucket4Ms]);
    ASSERT_GT(4 * 1000, kLowerBounds[kSlowBucket4Ms - 1]);
    BSONObjBuilder outBuilder2;
    hist.append(/*includeHistograms=*/true, /*slowMSBucketsOnly=*/true, &outBuilder2);
    out = outBuilder2.done();
    readBuckets = out["reads"]["histogram"].Obj();
    ASSERT_EQUALS(readBuckets.nFields(), 2);
    lastElement = getLastElement(readBuckets);
    ASSERT_APPROX_EQUAL(bucketFromFieldName(lastElement),
                        static_cast<int64_t>(kLowerBounds[kSlowBucket4Ms]),
                        kLowerBounds[kSlowBucket4Ms] / 100);
    ASSERT_EQ(lastElement.Long(), 2);

    // With slowMS of 6, no buckets are added since the 5ms latency goes into the last non-aggregate
    // bucket according to the logBucketScalingFactor.
    serverGlobalParams.slowMS.store(6);
    constexpr int kSlowBucket6Ms = kSlowBucket4Ms + 1;
    // Sanity check
    ASSERT_LT(6 * 1000, kLowerBounds[kSlowBucket6Ms]);
    ASSERT_GT(6 * 1000, kLowerBounds[kSlowBucket6Ms - 1]);
    BSONObjBuilder outBuilder3;
    hist.append(/*includeHistograms=*/true, /*slowMSBucketsOnly=*/true, &outBuilder3);
    out = outBuilder3.done();
    readBuckets = out["reads"]["histogram"].Obj();
    std::cout << readBuckets << std::endl;
    ASSERT_EQUALS(readBuckets.nFields(), 2);
    lastElement = getLastElement(readBuckets);
    ASSERT_APPROX_EQUAL(bucketFromFieldName(lastElement),
                        static_cast<int64_t>(kLowerBounds[kSlowBucket6Ms]),
                        kLowerBounds[kSlowBucket6Ms] / 100);
    ASSERT_EQ(lastElement.Long(), 1);
}


TEST(OperationLatencyHistogram, WhichBucketIsSlowLogBucketScalingFactorIncludeEmptyBuckets) {
    auto orig = serverGlobalParams.slowMS.load();
    ScopeGuard g1 = [orig] {
        serverGlobalParams.slowMS.store(orig);
    };

    OperationLatencyHistogram hist({.includeEmptyBuckets = true, .logBucketScalingFactor = 3});
    // Don't need to add any data since we'll record empty buckets anyway.

    // Start with slowMS of 3.
    serverGlobalParams.slowMS.store(3);
    constexpr int kSlowBucket3Ms = 12;
    // Sanity check
    ASSERT_LT(3 * 1000, kLowerBounds[kSlowBucket3Ms]);
    ASSERT_GT(3 * 1000, kLowerBounds[kSlowBucket3Ms - 1]);
    BSONObjBuilder outBuilder1;
    hist.append(/*includeHistograms=*/true, /*slowMSBucketsOnly=*/true, &outBuilder1);
    BSONObj out = outBuilder1.done();
    BSONObj readBuckets = out["reads"]["histogram"].Obj();
    ASSERT_EQUALS(readBuckets.nFields(), 5);
    ASSERT_APPROX_EQUAL(bucketFromFieldName(getLastElement(readBuckets)),
                        static_cast<int64_t>(kLowerBounds[kSlowBucket3Ms]),
                        kLowerBounds[kSlowBucket3Ms] / 100);

    // With slowMS of 4, one more bucket is added because there is a new non-aggregate bucket
    // according to the logBucketScalingFactor.
    serverGlobalParams.slowMS.store(4);
    constexpr int kSlowBucket4Ms = kSlowBucket3Ms + 1;
    // Sanity check
    ASSERT_LT(4 * 1000, kLowerBounds[kSlowBucket4Ms]);
    ASSERT_GT(4 * 1000, kLowerBounds[kSlowBucket4Ms - 1]);
    BSONObjBuilder outBuilder2;
    hist.append(/*includeHistograms=*/true, /*slowMSBucketsOnly=*/true, &outBuilder2);
    out = outBuilder2.done();
    readBuckets = out["reads"]["histogram"].Obj();
    ASSERT_EQUALS(readBuckets.nFields(), 6);
    ASSERT_APPROX_EQUAL(bucketFromFieldName(getLastElement(readBuckets)),
                        static_cast<int64_t>(kLowerBounds[kSlowBucket4Ms]),
                        kLowerBounds[kSlowBucket4Ms] / 100);

    // With slowMS of 6, no buckets are added since the 4ms bucket gets folded into the last
    // non-aggregate bucket according to the logBucketScalingFactor.
    serverGlobalParams.slowMS.store(6);
    constexpr int kSlowBucket6Ms = kSlowBucket4Ms + 1;
    // Sanity check
    ASSERT_LT(6 * 1000, kLowerBounds[kSlowBucket6Ms]);
    ASSERT_GT(6 * 1000, kLowerBounds[kSlowBucket6Ms - 1]);
    BSONObjBuilder outBuilder3;
    hist.append(/*includeHistograms=*/true, /*slowMSBucketsOnly=*/true, &outBuilder3);
    out = outBuilder3.done();
    readBuckets = out["reads"]["histogram"].Obj();
    ASSERT_EQUALS(readBuckets.nFields(), 6);
    ASSERT_APPROX_EQUAL(bucketFromFieldName(getLastElement(readBuckets)),
                        static_cast<int64_t>(kLowerBounds[kSlowBucket6Ms]),
                        kLowerBounds[kSlowBucket6Ms] / 100);
}

TEST(OperationLatencyHistogram, CheckLastBucketValueSlowBucketsExcludeEmptyBuckets) {
    // Set a specific slowMS value so we know how many histogram values will be recorded.
    auto orig = serverGlobalParams.slowMS.load();
    serverGlobalParams.slowMS.store(100);
    ScopeGuard g1 = [orig] {
        serverGlobalParams.slowMS.store(orig);
    };

    OperationLatencyHistogram hist({.includeEmptyBuckets = false});
    // Add one bucket below the slowMS threshold
    uint64_t expectedSum = 0;
    const size_t kMaxUnFilteredBuckets = 23;
    const size_t kUnfilteredBucket = 4;
    hist.increment(kLowerBounds[kUnfilteredBucket],
                   Command::ReadWriteType::kRead,
                   /*isQueryableEncryptionOp=*/false);
    expectedSum += kLowerBounds[kUnfilteredBucket];

    BSONObj out;
    BSONObjBuilder outBuilder1;
    hist.append(/*includeHistograms=*/true, /*slowMSBucketsOnly=*/true, &outBuilder1);
    out = outBuilder1.done();

    ASSERT_EQUALS(static_cast<uint64_t>(out["reads"]["latency"].Long()), expectedSum);
    ASSERT_EQUALS(out["reads"]["ops"].Long(), 1);
    // Only one entry, so just one bucket.
    BSONObj readBuckets = out["reads"]["histogram"].Obj();
    ASSERT_EQUALS(readBuckets.nFields(), 1);
    BSONElement field = readBuckets.firstElement();
    ASSERT_APPROX_EQUAL(bucketFromFieldName(field),
                        static_cast<int64_t>(kLowerBounds[kUnfilteredBucket]),
                        kLowerBounds[kUnfilteredBucket] / 100);
    ASSERT_EQUALS(field.Long(), 1);

    // Add a filtered entry.
    hist.increment(kLowerBounds[kMaxUnFilteredBuckets + 2],
                   Command::ReadWriteType::kRead,
                   /*isQueryableEncryptionOp=*/false);
    expectedSum += kLowerBounds[kMaxUnFilteredBuckets + 2];

    BSONObjBuilder outBuilder2;
    hist.append(/*includeHistograms=*/true, /*slowMSBucketsOnly=*/true, &outBuilder2);
    out = outBuilder2.done();

    ASSERT_EQUALS(static_cast<uint64_t>(out["reads"]["latency"].Long()), expectedSum);
    ASSERT_EQUALS(out["reads"]["ops"].Long(), 2);
    // Two buckets, one for the initial entry and one for the filtered entry.
    readBuckets = out["reads"]["histogram"].Obj();
    ASSERT_EQUALS(readBuckets.nFields(), 2);
    size_t bucketIndex = 0;
    for (const BSONElement field : readBuckets) {
        if (bucketIndex == 0) {
            ASSERT_APPROX_EQUAL(bucketFromFieldName(field),
                                static_cast<int64_t>(kLowerBounds[kUnfilteredBucket]),
                                kLowerBounds[kUnfilteredBucket] / 100);
        } else {
            ASSERT_APPROX_EQUAL(bucketFromFieldName(field),
                                static_cast<int64_t>(kLowerBounds[kMaxUnFilteredBuckets + 2]),
                                kLowerBounds[kMaxUnFilteredBuckets + 2] / 100);
        }
        ASSERT_EQUALS(field.Long(), 1);
        ++bucketIndex;
    }
}

TEST(OperationLatencyHistogram, CheckLastBucketValueSlowBucketsIncludeEmptyBuckets) {
    // Set a specific slowMS value so we know how many histogram values will be recorded.
    auto orig = serverGlobalParams.slowMS.load();
    serverGlobalParams.slowMS.store(100);
    ScopeGuard g1 = [orig] {
        serverGlobalParams.slowMS.store(orig);
    };

    OperationLatencyHistogram hist({.includeEmptyBuckets = true});
    // Add one bucket below the slowMS threshold
    uint64_t expectedSum = 0;
    const size_t kMaxUnFilteredBuckets = 23;
    const size_t kUnfilteredBucket = 4;
    hist.increment(kLowerBounds[kUnfilteredBucket],
                   Command::ReadWriteType::kRead,
                   /*isQueryableEncryptionOp=*/false);
    expectedSum += kLowerBounds[kUnfilteredBucket];

    BSONObj out;
    BSONObjBuilder outBuilder1;
    hist.append(/*includeHistograms=*/true, /*slowMSBucketsOnly=*/true, &outBuilder1);
    out = outBuilder1.done();

    ASSERT_EQUALS(static_cast<uint64_t>(out["reads"]["latency"].Long()), expectedSum);
    ASSERT_EQUALS(out["reads"]["ops"].Long(), 1);

    BSONObj readBuckets = out["reads"]["histogram"].Obj();
    ASSERT_EQUALS(readBuckets.nFields(), static_cast<unsigned int>(kMaxUnFilteredBuckets + 1));
    size_t bucketIndex = 0;
    for (const BSONElement field : readBuckets) {
        ASSERT_APPROX_EQUAL(bucketFromFieldName(field),
                            static_cast<int64_t>(kLowerBounds[bucketIndex]),
                            kLowerBounds[bucketIndex] / 100);
        ASSERT_EQUALS(field.Long(), bucketIndex == static_cast<int>(kUnfilteredBucket) ? 1 : 0);
        ++bucketIndex;
    }

    // Add a filtered entry.
    hist.increment(kLowerBounds[kMaxUnFilteredBuckets + 2],
                   Command::ReadWriteType::kRead,
                   /*isQueryableEncryptionOp=*/false);
    expectedSum += kLowerBounds[kMaxUnFilteredBuckets + 2];

    BSONObjBuilder outBuilder2;
    hist.append(/*includeHistograms=*/true, /*slowMSBucketsOnly=*/true, &outBuilder2);
    out = outBuilder2.done();

    ASSERT_EQUALS(static_cast<uint64_t>(out["reads"]["latency"].Long()), expectedSum);
    ASSERT_EQUALS(out["reads"]["ops"].Long(), 2);
    readBuckets = out["reads"]["histogram"].Obj();
    ASSERT_EQUALS(readBuckets.nFields(), static_cast<unsigned int>(kMaxUnFilteredBuckets + 1));
    bucketIndex = 0;
    for (const BSONElement field : readBuckets) {
        ASSERT_APPROX_EQUAL(bucketFromFieldName(field),
                            static_cast<int64_t>(kLowerBounds[bucketIndex]),
                            kLowerBounds[bucketIndex] / 100);
        bool isPopulatedBucket = (bucketIndex == static_cast<int>(kUnfilteredBucket)) ||
            (bucketIndex == static_cast<int>(kMaxUnFilteredBuckets));
        ASSERT_EQUALS(field.Long(), isPopulatedBucket ? 1 : 0);
        ++bucketIndex;
    }
}

// Check that we don't skip the last bucket value regardless of if it's a bucket we would include
// with the given log scaling factor.
TEST(OperationLatencyHistogram, CheckLastBucketValuePresentLogScalingFactor) {
    const uint64_t kHigherThanLastBucket = kLowerBounds[kMaxBuckets - 1] + 100;

    // We want one value for the following scenarios
    // - the max bucket would be the last bucket included in a set of buckets (3)
    // - the max bucket would be the first value in a new set of buckets (5)
    const std::vector<int> kLogScalingFactors{3, 5};
    ASSERT_EQ((kMaxBuckets - 1) % kLogScalingFactors[0], kLogScalingFactors[0] - 1);
    ASSERT_EQ((kMaxBuckets - 1) % kLogScalingFactors[1], 0);


    for (int logScalingFactor : kLogScalingFactors) {
        OperationLatencyHistogram hist(
            {.includeEmptyBuckets = false, .logBucketScalingFactor = logScalingFactor});
        hist.increment(kHigherThanLastBucket,
                       Command::ReadWriteType::kRead,
                       /*isQueryableEncryptionOp=*/false);

        BSONObj out;
        BSONObjBuilder outBuilder;
        hist.append(/*includeHistograms=*/true, /*slowMSBucketsOnly=*/false, &outBuilder);
        out = outBuilder.done();
        BSONObj readBuckets = out["reads"]["histogram"].Obj();

        ASSERT_EQUALS(readBuckets.nFields(), static_cast<unsigned int>(1));
        int64_t expectedBucketValue =
            kLowerBounds[kMaxBuckets - 1 - ((kMaxBuckets - 1) % logScalingFactor)];
        ASSERT_APPROX_EQUAL(bucketFromFieldName(readBuckets.firstElement()),
                            expectedBucketValue,
                            expectedBucketValue / 100);
        ASSERT_EQUALS(readBuckets.firstElement().Long(), 1);
    }
}

// QE
// Verify we count QE correctly.
TEST(OperationLatencyHistogram, CheckBucketCountsAndTotalLatencyQueryableEncryption) {
    OperationLatencyHistogram hist;
    // Increment at the boundary, boundary+1, and boundary-1.
    for (int i = 0; i < kMaxBuckets; i++) {
        hist.increment(kLowerBounds[i], Command::ReadWriteType::kRead, true);
        hist.increment(kLowerBounds[i] + 1, Command::ReadWriteType::kRead, true);
        if (i > 0) {
            hist.increment(kLowerBounds[i] - 1, Command::ReadWriteType::kRead, true);
        }
    }

    // The additional +1 because of the first boundary.
    uint64_t expectedSum = 3 * std::accumulate(kLowerBounds.begin(), kLowerBounds.end(), 0ULL) + 1;

    BSONObjBuilder outBuilder;
    hist.append(/*includeHistograms=*/true, /*slowMSBucketsOnly=*/false, &outBuilder);
    BSONObj out = outBuilder.done();
    ASSERT_EQUALS(static_cast<uint64_t>(out["reads"]["latency"].Long()), expectedSum);
    ASSERT_EQUALS(static_cast<uint64_t>(out["reads"]["queryableEncryptionLatencyMicros"].Long()),
                  expectedSum);

    // Each bucket has three counts with the exception of the last bucket, which has two.
    ASSERT_EQUALS(out["reads"]["ops"].Long(), 3 * kMaxBuckets - 1);

    BSONObj readBuckets = out["reads"]["histogram"].Obj();
    ASSERT_EQUALS(readBuckets.nFields(), static_cast<unsigned int>(kMaxBuckets));
    size_t bucketIndex = 0;
    for (const BSONElement field : readBuckets) {
        ASSERT_APPROX_EQUAL(bucketFromFieldName(field),
                            static_cast<int64_t>(kLowerBounds[bucketIndex]),
                            kLowerBounds[bucketIndex] / 100);
        ASSERT_EQUALS(field.Long(), (bucketIndex < kMaxBuckets - 1) ? 3 : 2);
        ++bucketIndex;
    }
}

}  // namespace
}  // namespace mongo
