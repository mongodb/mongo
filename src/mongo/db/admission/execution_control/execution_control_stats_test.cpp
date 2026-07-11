// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/admission/execution_control/execution_control_stats.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/unittest.h"

#include <string_view>

namespace mongo::admission::execution_control {
namespace {
using namespace std::literals::string_view_literals;

TEST(AdmissionsHistogramTest, RecordIgnoresNonPositiveAdmissions) {
    AdmissionsHistogram histogram;

    histogram.record(0);
    histogram.record(-1);
    histogram.record(-100);

    BSONObjBuilder b;
    histogram.appendStats(b);
    BSONObj stats = b.obj();

    // All buckets should be 0.
    for (size_t i = 0; i < AdmissionsHistogram::kNumBuckets; ++i) {
        ASSERT_EQ(stats[AdmissionsHistogram::kBucketNames[i]].Long(), 0);
    }
}

TEST(AdmissionsHistogramTest, RecordBucketBoundaries) {
    struct TestCase {
        int32_t admissions;
        std::string_view expectedBucket;
    };

    std::vector<TestCase> testCases = {
        {1, "1-2"sv},       {2, "1-2"sv},       {3, "3-4"sv},        {4, "3-4"sv},
        {5, "5-8"sv},       {8, "5-8"sv},       {9, "9-16"sv},       {16, "9-16"sv},
        {17, "17-32"sv},    {32, "17-32"sv},    {33, "33-64"sv},     {64, "33-64"sv},
        {65, "65-128"sv},   {128, "65-128"sv},  {129, "129-256"sv},  {256, "129-256"sv},
        {257, "257-512"sv}, {512, "257-512"sv}, {513, "513-1024"sv}, {1024, "513-1024"sv},
        {1025, "1025+"sv},  {10000, "1025+"sv},
    };

    for (const auto& tc : testCases) {
        AdmissionsHistogram histogram;
        histogram.record(tc.admissions);

        BSONObjBuilder b;
        histogram.appendStats(b);
        BSONObj stats = b.obj();

        ASSERT_EQ(stats[tc.expectedBucket].Long(), 1)
            << "admissions=" << tc.admissions << " should be in bucket " << tc.expectedBucket;

        // All other buckets should be 0.
        for (size_t i = 0; i < AdmissionsHistogram::kNumBuckets; ++i) {
            if (AdmissionsHistogram::kBucketNames[i] != tc.expectedBucket) {
                ASSERT_EQ(stats[AdmissionsHistogram::kBucketNames[i]].Long(), 0)
                    << "admissions=" << tc.admissions << " bucket "
                    << AdmissionsHistogram::kBucketNames[i] << " should be 0";
            }
        }
    }
}

TEST(AdmissionsHistogramTest, RecordAccumulatesInBucket) {
    AdmissionsHistogram histogram;

    histogram.record(1);
    histogram.record(2);
    histogram.record(1);

    BSONObjBuilder b;
    histogram.appendStats(b);
    BSONObj stats = b.obj();

    ASSERT_EQ(stats["1-2"sv].Long(), 3);
}

TEST(AdmissionsHistogramTest, AppendStatsOutputsAllBuckets) {
    AdmissionsHistogram histogram;

    BSONObjBuilder b;
    histogram.appendStats(b);
    BSONObj stats = b.obj();

    ASSERT_EQ(static_cast<size_t>(stats.nFields()), AdmissionsHistogram::kNumBuckets);
    for (size_t i = 0; i < AdmissionsHistogram::kNumBuckets; ++i) {
        ASSERT_TRUE(stats.hasField(AdmissionsHistogram::kBucketNames[i]));
    }
}

TEST(QueueWaitTimeHistogramTest, ClampsNonPositiveWaitToDidNotWaitBucket) {
    QueueWaitTimeHistogram histogram;

    histogram.record(Microseconds{0});
    histogram.record(Microseconds{-5});

    BSONArrayBuilder arrBuilder;
    histogram.appendStats(arrBuilder);
    BSONArray arr = arrBuilder.arr();

    for (auto&& el : arr) {
        BSONObj bucket = el.Obj();
        const int64_t lowerBound = bucket["lowerBound"].Long();
        const int64_t count = bucket["count"].Long();
        const int64_t expected = lowerBound == 0 ? 2 : 0;
        ASSERT_EQ(count, expected) << "unexpected count in bucket " << lowerBound;
    }
}

TEST(QueueWaitTimeHistogramTest, AppendStatsOutputsAllBuckets) {
    QueueWaitTimeHistogram histogram;

    BSONArrayBuilder arrBuilder;
    histogram.appendStats(arrBuilder);
    BSONArray arr = arrBuilder.arr();

    // One bucket per partition, plus the implicit "did not wait" bucket below the first partition.
    std::vector<int64_t> expectedLowerBounds = {0};
    for (int64_t p : QueueWaitTimeHistogram::partitions()) {
        expectedLowerBounds.push_back(p);
    }

    ASSERT_EQ(static_cast<size_t>(arr.nFields()), expectedLowerBounds.size());

    size_t i = 0;
    for (auto&& el : arr) {
        ASSERT_EQ(el.Obj()["lowerBound"].Long(), expectedLowerBounds[i]) << "bucket index " << i;
        ++i;
    }
}

}  // namespace
}  // namespace mongo::admission::execution_control

