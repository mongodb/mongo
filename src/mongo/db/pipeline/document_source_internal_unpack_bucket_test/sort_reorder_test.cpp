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
#include "mongo/util/intrusive_counter.h"

#include <memory>
#include <vector>

namespace mongo {
namespace {

using InternalUnpackBucketSortReorderTest = AggregationContextFixture;

TEST_F(InternalUnpackBucketSortReorderTest, OptimizeForMetaSort) {
    auto unpackSpecObj = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'foo', metaField: 'meta1', "
        "bucketMaxSpanSeconds: 3600}}");
    auto sortSpecObj = fromjson("{$sort: {'meta1.a': 1, 'meta1.b': -1}}");

    auto pipeline = pipeline_factory::makePipeline(
        makeVector(unpackSpecObj, sortSpecObj), getExpCtx(), pipeline_factory::kOptionsMinimal);
    pipeline_optimization::optimizePipeline(*pipeline);

    auto serialized = pipeline->serializeToBson();

    // $sort is now before unpack bucket.
    ASSERT_EQ(2, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{ $sort: {'meta.a': 1, 'meta.b': -1} }"), serialized[0]);
    ASSERT_BSONOBJ_EQ(unpackSpecObj, serialized[1]);

    // Optimize the optimized pipeline again. We do not expect anymore rewrites to happen.
    makePipelineOptimizeAssertNoRewrites(getExpCtx(), serialized);
}

TEST_F(InternalUnpackBucketSortReorderTest, OptimizeForMetaSortNegative) {
    auto unpackSpecObj = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'foo', metaField: 'meta1', "
        "bucketMaxSpanSeconds: 3600}}");
    auto sortSpecObj = fromjson("{$sort: {'meta1.a': 1, 'unrelated': -1}}");

    auto pipeline = pipeline_factory::makePipeline(
        makeVector(unpackSpecObj, sortSpecObj), getExpCtx(), pipeline_factory::kOptionsMinimal);
    pipeline_optimization::optimizePipeline(*pipeline);

    auto serialized = pipeline->serializeToBson();

    // $sort is still before unpack bucket stage.
    ASSERT_EQ(2, serialized.size());
    ASSERT_BSONOBJ_EQ(unpackSpecObj, serialized[0]);
    ASSERT_BSONOBJ_EQ(fromjson("{$sort: {'meta1.a': 1, 'unrelated': -1}}"), serialized[1]);

    // Optimize the optimized pipeline again. We do not expect anymore rewrites to happen.
    makePipelineOptimizeAssertNoRewrites(getExpCtx(), serialized);
}

TEST_F(InternalUnpackBucketSortReorderTest, OptimizeForMetaSortLimit) {
    auto unpackSpecObj = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'foo', metaField: 'meta1', "
        "bucketMaxSpanSeconds: 3600}}");
    // The $match is necessary here to allow the sort-limit to coalesce.
    auto matchSpecObj = fromjson("{$match: {meta1: {$gt: 2}}}");
    auto sortSpecObj = fromjson("{$sort: {'meta1.a': 1, 'meta1.b': -1}}");
    auto limitSpecObj = fromjson("{$limit: 2}");

    auto pipeline = pipeline_factory::makePipeline(
        makeVector(unpackSpecObj, matchSpecObj, sortSpecObj, limitSpecObj),
        getExpCtx(),
        pipeline_factory::kOptionsMinimal);
    pipeline_optimization::optimizePipeline(*pipeline);

    auto serialized = pipeline->serializeToBson();
    auto container = pipeline->getSources();

    // $match and $sort are now before $_internalUnpackBucket, with a new $limit added before and
    // after the stage.
    ASSERT_EQ(5, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$match: {meta: {$gt: 2}}}"), serialized[0]);
    ASSERT_BSONOBJ_EQ(fromjson("{$sort: {'meta.a': 1, 'meta.b': -1}}"), serialized[1]);
    ASSERT_BSONOBJ_EQ(fromjson("{$limit: 2}"), serialized[2]);
    ASSERT_BSONOBJ_EQ(unpackSpecObj, serialized[3]);
    ASSERT_BSONOBJ_EQ(fromjson("{$limit: 2}"), serialized[4]);

    // The following assertions ensure that the first limit is absorbed by the sort. When we call
    // serializeToArray on DocumentSourceSort, it tries to pull the limit out of sort as its own
    // additional stage. The container from pipeline->getSources(), on the other hand, preserves the
    // original pipeline with limit absorbed into sort. Therefore, there should only be 4 stages
    ASSERT_EQ(4, container.size());

    // Optimize the optimized pipeline again. We do not expect anymore rewrites to happen.
    makePipelineOptimizeAssertNoRewrites(getExpCtx(), serialized);
}

