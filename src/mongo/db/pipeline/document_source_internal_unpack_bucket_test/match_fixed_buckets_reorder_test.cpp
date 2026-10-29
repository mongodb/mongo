// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"
#include "mongo/db/pipeline/optimization/optimize.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <vector>

namespace mongo {
namespace {

using InternalUnpackBucketMatchFixedBucketTest = AggregationContextFixture;
}

auto unpackSpecObj = fromjson(R"({
            $_internalUnpackBucket: { 
                exclude: [], timeField: 'foo', 
                metaField: 'meta1', 
                bucketMaxSpanSeconds: 3600,
                fixedBuckets: true
            }
  })");
auto groupSpecObj = fromjson(
    "{$group: {_id: '$meta1.a.b', accmin: {$min: '$b'}, accmax: {$max: '$c'}, $willBeMerged: "
    "false}}");
auto limitSpecObj = fromjson("{$limit: 2}");

TEST_F(InternalUnpackBucketMatchFixedBucketTest, MatchWithGroupLimitRewrite) {
    // Since the buckets are fixed, and the predicate is on the timeField and aligns with the
    // bucket boundaries, the $match stage will be pushed up and there will be no _eventFilter.
    // Since the measurements do not need to be individually filtered in the $match stage, further
    // optimizations are allowed.
    Date_t date = dateFromISOString("2012-10-26T09:00:00+0000").getValue();
    auto matchSpecObj = BSON("$match" << BSON("foo" << BSON("$lt" << date)));

    // $match followed by a $group should remove the $unpack stage.
    {
        auto pipeline =
            pipeline_factory::makePipeline(makeVector(unpackSpecObj, matchSpecObj, groupSpecObj),
                                           getExpCtx(),
                                           pipeline_factory::kOptionsMinimal);
        pipeline_optimization::optimizePipeline(*pipeline);

        auto serialized = pipeline->serializeToBson();
        ASSERT_EQ(2, serialized.size());
        // The $match stage includes an ObjectId that we can't predict, so we will validate the
        // first stage is a $match stage.
        ASSERT_TRUE(serialized[0].hasField("$match"));
        auto groupOptimized = fromjson(
            "{$group: {_id: '$meta.a.b', accmin: {$min: '$control.min.b'}, accmax: {$max: "
            "'$control.max.c'}, $willBeMerged: false}}");
        ASSERT_BSONOBJ_EQ(groupOptimized, serialized[1]);
    }

    // $limit can be pushed up after a $match.
    {
        auto pipeline =
            pipeline_factory::makePipeline(makeVector(unpackSpecObj, matchSpecObj, limitSpecObj),
                                           getExpCtx(),
                                           pipeline_factory::kOptionsMinimal);
        pipeline_optimization::optimizePipeline(*pipeline);

        auto serialized = pipeline->serializeToBson();
        ASSERT_EQ(4, serialized.size());
        ASSERT_TRUE(serialized[0].hasField("$match"));
        ASSERT_BSONOBJ_EQ(limitSpecObj, serialized[1]);
        ASSERT_BSONOBJ_EQ(unpackSpecObj, serialized[2]);
        ASSERT_BSONOBJ_EQ(limitSpecObj, serialized[3]);
    }
}

