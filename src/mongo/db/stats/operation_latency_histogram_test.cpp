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