TEST_F(InternalUnpackBucketSortReorderTest, OptimizeForMetaSortSkipLimit) {
    auto unpackSpecObj = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'foo', metaField: 'meta1', "
        "bucketMaxSpanSeconds: 3600}}");
    auto projectSpecObj = fromjson("{$project: {'t': '$meta1.a'}}");
    auto sortSpecObj = fromjson("{$sort: {'meta1.a': 1}}");
    auto skipSpecObj = fromjson("{$skip: 1}");
    auto limitSpecObj = fromjson("{$limit: 2}");

    auto pipeline = pipeline_factory::makePipeline(
        makeVector(unpackSpecObj, projectSpecObj, sortSpecObj, skipSpecObj, limitSpecObj),
        getExpCtx(),
        pipeline_factory::kOptionsMinimal);
    pipeline_optimization::optimizePipeline(*pipeline);

    auto serialized = pipeline->serializeToBson();

    ASSERT_EQ(5, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$addFields: {t:'$meta.a'}}"), serialized[0]);
    ASSERT_BSONOBJ_EQ(fromjson("{$sort: {'meta.a': 1}}"), serialized[1]);
    ASSERT_BSONOBJ_EQ(
        fromjson("{$_internalUnpackBucket: {include:['_id', 't'], timeField: 'foo', metaField: "
                 "'meta1', bucketMaxSpanSeconds: 3600, computedMetaProjFields: ['t'] } }"),
        serialized[2]);
    ASSERT_BSONOBJ_EQ(fromjson("{$limit: 3}"), serialized[3]);
    ASSERT_BSONOBJ_EQ(fromjson("{$skip: 1}"), serialized[4]);

    // Optimize the optimized pipeline again. More rewrites will happen here.
    auto optimizedPipeline =
        pipeline_factory::makePipeline(serialized, getExpCtx(), pipeline_factory::kOptionsMinimal);
    pipeline_optimization::optimizePipeline(*optimizedPipeline);
    auto optimizedSerialized = optimizedPipeline->serializeToBson();
    ASSERT_EQ(6, optimizedSerialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$addFields: {t:'$meta.a'}}"), optimizedSerialized[0]);
    ASSERT_BSONOBJ_EQ(fromjson("{$sort: {'meta.a': 1}}"), optimizedSerialized[1]);
    ASSERT_BSONOBJ_EQ(fromjson("{$limit: 3}"), optimizedSerialized[2]);
    ASSERT_BSONOBJ_EQ(
        fromjson("{$_internalUnpackBucket: {include:['_id', 't'], timeField: 'foo', metaField: "
                 "'meta1', bucketMaxSpanSeconds: 3600, computedMetaProjFields: ['t'] } }"),
        optimizedSerialized[3]);
    ASSERT_BSONOBJ_EQ(fromjson("{$limit: 3}"), optimizedSerialized[4]);
    ASSERT_BSONOBJ_EQ(fromjson("{$skip: 1}"), optimizedSerialized[5]);
}

TEST_F(InternalUnpackBucketSortReorderTest, OptimizeForMetaLimitSortSkipLimit) {
    auto unpackSpecObj = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'foo', metaField: 'meta1', "
        "bucketMaxSpanSeconds: 3600}}");
    auto projectSpecObj = fromjson("{$project: {'t': '$meta1.a'}}");
    auto sortSpecObj = fromjson("{$sort: {'meta1.a': 1}}");
    auto skipSpecObj = fromjson("{$skip: 1}");
    auto limitTwoSpecObj = fromjson("{$limit: 2}");
    auto limitFourSpecObj = fromjson("{$limit: 4}");

    auto pipeline = pipeline_factory::makePipeline(makeVector(unpackSpecObj,
                                                              projectSpecObj,
                                                              limitFourSpecObj,
                                                              sortSpecObj,
                                                              skipSpecObj,
                                                              limitTwoSpecObj),
                                                   getExpCtx(),
                                                   pipeline_factory::kOptionsMinimal);
    pipeline_optimization::optimizePipeline(*pipeline);

    auto serialized = pipeline->serializeToBson();

    ASSERT_EQ(7, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$addFields: {t:'$meta.a'}}"), serialized[0]);
    ASSERT_BSONOBJ_EQ(fromjson("{$limit: 4}"), serialized[1]);
    ASSERT_BSONOBJ_EQ(
        fromjson("{$_internalUnpackBucket: {include:['_id', 't'], timeField: 'foo', metaField: "
                 "'meta1', bucketMaxSpanSeconds: 3600, computedMetaProjFields: ['t'] } }"),
        serialized[2]);
    ASSERT_BSONOBJ_EQ(fromjson("{$limit: 4}"), serialized[3]);
    ASSERT_BSONOBJ_EQ(fromjson("{$sort: {'meta1.a': 1}}"), serialized[4]);
    ASSERT_BSONOBJ_EQ(fromjson("{$limit: 3}"), serialized[5]);
    ASSERT_BSONOBJ_EQ(fromjson("{$skip : 1}"), serialized[6]);

    // Optimize the optimized pipeline again. We do not expect anymore rewrites to happen.
    makePipelineOptimizeAssertNoRewrites(getExpCtx(), serialized);
}

}  // namespace
}  // namespace mongo
