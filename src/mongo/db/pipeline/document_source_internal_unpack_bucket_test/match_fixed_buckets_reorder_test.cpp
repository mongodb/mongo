// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/expression_context.h"
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
}  // namespace mongo
