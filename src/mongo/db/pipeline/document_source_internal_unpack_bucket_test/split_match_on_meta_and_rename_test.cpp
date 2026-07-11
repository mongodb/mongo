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

using InternalUnpackBucketSplitMatchOnMetaAndRename = AggregationContextFixture;

TEST_F(InternalUnpackBucketSplitMatchOnMetaAndRename, OptimizeSplitsMatchAndMapsControlPredicates) {
    auto unpack = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'foo', metaField: 'myMeta', "
        "bucketMaxSpanSeconds: 3600}}");
    auto pipeline = pipeline_factory::makePipeline(
        makeVector(unpack, fromjson("{$match: {myMeta: {$gte: 0, $lte: 5}, a: {$lte: 4}}}")),
        getExpCtx(),
        pipeline_factory::kOptionsMinimal);
    ASSERT_EQ(2u, pipeline->size());

    pipeline_optimization::optimizePipeline(*pipeline);

    // We should split and rename the $match. A separate optimization maps the predicate on 'a' to a
    // predicate on 'control.min.a'. These two created $match stages should be added before
    // $_internalUnpackBucket and merged.
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$match: {$and: ["
                               "  {meta: {$gte: 0}},"
                               "  {meta: {$lte: 5}},"
                               "  {$or: ["
                               "  {'control.min.a': {$_internalExprLte: 4}},"
                               "    {$expr: {$ne: [ {$type: [ \"$control.min.a\" ]},"
                               "                    {$type: [ \"$control.max.a\" ]}"
                               "    ]}}"
                               "  ]}"
                               "]}}"),
                      serialized[0]);
    ASSERT_BSONOBJ_EQ(fromjson("{ $_internalUnpackBucket: { "
                               "exclude: [], "
                               "timeField: \"foo\", "
                               "metaField: \"myMeta\", "
                               "bucketMaxSpanSeconds: 3600, "
                               "eventFilter: { a: { $lte: 4 } } } }"),
                      serialized[1]);
}

TEST_F(InternalUnpackBucketSplitMatchOnMetaAndRename, OptimizeMovesMetaMatchBeforeUnpack) {
    auto unpack = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'foo', metaField: 'myMeta', "
        "bucketMaxSpanSeconds: 3600}}");
    auto pipeline = pipeline_factory::makePipeline(
        makeVector(unpack, fromjson("{$match: {myMeta: {$gte: 0}}}")),
        getExpCtx(),
        pipeline_factory::kOptionsMinimal);
    ASSERT_EQ(2u, pipeline->size());

    pipeline_optimization::optimizePipeline(*pipeline);

    // The $match on meta is moved before $_internalUnpackBucket and no other optimization is done.
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$match: {meta: {$gte: 0}}}"), serialized[0]);
    ASSERT_BSONOBJ_EQ(unpack, serialized[1]);
}

TEST_F(InternalUnpackBucketSplitMatchOnMetaAndRename,
       OptimizeDoesNotMoveMetaMatchBeforeUnpackWithExclusionOnMeta) {
    auto unpack = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'foo', metaField: 'myMeta', "
        "bucketMaxSpanSeconds: 3600}}");
    auto pipeline = pipeline_factory::makePipeline(
        makeVector(
            unpack, fromjson("{$project: {data: 1}}"), fromjson("{$match: {myMeta: {$gte: 0}}}")),
        getExpCtx(),
        pipeline_factory::kOptionsMinimal);
    ASSERT_EQ(3u, pipeline->size());

    pipeline_optimization::optimizePipeline(*pipeline);

    // The $match on meta is not moved before $_internalUnpackBucket since the field is excluded.
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{ $_internalUnpackBucket: { include: [ '_id', 'data' ], "
                               "timeField: 'foo', metaField: 'myMeta', bucketMaxSpanSeconds: "
                               "3600} }"),
                      serialized[0]);
    ASSERT_BSONOBJ_EQ(fromjson("{$match: {myMeta: {$gte: 0}}}"), serialized[1]);
}

TEST_F(InternalUnpackBucketSplitMatchOnMetaAndRename,
       OptimizeDoesNotErrorOnFailedSplitOfMetaMatch) {
    auto unpack = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'foo', metaField: 'myMeta', "
        "bucketMaxSpanSeconds: 3600}}");
    auto match = fromjson(
        "{$match: {$and: ["
        "  {x: {$lte: 1}},"
        "  {$or: ["
        "    {'myMeta.a': {$gt: 1}},"
        "    {y: {$lt: 1}}"
        "  ]}"
        "]}}");
    auto pipeline = pipeline_factory::makePipeline(
        makeVector(unpack, match), getExpCtx(), pipeline_factory::kOptionsMinimal);
    ASSERT_EQ(2u, pipeline->size());

    pipeline_optimization::optimizePipeline(*pipeline);

    // We should fail to split the match because of the $or clause. We should still be able to
    // map the predicate on 'x' to a predicate on the control field.
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());
    auto expected = fromjson(
        "{$match: {$and: ["
        // Result of pushing down {x: {$lte: 1}}.
        "  {$or: ["
        "    {'control.min.x': {$_internalExprLte: 1}},"
        "    {$expr: {$ne: [ {$type: [ \"$control.min.x\" ]},"
        "                    {$type: [ \"$control.max.x\" ]} ]}}"
        "  ]},"
        // Result of pushing down {$or ... myMeta.a ... y ...}.
        "  {$or: ["
        "    {'meta.a': {$gt: 1}},"
        "    {$or: ["
        "      {'control.min.y': {$_internalExprLt: 1}},"
        "      {$expr: {$ne: [ {$type: [ \"$control.min.y\" ]},"
        "                      {$type: [ \"$control.max.y\" ]} ]}}"
        "    ]}"
        "  ]}"
        "]}}");
    ASSERT_BSONOBJ_EQ(expected, serialized[0]);
    ASSERT_BSONOBJ_EQ(
        fromjson(
            "{ $_internalUnpackBucket: { "
            "exclude: [], timeField: \"foo\", metaField: \"myMeta\", bucketMaxSpanSeconds: 3600, "
            "eventFilter: { $and: [ { x: { $lte: 1 } }, { $or: [ { \"myMeta.a\": { $gt: 1 } }, { "
            "y: { $lt: 1 } } ] } ] } } }"),
        serialized[1]);
}
}  // namespace
}  // namespace mongo
