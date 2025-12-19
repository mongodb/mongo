/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/admission/execution_control/execution_control_stats.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/unittest.h"

namespace mongo::admission::execution_control {
namespace {

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
        StringData expectedBucket;
    };

    std::vector<TestCase> testCases = {
        {1, "1-2"_sd},       {2, "1-2"_sd},       {3, "3-4"_sd},        {4, "3-4"_sd},
        {5, "5-8"_sd},       {8, "5-8"_sd},       {9, "9-16"_sd},       {16, "9-16"_sd},
        {17, "17-32"_sd},    {32, "17-32"_sd},    {33, "33-64"_sd},     {64, "33-64"_sd},
        {65, "65-128"_sd},   {128, "65-128"_sd},  {129, "129-256"_sd},  {256, "129-256"_sd},
        {257, "257-512"_sd}, {512, "257-512"_sd}, {513, "513-1024"_sd}, {1024, "513-1024"_sd},
        {1025, "1025+"_sd},  {10000, "1025+"_sd},
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

    ASSERT_EQ(stats["1-2"_sd].Long(), 3);
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

}  // namespace
}  // namespace mongo::admission::execution_control

