/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/timeseries/bucket_catalog/bucket_catalog.h"
#include "mongo/unittest/unittest.h"

namespace mongo::timeseries::bucket_catalog {
/**
 * The tests in this file estimate memory usage constants for time-series deployments. If any tests
 * in this file fail, please update the constant below and update the timeseries sizing sheet.
 *
 * 8.0: WRITING-27079
 * 9.0: WRITING-27603
 */
namespace {
static constexpr uint64_t kExpectedOpenBucketSize = 872;
static constexpr uint64_t kExpectedArchivedBucketSize = 52;
static constexpr uint64_t kExpectedSchemaSizePerElement = 110;
static constexpr uint64_t kExpectedMinMaxSizePerElement = 216;
static constexpr uint64_t kExpectedBSONColumnBuilderPerElement = 552;
}  // namespace

#if !defined(MONGO_CONFIG_DEBUG_BUILD) && !defined(_MSC_VER) && !defined(__APPLE__) && \
    !__has_feature(address_sanitizer) && !defined(__s390x__)
TEST(TimeseriesSizingConstants, OpenBucket) {
    TrackingContexts contexts;
    tracking::Context registryContext;

    BucketStateRegistry registry(registryContext);
    [[maybe_unused]] auto bucket = tracking::make_unique<Bucket>(
        contexts.global,
        contexts,
        BucketId{UUID::gen(), OID{}, BucketKey::Signature{}},
        BucketKey{UUID::gen(), BucketMetadata{contexts.global, BSONElement{}, boost::none}},
        ""_sd,
        Date_t{},
        registry);

    ASSERT_EQ(contexts.global.allocated(), kExpectedOpenBucketSize);
}

TEST(TimeseriesSizingConstants, ArchivedBucket) {
    TrackingContexts contexts;
    Stripe stripe(contexts);
    uint64_t empty = contexts.global.allocated();

    auto uuid = UUID::gen();
    BucketKey::Hash hash(0);

    // Calculate the average size to minimize container overhead for a more accurate memory usage
    // per bucket.
    static constexpr int kNum = 32768;
    uint64_t memUsage = 0;
    uint64_t numBuckets = 0;
    for (int i = 0; i < kNum; ++i) {
        stripe.archivedBuckets.emplace(std::make_tuple(uuid, hash, Date_t::fromMillisSinceEpoch(i)),
                                       ArchivedBucket{});
        memUsage += contexts.global.allocated() - empty;
        numBuckets += stripe.archivedBuckets.size();
    }

    ASSERT_EQ(memUsage / numBuckets, kExpectedArchivedBucketSize);
}

TEST(TimeseriesSizingConstants, Schema) {
    tracking::Context context;
    Schema schema(context);
    uint64_t empty = context.allocated();

    // Add the smallest possible BSONElement.
    BSONObjBuilder b;
    b.appendNull("");
    BSONObj obj = b.obj();
    schema.update(obj, boost::none, nullptr);

    // Calculate overhead of storing an object in MinMax. Assuming that the element is stored once
    // internally.
    ASSERT_EQ(context.allocated() - empty - obj.firstElement().size(),
              kExpectedSchemaSizePerElement);
}

TEST(TimeseriesSizingConstants, MinMax) {
    tracking::Context context;
    MinMax mm(context);
    uint64_t empty = context.allocated();

    // Add the smallest possible BSONElement.
    BSONObjBuilder b;
    b.appendNull("");
    BSONObj obj = b.obj();
    mm.update(obj, boost::none, nullptr);

    // Calculate overhead of storing an object in MinMax. Assuming that the element is stored twice
    // internally.
    ASSERT_EQ(context.allocated() - empty - (obj.firstElement().size() * 2),
              kExpectedMinMaxSizePerElement);
}

TEST(TimeseriesSizingConstants, BSONColumnBuilder) {
    tracking::Context context;

    BSONColumnBuilder<tracking::Allocator<void>> column(context.makeAllocator<void>());

    // Estimate internal usage for BSONColumnBuilder. This depends on what data has been appended.
    // We use a fairly small delta for this estimation.
    static constexpr int kMaxColumnSize = 1000;
    static constexpr int kDelta = 3;
    // We never store an empty column
    uint64_t empty = context.allocated();
    uint64_t memUsage = 0;
    for (int i = 0; i < kMaxColumnSize; ++i) {
        {
            BSONObjBuilder b;
            // Ensure we don't use RLE internally.
            b.appendNumber(""_sd, i * kDelta + i % 2);
            column.append(b.obj().firstElement());
            // Discard any unused data.
            [[maybe_unused]] auto diff = column.intermediate();
        }
        memUsage += context.allocated();
    }

    ASSERT_EQ(sizeof(column) + (memUsage - empty) / kMaxColumnSize,
              kExpectedBSONColumnBuilderPerElement);
}

#endif

// Dummy test so we always have one test. The test framework does not like if no tests are defined.
TEST(TimeseriesSizingConstants, Noop) {}

}  // namespace mongo::timeseries::bucket_catalog
