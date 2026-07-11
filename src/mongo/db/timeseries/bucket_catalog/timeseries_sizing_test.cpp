// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/timeseries/bucket_catalog/bucket_catalog.h"
#include "mongo/unittest/unittest.h"

namespace mongo::timeseries::bucket_catalog {
using namespace std::literals::string_view_literals;
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
        ""sv,
        Date_t{},
        registry);

    EXPECT_EQ(contexts.global.allocated(), kExpectedOpenBucketSize);
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

    EXPECT_EQ(memUsage / numBuckets, kExpectedArchivedBucketSize);
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
    EXPECT_EQ(context.allocated() - empty - obj.firstElement().size(),
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
    EXPECT_EQ(context.allocated() - empty - (obj.firstElement().size() * 2),
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
            b.appendNumber(""sv, i * kDelta + i % 2);
            column.append(b.obj().firstElement());
            // Discard any unused data.
            [[maybe_unused]] auto diff = column.intermediate();
        }
        memUsage += context.allocated();
    }

    EXPECT_EQ(sizeof(column) + (memUsage - empty) / kMaxColumnSize,
              kExpectedBSONColumnBuilderPerElement);
}

#endif

// Dummy test so we always have one test. The test framework does not like if no tests are defined.
TEST(TimeseriesSizingConstants, Noop) {}

}  // namespace mongo::timeseries::bucket_catalog