TEST_F(InternalUnpackBucketMatchFixedBucketTest, MatchWithGroupLimitRewriteNegative) {
    // Even though the buckets are fixed, a $gt query must always contain an _eventFilter, even when
    // the predicate aligns with the bucket boundaries. Therefore, we expect an _eventFilter to
    // exist, which prevents further optimizations.
    Date_t date = dateFromISOString("2012-10-26T09:00:00+0000").getValue();
    auto matchSpecObj = BSON("$match" << BSON("foo" << BSON("$gt" << date)));
    auto serializedUnpackObj = fromjson(R"({
            $_internalUnpackBucket: { 
                include: ['b', 'c', 'foo', 'meta1'], timeField: 'foo', 
                metaField: 'meta1', 
                bucketMaxSpanSeconds: 3600,
                wholeBucketFilter: {'control.min.foo': { $gt: new Date(1351242000000) }},
                eventFilter: { 'foo': { $gt: new Date(1351242000000) } },
                fixedBuckets: true
            }
  })");
    // The $group stage should not replace the $unpack stage like in the previous example, because
    // the _eventFilter exists.
    {
        auto pipeline =
            pipeline_factory::makePipeline(makeVector(unpackSpecObj, matchSpecObj, groupSpecObj),
                                           getExpCtx(),
                                           pipeline_factory::kOptionsMinimal);
        pipeline_optimization::optimizePipeline(*pipeline);

        auto serialized = pipeline->serializeToBson();
        ASSERT_EQ(3, serialized.size());
        ASSERT_TRUE(serialized[0].hasField("$match"));
        ASSERT_BSONOBJ_EQ(serializedUnpackObj, serialized[1]);
        ASSERT_BSONOBJ_EQ(groupSpecObj, serialized[2]);
    }
    // The $limit stage cannot be pushed up after the $match, since the _eventFilter remains.
    {
        auto pipeline =
            pipeline_factory::makePipeline(makeVector(unpackSpecObj, matchSpecObj, limitSpecObj),
                                           getExpCtx(),
                                           pipeline_factory::kOptionsMinimal);
        pipeline_optimization::optimizePipeline(*pipeline);

        auto serialized = pipeline->serializeToBson();
        ASSERT_EQ(3, serialized.size());
        ASSERT_TRUE(serialized[0].hasField("$match"));
        auto serializedUnpackObj = fromjson(R"({
            $_internalUnpackBucket: { 
                exclude: [], timeField: 'foo', 
                metaField: 'meta1', 
                bucketMaxSpanSeconds: 3600,
                wholeBucketFilter: {'control.min.foo': { $gt: new Date(1351242000000) }},
                eventFilter: { 'foo': { $gt: new Date(1351242000000) } },
                fixedBuckets: true
            }
        })");
        ASSERT_BSONOBJ_EQ(serializedUnpackObj, serialized[1]);
        ASSERT_BSONOBJ_EQ(limitSpecObj, serialized[2]);
    }
}
TEST_F(InternalUnpackBucketMatchFixedBucketTest, ShardSideEventFilterPrunedForAlignedPredicate) {
    // Simulate a pipeline that arrived on a shard from the router.
    // The router had fixedBuckets=false, so it produced:
    //   [$match{looseBucketFilter}, $unpack{fixedBuckets:false, eventFilter:{foo < date}}]
    // populateUnpackBucketStagesFromCollection then set fixedBuckets=true.
    // doOptimize should remove the redundant eventFilter.

    // hour-aligned date: 2012-10-26T09:00:00Z is a bucket boundary for 3600s buckets.
    Date_t date = dateFromISOString("2012-10-26T09:00:00+0000").getValue();

    // The loose bucket match the router generated before the unpack stage.
    auto looseBucketMatch = BSON("$match" << BSON("control.max.foo" << BSON("$gte" << date)));

    // The unpack stage as serialized by the router: fixedBuckets=false, eventFilter present.
    auto unpackWithEventFilter =
        BSON("$_internalUnpackBucket"
             << BSON("exclude" << BSONArray() << "timeField" << "foo" << "metaField" << "meta1"
                               << "bucketMaxSpanSeconds" << 3600 << "fixedBuckets" << false
                               << "eventFilter" << BSON("foo" << BSON("$lt" << date))));

    auto pipeline =
        pipeline_factory::makePipeline(makeVector(looseBucketMatch, unpackWithEventFilter),
                                       getExpCtx(),
                                       pipeline_factory::kOptionsMinimal);

    // Simulate populateUnpackBucketStagesFromCollection setting fixedBuckets=true on the shard.
    auto& sources = pipeline->getSources();
    auto unpackIt = std::next(sources.begin());  // second stage is the unpack
    auto* unpack = dynamic_cast<DocumentSourceInternalUnpackBucket*>(unpackIt->get());
    ASSERT(unpack);
    unpack->setFixedBuckets(true);

    // Shard runs doOptimize. Expect the eventFilter to be pruned.
    pipeline_optimization::optimizePipeline(*pipeline);

    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2, serialized.size());
    auto unpackBson = serialized[1].getObjectField("$_internalUnpackBucket");
    ASSERT_FALSE(unpackBson.hasField("eventFilter"))
        << "eventFilter should be pruned when fixedBuckets=true and predicate is aligned; got: "
        << serialized[1].toString();
}

TEST_F(InternalUnpackBucketMatchFixedBucketTest,
       ShardSideWholeBucketFilterPrunedAlongsideEventFilter) {
    // Simulate a pipeline that arrived on a shard from the router with fixedBuckets=false.
    // Since the router didn't yet know the predicate would turn out to be an exact match, its
    // $match-processing pass (the one at the "Attempt to map predicates on bucketed fields"
    // block) computed both a wholeBucketFilter and an eventFilter for the non-fixed-bucket case.
    // Once the shard learns fixedBuckets=true and determines the predicate is exact, both the
    // eventFilter and the now-redundant wholeBucketFilter must be pruned.

    // hour-aligned date: 2012-10-26T09:00:00Z is a bucket boundary for 3600s buckets.
    Date_t date = dateFromISOString("2012-10-26T09:00:00+0000").getValue();

    auto looseBucketMatch = BSON("$match" << BSON("control.max.foo" << BSON("$gte" << date)));

    // The unpack stage as serialized by the router: fixedBuckets=false, both filters present.
    auto unpackWithBothFilters =
        BSON("$_internalUnpackBucket" << BSON(
                 "exclude" << BSONArray() << "timeField" << "foo" << "metaField" << "meta1"
                           << "bucketMaxSpanSeconds" << 3600 << "fixedBuckets" << false
                           << "wholeBucketFilter" << BSON("control.min.foo" << BSON("$gte" << date))
                           << "eventFilter" << BSON("foo" << BSON("$lt" << date))));

    auto pipeline =
        pipeline_factory::makePipeline(makeVector(looseBucketMatch, unpackWithBothFilters),
                                       getExpCtx(),
                                       pipeline_factory::kOptionsMinimal);

    // Simulate populateUnpackBucketStagesFromCollection setting fixedBuckets=true on the shard.
    auto& sources = pipeline->getSources();
    auto unpackIt = std::next(sources.begin());
    auto* unpack = dynamic_cast<DocumentSourceInternalUnpackBucket*>(unpackIt->get());
    ASSERT(unpack);
    unpack->setFixedBuckets(true);

    pipeline_optimization::optimizePipeline(*pipeline);

    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2, serialized.size());
    auto unpackBson = serialized[1].getObjectField("$_internalUnpackBucket");
    ASSERT_FALSE(unpackBson.hasField("eventFilter"))
        << "eventFilter should be pruned when fixedBuckets=true and predicate is aligned; got: "
        << serialized[1].toString();
    ASSERT_FALSE(unpackBson.hasField("wholeBucketFilter"))
        << "wholeBucketFilter is redundant once the eventFilter is pruned; got: "
        << serialized[1].toString();
}

TEST_F(InternalUnpackBucketMatchFixedBucketTest, ShardSideEventFilterKeptWhenExtendedRangeSet) {

    Date_t date = dateFromISOString("2012-10-26T09:00:00+0000").getValue();
    auto looseBucketMatch = BSON("$match" << BSON("control.max.foo" << BSON("$gte" << date)));
    // 'usesExtendedRange' is carried on the '$_internalUnpackBucket' BSON spec itself (it round-
    // trips into '_sharedState->_bucketUnpacker', which is the source of truth doOptimize checks),
    // rather than being set via a separate stage-level setter.
    auto unpackWithEventFilter =
        BSON("$_internalUnpackBucket"
             << BSON("exclude" << BSONArray() << "timeField" << "foo" << "metaField" << "meta1"
                               << "bucketMaxSpanSeconds" << 3600 << "fixedBuckets" << false
                               << "usesExtendedRange" << true  // shard has extended range data
                               << "eventFilter" << BSON("foo" << BSON("$lt" << date))));

    auto pipeline =
        pipeline_factory::makePipeline(makeVector(looseBucketMatch, unpackWithEventFilter),
                                       getExpCtx(),
                                       pipeline_factory::kOptionsMinimal);

    auto& sources = pipeline->getSources();
    auto unpackIt = std::next(sources.begin());
    auto* unpack = dynamic_cast<DocumentSourceInternalUnpackBucket*>(unpackIt->get());
    ASSERT(unpack);
    unpack->setFixedBuckets(true);

    pipeline_optimization::optimizePipeline(*pipeline);

    auto serialized = pipeline->serializeToBson();
    auto unpackBson = serialized.back().getObjectField("$_internalUnpackBucket");
    ASSERT_TRUE(unpackBson.hasField("eventFilter"))
        << "eventFilter must be kept when usesExtendedRange=true";
}

}  // namespace mongo
