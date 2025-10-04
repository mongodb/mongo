/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"

#include <memory>
#include <vector>

namespace mongo {
namespace {

using InternalUnpackBucketGroupReorder = AggregationContextFixture;

std::unique_ptr<Pipeline> makePipeline(boost::intrusive_ptr<mongo::ExpressionContextForTest> expCtx,
                                       std::vector<BSONObj> stages,
                                       int bucketMaxSpanSeconds,
                                       bool fixedBuckets,
                                       BSONArray fields,
                                       bool exclude) {

    BSONObjBuilder unpackSpecBuilder;
    if (exclude) {
        unpackSpecBuilder.append("exclude", fields);
    } else {
        unpackSpecBuilder.append("include", fields);
    }
    unpackSpecBuilder.append("timeField", "t");
    unpackSpecBuilder.append("metaField", "meta1");
    unpackSpecBuilder.append("bucketMaxSpanSeconds", bucketMaxSpanSeconds);
    unpackSpecBuilder.append("fixedBuckets", fixedBuckets);
    auto unpackSpecObj = BSON("$_internalUnpackBucket" << unpackSpecBuilder.obj());

    stages.insert(stages.begin(), unpackSpecObj);
    return Pipeline::parse(stages, expCtx);
}

std::vector<BSONObj> makeAndOptimizePipeline(
    boost::intrusive_ptr<mongo::ExpressionContextForTest> expCtx,
    std::vector<BSONObj> stages,
    int bucketMaxSpanSeconds,
    bool fixedBuckets,
    BSONArray fields = BSONArray(),
    bool exclude = true) {
    auto pipeline =
        makePipeline(expCtx, stages, bucketMaxSpanSeconds, fixedBuckets, fields, exclude);
    pipeline->optimizePipeline();
    return pipeline->serializeToBson();
}

// This makes a pipeline and optimizes twice, once as the router, and once as a shard. This is meant
// to simulate the path that is taken for an unsharded collection in a sharded cluster.
std::vector<BSONObj> makePipelineAndOptimizeTwice(
    boost::intrusive_ptr<mongo::ExpressionContextForTest> expCtx,
    std::vector<BSONObj> stages,
    int bucketMaxSpanSeconds,
    bool fixedBuckets,
    BSONArray fields = BSONArray(),
    bool exclude = true) {

    expCtx->setInRouter(true);
    auto pipeline =
        makePipeline(expCtx, stages, bucketMaxSpanSeconds, fixedBuckets, fields, exclude);
    pipeline->optimizePipeline();
    expCtx->setInRouter(false);
    pipeline->optimizePipeline();
    return pipeline->serializeToBson();
}

// The following tests confirm the expected behavior for the $count aggregation stage rewrite.
TEST_F(InternalUnpackBucketGroupReorder, OptimizeForCountAggStage) {
    auto countSpecObj = fromjson("{$count: 'foo'}");
    auto serialized = makeAndOptimizePipeline(
        getExpCtx(), {countSpecObj}, 3600 /* bucketMaxSpanSeconds */, false /* fixedBuckets */);

    // $count gets rewritten to $group + $project without the $unpack stage.
    ASSERT_EQ(2, serialized.size());
    auto groupOptimized = fromjson(
        "{ $group : { _id : {$const: null }, foo : { $sum : { $cond: [{$gte : [ "
        "'$control.version', {$const : 2} ]}, '$control.count', {$size : [ {$objectToArray : "
        "['$data.t']} ] } ] } }, $willBeMerged: false } }");
    ASSERT_BSONOBJ_EQ(groupOptimized, serialized[0]);
}

TEST_F(InternalUnpackBucketGroupReorder, OptimizeForCountInGroup) {
    auto groupSpecObj = fromjson("{$group: {_id: '$meta1.a.b', acccount: {$count: {} }}}");

    auto serialized = makeAndOptimizePipeline(
        getExpCtx(), {groupSpecObj}, 3600 /* bucketMaxSpanSeconds */, false /* fixedBuckets */);
    ASSERT_EQ(1, serialized.size());

    auto groupOptimized = fromjson(
        "{ $group : { _id : '$meta.a.b', acccount : { $sum : { $cond: [{$gte : [ "
        "'$control.version', {$const : 2} ]}, '$control.count', {$size : [ {$objectToArray : "
        "['$data.t']} ] } ] } }, $willBeMerged: false } }");
    ASSERT_BSONOBJ_EQ(groupOptimized, serialized[0]);
}

TEST_F(InternalUnpackBucketGroupReorder, OptimizeForCountNegative) {
    auto groupSpecObj = fromjson("{$group: {_id: '$a', s: {$sum: '$b'}}}");
    auto serialized = makeAndOptimizePipeline(getExpCtx(),
                                              {groupSpecObj},
                                              3600 /* bucketMaxSpanSeconds */,
                                              false /* fixedBuckets */,
                                              BSONArray());
    ASSERT_EQ(2, serialized.size());

    // We do not get the reorder since we are grouping on a field.
    auto optimized = fromjson(
        "{$_internalUnpackBucket: { include: ['a', 'b'], timeField: 't', metaField: 'meta1', "
        "bucketMaxSpanSeconds: 3600}}");
    ASSERT_BSONOBJ_EQ(optimized, serialized[0]);

    // Remake the pipeline and optimize again to ensure no rewrites are expected.
    makePipelineOptimizeAssertNoRewrites(getExpCtx(), serialized);
}

// The following tests confirms the $group rewrite applies when the _id field is a field path
// referencing the metaField, a constant expression, and/or for fixed buckets $dateTrunc expression
// on the timeField
TEST_F(InternalUnpackBucketGroupReorder, MinMaxGroupOnMetadata) {
    {
        auto groupSpecObj =
            fromjson("{$group: {_id: '$meta1.a.b', accmin: {$min: '$b'}, accmax: {$max: '$c'}}}");

        auto serialized = makeAndOptimizePipeline(
            getExpCtx(), {groupSpecObj}, 3600 /* bucketMaxSpanSeconds */, false /* fixedBuckets */);
        ASSERT_EQ(1, serialized.size());

        auto optimized = fromjson(
            "{$group: {_id: '$meta.a.b', accmin: {$min: '$control.min.b'}, accmax: {$max: "
            "'$control.max.c'}, $willBeMerged: false}}");
        ASSERT_BSONOBJ_EQ(optimized, serialized[0]);
    }

    {
        // Ensure that $willBeMerged is copied into the rewritten $group stage.
        auto groupSpecObj = fromjson(
            "{$group: {_id: '$meta1.a.b', accmin: {$min: '$b'}, accmax: {$max: '$c'}, "
            "$willBeMerged: "
            "true}}");

        auto serialized = makeAndOptimizePipeline(
            getExpCtx(), {groupSpecObj}, 3600 /* bucketMaxSpanSeconds */, false /* fixedBuckets */);
        ASSERT_EQ(1, serialized.size());

        // $willBeMerged will only be serialized when it is set to false & we are not already
        // merging.
        auto optimized = fromjson(
            "{$group: {_id: '$meta.a.b', accmin: {$min: '$control.min.b'}, accmax: {$max: "
            "'$control.max.c'}}}");
        ASSERT_BSONOBJ_EQ(optimized, serialized[0]);
    }
}

// Test SERVER-73822 fix: complex $min and $max (i.e. not just straight field refs) work correctly.
TEST_F(InternalUnpackBucketGroupReorder, MinMaxComplexGroupOnMetadata) {
    auto groupSpecObj = fromjson(
        "{$group: {_id: '$meta1.a.b', accmin: {$min: {$add: ['$b', {$const: 0}]}}, accmax: {$max: "
        "{$add: [{$const: 0}, '$c']}}, $willBeMerged: false }}");

    auto serialized = makeAndOptimizePipeline(
        getExpCtx(), {groupSpecObj}, 3600 /* bucketMaxSpanSeconds */, false /* fixedBuckets */);
    ASSERT_EQ(2, serialized.size());
    // Order of fields may be different between original 'unpackSpecObj' and 'serialized[0]'.
    //   ASSERT_BSONOBJ_EQ(unpackSpecObj, serialized[0]);
    ASSERT_BSONOBJ_EQ(groupSpecObj, serialized[1]);
}

TEST_F(InternalUnpackBucketGroupReorder, MinMaxGroupOnMetafield) {
    auto groupSpecObj = fromjson("{$group: {_id: '$meta1.a.b', accmin: {$min: '$meta1.f1'}}}");

    auto serialized = makeAndOptimizePipeline(
        getExpCtx(), {groupSpecObj}, 3600 /* bucketMaxSpanSeconds */, false /* fixedBuckets */);
    ASSERT_EQ(1, serialized.size());

    auto optimized =
        fromjson("{$group: {_id: '$meta.a.b', accmin: {$min: '$meta.f1'}, $willBeMerged: false }}");
    ASSERT_BSONOBJ_EQ(optimized, serialized[0]);
}

TEST_F(InternalUnpackBucketGroupReorder, MinMaxGroupOnMetafieldIdObj) {
    auto groupSpecObj =
        fromjson("{$group: {_id: { d: '$meta1.a.b' }, accmin: {$min: '$meta1.f1'}}}");

    auto serialized = makeAndOptimizePipeline(
        getExpCtx(), {groupSpecObj}, 3600 /* bucketMaxSpanSeconds */, false /* fixedBuckets */);
    ASSERT_EQ(1, serialized.size());

    auto optimized = fromjson(
        "{$group: {_id: {d: '$meta.a.b'}, accmin: {$min: '$meta.f1'}, $willBeMerged: false }}");
    ASSERT_BSONOBJ_EQ(optimized, serialized[0]);
}

TEST_F(InternalUnpackBucketGroupReorder, MinMaxDateTruncTimeField) {
    auto groupSpecObj = fromjson(
        "{$group: {_id: {time: {$dateTrunc: {date: '$t', unit: 'day'}}}, accmin: {$min: '$a'}}}");

    auto serialized = makeAndOptimizePipeline(
        getExpCtx(), {groupSpecObj}, 3600 /* bucketMaxSpanSeconds */, true /* fixedBuckets */);
    ASSERT_EQ(1, serialized.size());

    auto optimized = fromjson(
        "{$group: {_id: {time: {$dateTrunc: {date: '$control.min.t', unit: {$const: 'day'}}}}, "
        "accmin: {$min: '$control.min.a'}, $willBeMerged: false}}");
    ASSERT_BSONOBJ_EQ(optimized, serialized[0]);
}

TEST_F(InternalUnpackBucketGroupReorder, MinMaxDateTruncTimeFieldOverwrittenDoesNotOptimize) {
    auto groupSpecObj = fromjson(
        "{$group: {_id: {time: {$dateTrunc: {date: '$t', unit: {$const: 'day'}}}}, accmin: {$min: "
        "'$a'}, $willBeMerged: false }}");

    auto serialized = makeAndOptimizePipeline(getExpCtx(),
                                              {fromjson("{$project: {t: '$meta1'}}"), groupSpecObj},
                                              3600 /* bucketMaxSpanSeconds */,
                                              true /* fixedBuckets */);
    ASSERT_EQ(3, serialized.size());

    auto expectedAddFieldsStage = fromjson("{ '$addFields': { t: '$meta' } }");
    auto expectedUpackStage = fromjson(
        "{ $_internalUnpackBucket: { include: [ '_id', 't' ], timeField: 't', metaField: "
        "'meta1', bucketMaxSpanSeconds: 3600, computedMetaProjFields: [ 't' ], "
        "fixedBuckets: true } }");

    ASSERT_BSONOBJ_EQ(expectedAddFieldsStage, serialized[0]);
    ASSERT_BSONOBJ_EQ(expectedUpackStage, serialized[1]);
    ASSERT_BSONOBJ_EQ(groupSpecObj, serialized[2]);

    // Remake the pipeline and optimize again to ensure no rewrites are expected.
    makePipelineOptimizeAssertNoRewrites(getExpCtx(), serialized);
}

TEST_F(InternalUnpackBucketGroupReorder, MinMaxConstantGroupKey) {
    // Test with a null group key.
    {
        auto groupSpecObj = fromjson("{$group: {_id: null, accmin: {$min: '$meta1.f1'}}}");

        auto serialized = makeAndOptimizePipeline(
            getExpCtx(), {groupSpecObj}, 3600 /* bucketMaxSpanSeconds */, false /* fixedBuckets */);
        ASSERT_EQ(1, serialized.size());

        auto optimized = fromjson(
            "{$group: {_id: { $const: null }, accmin: {$min: '$meta.f1'}, $willBeMerged: false}}");
        ASSERT_BSONOBJ_EQ(optimized, serialized[0]);
    }
    // Test with an int group key.
    {
        auto groupSpecObj = fromjson("{$group: {_id: 0, accmin: {$min: '$meta1.f1'}}}");

        auto serialized = makeAndOptimizePipeline(
            getExpCtx(), {groupSpecObj}, 3600 /* bucketMaxSpanSeconds */, false /* fixedBuckets */);
        ASSERT_EQ(1, serialized.size());

        auto optimized = fromjson(
            "{$group: {_id:  {$const: 0}, accmin: {$min: '$meta.f1'}, $willBeMerged: false}}");
        ASSERT_BSONOBJ_EQ(optimized, serialized[0]);
    }
    // Test with an expression that is optimized to a constant.
    {
        auto groupSpecObj =
            fromjson("{$group: {_id: {$add: [2, 3]}, accmin: {$min: '$meta1.f1'}}}");

        auto serialized = makeAndOptimizePipeline(
            getExpCtx(), {groupSpecObj}, 3600 /* bucketMaxSpanSeconds */, false /* fixedBuckets */);
        ASSERT_EQ(1, serialized.size());

        auto optimized = fromjson(
            "{$group: {_id:  {$const: 5}, accmin: {$min: '$meta.f1'}, $willBeMerged: false}}");
        ASSERT_BSONOBJ_EQ(optimized, serialized[0]);
    }
    // Test with an int group key and no metaField.
    {
        auto unpackSpecObj = fromjson(
            "{$_internalUnpackBucket: { exclude: [], timeField: 't', bucketMaxSpanSeconds: 3600}}");
        auto groupSpecObj = fromjson("{$group: {_id: 0, accmin: {$min: '$meta1.f1'}}}");
        auto pipeline = Pipeline::parse(makeVector(unpackSpecObj, groupSpecObj), getExpCtx());
        pipeline->optimizePipeline();
        auto serialized = pipeline->serializeToBson();
        ASSERT_EQ(1, serialized.size());

        auto optimized = fromjson(
            "{$group: {_id:  {$const: 0}, accmin: {$min: '$control.min.meta1.f1'}, $willBeMerged: "
            "false}}");
        ASSERT_BSONOBJ_EQ(optimized, serialized[0]);
    }
}

TEST_F(InternalUnpackBucketGroupReorder, MinMaxGroupOnMultipleMetaFields) {
    auto groupSpecObj = fromjson(
        "{$group: {_id: {m1: '$meta1.m1', m2: '$meta1.m2', m3: '$meta1' }, accmin: {$min: "
        "'$meta1.f1'}}}");

    auto serialized = makeAndOptimizePipeline(
        getExpCtx(), {groupSpecObj}, 3600 /* bucketMaxSpanSeconds */, false /* fixedBuckets */);
    ASSERT_EQ(1, serialized.size());

    auto optimized = fromjson(
        "{$group: {_id: {m1: '$meta.m1', m2: '$meta.m2', m3: '$meta' }, accmin: {$min: "
        "'$meta.f1'}, $willBeMerged: false}}");
    ASSERT_BSONOBJ_EQ(optimized, serialized[0]);
}

TEST_F(InternalUnpackBucketGroupReorder, MinMaxGroupOnMultipleMetaFieldsAndConst) {
    auto groupSpecObj = fromjson(
        "{$group: {_id: {m1: 'hello', m2: '$meta1.m1', m3: '$meta1' }, accmin: {$min: "
        "'$meta1.f1'}}}");

    auto serialized = makeAndOptimizePipeline(
        getExpCtx(), {groupSpecObj}, 3600 /* bucketMaxSpanSeconds */, false /* fixedBuckets */);
    ASSERT_EQ(1, serialized.size());

    auto optimized = fromjson(
        "{$group: {_id: {m1: {$const: 'hello'}, m2: '$meta.m1', m3: '$meta' }, accmin: {$min: "
        "'$meta.f1'}, $willBeMerged: false}}");
    ASSERT_BSONOBJ_EQ(optimized, serialized[0]);
}

// The following tests demonstrate that $group rewrites for the _id field will recurse into
// arbitrary expressions.
TEST_F(InternalUnpackBucketGroupReorder, MinMaxGroupOnMetaFieldsExpression) {
    {
        auto groupSpecObj =
            fromjson("{$group: {_id: {m1: {$toUpper: '$meta1.m1'}}, accmin: {$min: '$val'}}}");
        auto serialized = makeAndOptimizePipeline(getExpCtx(), {groupSpecObj}, 3600, false);
        ASSERT_EQ(1, serialized.size());

        auto optimized = fromjson(
            "{$group: {_id: {m1: {$toUpper: [ '$meta.m1' ] }}, accmin: {$min: "
            "'$control.min.val'}, $willBeMerged: false}}");
        ASSERT_BSONOBJ_EQ(optimized, serialized[0]);
    }
    {
        auto groupSpecObj = fromjson(
            "{$group: {_id: {m1: {$concat: [{$trim: {input: {$toUpper: '$meta1.m1'}}}, '-', "
            "{$trim: {input: {$toUpper: '$meta1.m2'}}}]}}, accmin: {$min: '$val'}}}");
        auto serialized = makeAndOptimizePipeline(getExpCtx(), {groupSpecObj}, 3600, false);
        ASSERT_EQ(1, serialized.size());

        auto optimized = fromjson(
            "{$group: {_id: {m1: {$concat: [{$trim: {input: {$toUpper: [ '$meta.m1' ]}}}, "
            "{$const: '-'}, {$trim: {input: {$toUpper: [ '$meta.m2' ]}}}]}}, accmin: {$min: "
            "'$control.min.val'}, $willBeMerged: false}}");
        ASSERT_BSONOBJ_EQ(optimized, serialized[0]);
    }
}

/*
 * We can rewrite a $group on $max to reference the bucket control.max.time if there is no extended
 * range data, and we are on mongod. The flag `requiresTimeseriesExtendedRangeSupport` is not
 * accurate on mongos.
 */
TEST_F(InternalUnpackBucketGroupReorder, MaxGroupRewriteTimeField) {
    struct TestData {
        bool inRouter = false;
        bool extendedRange = false;
        bool shouldRewrite = false;
    };

    // Iterate through every test case, with inRouter true/false, and extended range true/false.
    std::vector<TestData> testCases = {
        {.inRouter = false, .extendedRange = false, .shouldRewrite = true},
        {.inRouter = false, .extendedRange = true, .shouldRewrite = false},
        {.inRouter = true, .extendedRange = false, .shouldRewrite = false},
        {.inRouter = true, .extendedRange = true, .shouldRewrite = false},
    };

    auto groupSpecObj =
        fromjson("{$group: {_id:'$meta1.m1', accmax: {$max: '$t'}, $willBeMerged: false}}");
    auto rewrittenGroupStage = fromjson(
        "{$group: {_id: '$meta.m1', accmax: {$max: '$control.max.t'}, $willBeMerged: false}}");
    auto expectedUnpackStageNoExtendedRange = fromjson(
        "{ $_internalUnpackBucket: { include: [ 't', 'meta1' ], timeField: 't', metaField: "
        "'meta1', bucketMaxSpanSeconds: 3600 } }");
    auto expectedUnpackStageExtendedRangeTrue = fromjson(
        "{ $_internalUnpackBucket: { include: [ 't', 'meta1' ], timeField: 't', metaField: "
        "'meta1', bucketMaxSpanSeconds: 3600, usesExtendedRange: true } }");

    for (auto&& testData : testCases) {
        setExpCtx({.inRouter = testData.inRouter,
                   .requiresTimeseriesExtendedRangeSupport = testData.extendedRange});
        auto serialized = makeAndOptimizePipeline(
            getExpCtx(), {groupSpecObj}, 3600 /* bucketMaxSpanSeconds */, false /* fixedBuckets */);

        if (testData.shouldRewrite) {
            // We should see the rewrite occur, since we are on mongod and do not have extended
            // range data.
            ASSERT_EQ(1, serialized.size());
            ASSERT_BSONOBJ_EQ(rewrittenGroupStage, serialized[0]);
        } else {
            // No rewrite should occur since we are on mongos or have extended range data.
            ASSERT_EQ(2, serialized.size());
            ASSERT_BSONOBJ_EQ(testData.extendedRange ? expectedUnpackStageExtendedRangeTrue
                                                     : expectedUnpackStageNoExtendedRange,
                              serialized[0]);
            ASSERT_BSONOBJ_EQ(groupSpecObj, serialized[1]);
        }
    }

    struct TestDataRouterShard {
        bool extendedRange = false;
        bool shouldRewrite = false;
    };
    // Try the "in router" cases again, this time optimizing once as the router, and once as the
    // shard, to simulate a query on an unsharded collection in a sharded cluster.
    std::vector<TestDataRouterShard> testCasesRouterShard = {
        {.extendedRange = false, .shouldRewrite = true},
        {.extendedRange = true, .shouldRewrite = false},
    };

    for (auto&& testData : testCasesRouterShard) {
        setExpCtx({.requiresTimeseriesExtendedRangeSupport = testData.extendedRange});
        auto serialized = makePipelineAndOptimizeTwice(
            getExpCtx(), {groupSpecObj}, 3600 /* bucketMaxSpanSeconds */, false /* fixedBuckets */);

        if (testData.shouldRewrite) {
            // We should see the rewrite occur, since we are on mongod and do not have extended
            // range data.
            ASSERT_EQ(1, serialized.size());
            ASSERT_BSONOBJ_EQ(rewrittenGroupStage, serialized[0]);
        } else {
            // No rewrite should occur since we are on mongos or have extended range data.
            ASSERT_EQ(2, serialized.size());
            ASSERT_BSONOBJ_EQ(testData.extendedRange ? expectedUnpackStageExtendedRangeTrue
                                                     : expectedUnpackStageNoExtendedRange,
                              serialized[0]);
            ASSERT_BSONOBJ_EQ(groupSpecObj, serialized[1]);
        }
    }
}

// The following tests confirms the $group rewrite does not apply when some requirements are not
// met.
TEST_F(InternalUnpackBucketGroupReorder, MinMaxGroupOnMetadataNegative) {
    // This rewrite does not apply because the $group stage uses the $sum accumulator.
    auto groupSpecObj = fromjson(
        "{$group: {_id: '$meta1', accmin: {$min: '$b'}, s: {$sum: '$c'}, $willBeMerged: false}}");

    auto serialized = makeAndOptimizePipeline(
        getExpCtx(), {groupSpecObj}, 3600 /* bucketMaxSpanSeconds */, false /* fixedBuckets */);
    ASSERT_EQ(2, serialized.size());

    auto unpackSpecObj = fromjson(
        "{$_internalUnpackBucket: { include: ['b', 'c', 'meta1'], timeField: 't', metaField: "
        "'meta1', bucketMaxSpanSeconds: 3600}}");
    ASSERT_BSONOBJ_EQ(unpackSpecObj, serialized[0]);
    ASSERT_BSONOBJ_EQ(groupSpecObj, serialized[1]);

    // Remake the pipeline and optimize again to ensure no rewrites are expected.
    makePipelineOptimizeAssertNoRewrites(getExpCtx(), serialized);
}

TEST_F(InternalUnpackBucketGroupReorder, MinMaxGroupOnMetadataNegative1) {
    // This rewrite does not apply because the $min accumulator is on a nested field referencing the
    // timeField.
    auto groupSpecObj =
        fromjson("{$group: {_id: '$meta1', accmin: {$min: '$t.a'}, $willBeMerged: false}}");

    auto serialized = makeAndOptimizePipeline(
        getExpCtx(), {groupSpecObj}, 3600 /* bucketMaxSpanSeconds */, false /* fixedBuckets */);
    ASSERT_EQ(2, serialized.size());

    auto unpackSpecObj = fromjson(
        "{$_internalUnpackBucket: { include: ['t', 'meta1'], timeField: 't', metaField: 'meta1', "
        "bucketMaxSpanSeconds: 3600}}");
    ASSERT_BSONOBJ_EQ(unpackSpecObj, serialized[0]);
    ASSERT_BSONOBJ_EQ(groupSpecObj, serialized[1]);

    // Remake the pipeline and optimize again to ensure no rewrites are expected.
    makePipelineOptimizeAssertNoRewrites(getExpCtx(), serialized);
}

TEST_F(InternalUnpackBucketGroupReorder, MinMaxGroupOnMetadataExpressionNegative) {
    // This rewrite does not apply because we are grouping on an expression that references a field.
    {
        auto groupSpecObj = fromjson(
            "{$group: {_id: {m1: {$toUpper: [ '$val.a' ]}}, accmin: {$min: '$val.b'}, "
            "$willBeMerged: false}}");
        auto serialized = makeAndOptimizePipeline(getExpCtx(), {groupSpecObj}, 3600, false);
        ASSERT_EQ(2, serialized.size());

        // The dependency analysis optimization will modify the the unpack spec to have include
        // field 'val'.
        auto unpackSpecObj = fromjson(
            "{$_internalUnpackBucket: { include: ['val'], timeField: 't', metaField: "
            "'meta1', bucketMaxSpanSeconds: 3600}}");
        ASSERT_BSONOBJ_EQ(unpackSpecObj, serialized[0]);
        ASSERT_BSONOBJ_EQ(groupSpecObj, serialized[1]);

        // Remake the pipeline and optimize again to ensure no rewrites are expected.
        makePipelineOptimizeAssertNoRewrites(getExpCtx(), serialized);
    }
    // This rewrite does not apply because _id.m2 references a field. Moreover, the
    // original group spec remains unchanged even though we were able to rewrite _id.m1.
    {
        auto groupSpecObj = fromjson(
            "{$group: {_id: {"
            // m1 is allowed since all field paths reference the metaField.
            "  m1: {$concat: [{$trim: {input: {$toUpper: [ '$meta1.m1' ]}}}, {$trim: {input: "
            "    {$toUpper: [ '$meta1.m2' ]}}}]},"
            // m2 is not allowed and so inhibits the optimization.
            "  m2: {$trim: {input: {$toUpper: [ '$val.a']}}}"
            "}, accmin: {$min: '$val'}, $willBeMerged: false}}");
        auto serialized = makeAndOptimizePipeline(getExpCtx(), {groupSpecObj}, 3600, false);
        ASSERT_EQ(2, serialized.size());

        auto unpackSpecObj = fromjson(
            "{$_internalUnpackBucket: { include: ['val', 'meta1'], timeField: 't', metaField: "
            "'meta1', bucketMaxSpanSeconds: 3600}}");
        ASSERT_BSONOBJ_EQ(unpackSpecObj, serialized[0]);
        ASSERT_BSONOBJ_EQ(groupSpecObj, serialized[1]);

        // Remake the pipeline and optimize again to ensure no rewrites are expected.
        makePipelineOptimizeAssertNoRewrites(getExpCtx(), serialized);
    }
    // When there is no metaField, any field path prevents rewriting the $group stage.
    {
        auto unpackSpecObj = fromjson(
            "{$_internalUnpackBucket: { include: ['a', 'meta1', 'x'], timeField: 't', "
            "bucketMaxSpanSeconds: 3600}}");
        auto groupSpecObj = fromjson(
            "{$group: {_id: {g0: {$toUpper: [ '$x' ] }}, accmin: {$min: '$meta1.f1'}, "
            "$willBeMerged: false}}");
        auto pipeline = Pipeline::parse(makeVector(unpackSpecObj, groupSpecObj), getExpCtx());
        pipeline->optimizePipeline();
        auto serialized = pipeline->serializeToBson();
        ASSERT_EQ(2, serialized.size());


        ASSERT_BSONOBJ_EQ(unpackSpecObj, serialized[0]);
        ASSERT_BSONOBJ_EQ(groupSpecObj, serialized[1]);

        // Remake the pipeline and optimize again to ensure no rewrites are expected.
        makePipelineOptimizeAssertNoRewrites(getExpCtx(), serialized);
    }
    // When there is no metaField, any field path prevents rewriting the $group stage, even if the
    // field path starts with $$CURRENT.
    {
        auto unpackSpecObj = fromjson(
            "{$_internalUnpackBucket: {include: ['a', 'meta1', 'x'], timeField: 't', "
            "bucketMaxSpanSeconds: 3600}}");
        auto groupSpecObj = fromjson(
            "{$group: {_id: {g0: {$toUpper: [ '$$CURRENT.x' ] }}, accmin: {$min: '$meta1.f1'}}}");

        auto pipeline = Pipeline::parse(makeVector(unpackSpecObj, groupSpecObj), getExpCtx());
        pipeline->optimizePipeline();
        auto serialized = pipeline->serializeToBson();

        ASSERT_EQ(2, serialized.size());

        // The $$CURRENT.x field path will be simplified to $x before it reaches the group
        // optimization.
        auto wantGroupSpecObj = fromjson(
            "{$group: {_id: {g0: {$toUpper: [ '$x' ] }}, accmin: {$min: '$meta1.f1'}, "
            "$willBeMerged: false}}");
        ASSERT_BSONOBJ_EQ(unpackSpecObj, serialized[0]);
        ASSERT_BSONOBJ_EQ(wantGroupSpecObj, serialized[1]);

        // Remake the pipeline and optimize again to ensure no rewrites are expected.
        makePipelineOptimizeAssertNoRewrites(getExpCtx(), serialized);
    }
    // When there is no metaField, any field path prevents rewriting the $group stage, even if the
    // field path starts with $$ROOT.
    {
        auto unpackSpecObj = fromjson(
            "{$_internalUnpackBucket: { exclude: [], timeField: 't', "
            "bucketMaxSpanSeconds: 3600}}");
        auto groupSpecObj = fromjson(
            "{$group: {_id: {g0: {$toUpper: [ '$$ROOT.x' ] }}, accmin: {$min: '$meta1.f1'}, "
            "$willBeMerged: false}}");

        auto pipeline = Pipeline::parse(makeVector(unpackSpecObj, groupSpecObj), getExpCtx());
        pipeline->optimizePipeline();
        auto serialized = pipeline->serializeToBson();

        ASSERT_EQ(2, serialized.size());

        auto optimizedUnpackSpec = fromjson(
            "{$_internalUnpackBucket: { include: ['meta1', 'x'], timeField: 't', "
            "bucketMaxSpanSeconds: 3600}}");
        ASSERT_BSONOBJ_EQ(optimizedUnpackSpec, serialized[0]);
        ASSERT_BSONOBJ_EQ(groupSpecObj, serialized[1]);

        // Remake the pipeline and optimize again to ensure no rewrites are expected.
        makePipelineOptimizeAssertNoRewrites(getExpCtx(), serialized);
    }
}

TEST_F(InternalUnpackBucketGroupReorder, MinMaxDateTruncTimeFieldNegative) {
    // The rewrite does not apply because the buckets are not fixed.
    {
        auto groupSpecObj = fromjson(
            "{$group: {_id: {time: {$dateTrunc: {date: '$t', unit: 'day'}}}, accmin: {$min: "
            "'$a'}}}");

        auto serialized = makeAndOptimizePipeline(
            getExpCtx(), {groupSpecObj}, 3600 /* bucketMaxSpanSeconds */, false /* fixedBuckets */);
        ASSERT_EQ(2, serialized.size());

        auto serializedGroup = fromjson(
            "{$group: {_id: {time: {$dateTrunc: {date: '$t', unit: {$const: 'day'}}}}, accmin: "
            "{$min: '$a'}, $willBeMerged: false}}");
        auto unpackSpecObj = fromjson(
            "{$_internalUnpackBucket: { include: ['a', 't'], timeField: 't', metaField: "
            "'meta1', bucketMaxSpanSeconds: 3600}}");
        ASSERT_BSONOBJ_EQ(unpackSpecObj, serialized[0]);
        ASSERT_BSONOBJ_EQ(serializedGroup, serialized[1]);

        // Remake the pipeline and optimize again to ensure no rewrites are expected.
        makePipelineOptimizeAssertNoRewrites(getExpCtx(), serialized);
    }
    // The rewrite does not apply because bucketMaxSpanSeconds is too large.
    {
        auto groupSpecObj = fromjson(
            "{$group: {_id: {time: {$dateTrunc: {date: '$t', unit: 'day'}}}, accmin: {$min: "
            "'$a'}}}");

        auto serialized = makeAndOptimizePipeline(getExpCtx(),
                                                  {groupSpecObj},
                                                  604800 /* bucketMaxSpanSeconds */,
                                                  true /* fixedBuckets */,
                                                  BSON_ARRAY("x"));
        ASSERT_EQ(2, serialized.size());

        auto unpackSpecObj = fromjson(
            "{$_internalUnpackBucket: { exclude: ['x'], timeField: 't', metaField: "
            "'meta1', bucketMaxSpanSeconds: 604800, fixedBuckets: true}}");
        auto serializedGroupObj = fromjson(
            "{$group: {_id: {time: {$dateTrunc: {date: '$t', unit: {$const: 'day'}}}}, accmin: "
            "{$min: '$a'}, $willBeMerged: false}}");
        ASSERT_BSONOBJ_EQ(unpackSpecObj, serialized[0]);
        ASSERT_BSONOBJ_EQ(serializedGroupObj, serialized[1]);

        // Remake the pipeline and optimize again to ensure no rewrites are expected.
        makePipelineOptimizeAssertNoRewrites(getExpCtx(), serialized);
    }
    // The rewrite does not apply because $dateTrunc is not on the timeField.
    {
        auto groupSpecObj = fromjson(
            "{$group: {_id: {time: {$dateTrunc: {date: '$c', unit: 'day'}}}, accmin: {$min: "
            "'$a'}}}");

        auto serialized = makeAndOptimizePipeline(getExpCtx(),
                                                  {groupSpecObj},
                                                  3600 /* bucketMaxSpanSeconds */,
                                                  true /* fixedBuckets */,
                                                  BSON_ARRAY("y"));
        ASSERT_EQ(2, serialized.size());

        auto unpackSpecObj = fromjson(
            "{$_internalUnpackBucket: { exclude: ['y'], timeField: 't', metaField: "
            "'meta1', bucketMaxSpanSeconds: 3600, fixedBuckets: true}}");
        auto serializedGroupObj = fromjson(
            "{$group: {_id: {time: {$dateTrunc: {date: '$c', unit: {$const: 'day'}}}}, accmin: "
            "{$min: '$a'}, $willBeMerged: false}}");
        ASSERT_BSONOBJ_EQ(unpackSpecObj, serialized[0]);
        ASSERT_BSONOBJ_EQ(serializedGroupObj, serialized[1]);

        // Remake the pipeline and optimize again to ensure no rewrites are expected.
        makePipelineOptimizeAssertNoRewrites(getExpCtx(), serialized);
    }
}

TEST_F(InternalUnpackBucketGroupReorder, MinMaxGroupOnMultipleMetaFieldsNegative) {
    // The rewrite does not apply, because some fields in the group key are not referencing the
    // metaField.
    auto groupSpecObj = fromjson(
        "{$group: {_id: {m1: '$meta1.m1', m2: '$val' }, accmin: {$min: '$meta1.f1'}, "
        "$willBeMerged: false}}");

    auto serialized = makeAndOptimizePipeline(
        getExpCtx(), {groupSpecObj}, 3600 /* bucketMaxSpanSeconds */, false /* fixedBuckets */);
    ASSERT_EQ(2, serialized.size());

    auto unpackSpecObj = fromjson(
        "{$_internalUnpackBucket: { include: ['val', 'meta1'],  timeField: 't', metaField: "
        "'meta1', bucketMaxSpanSeconds: 3600}}");
    ASSERT_BSONOBJ_EQ(unpackSpecObj, serialized[0]);
    ASSERT_BSONOBJ_EQ(groupSpecObj, serialized[1]);

    // Remake the pipeline and optimize again to ensure no rewrites are expected.
    makePipelineOptimizeAssertNoRewrites(getExpCtx(), serialized);
}

TEST_F(InternalUnpackBucketGroupReorder, InclusionProjectBeforeGroupNegative) {
    auto projectSpec = fromjson("{$project: {'meta1': 1}}");
    auto groupSpecObj =
        fromjson("{$group: {_id: {m1: '$meta1.m1'}, accmin: {$min: '$v'}, $willBeMerged: false}}");

    auto serialized = makeAndOptimizePipeline(getExpCtx(),
                                              {projectSpec, groupSpecObj},
                                              3600 /* bucketMaxSpanSeconds */,
                                              false /* fixedBuckets */);
    ASSERT_EQ(2, serialized.size());

    auto unpackSpecObj = fromjson(
        "{$_internalUnpackBucket: { include: ['_id', 'meta1'],  timeField: 't', metaField: "
        "'meta1', bucketMaxSpanSeconds: 3600}}");
    ASSERT_BSONOBJ_EQ(unpackSpecObj, serialized[0]);
    ASSERT_BSONOBJ_EQ(groupSpecObj, serialized[1]);

    // Remake the pipeline and optimize again to ensure no rewrites are expected.
    makePipelineOptimizeAssertNoRewrites(getExpCtx(), serialized);
}

TEST_F(InternalUnpackBucketGroupReorder, InclusionProjectBeforeGroupNotMetaNegative) {
    auto projectSpec = fromjson("{$project: {'_id' : 0, 'meta1': 1}}");
    auto groupSpecObj = fromjson("{$group: {_id: '$x', $willBeMerged: false}}");

    auto serialized = makeAndOptimizePipeline(getExpCtx(),
                                              {projectSpec, groupSpecObj},
                                              3600 /* bucketMaxSpanSeconds */,
                                              false /* fixedBuckets */);
    ASSERT_EQ(2, serialized.size());

    auto unpackSpecObj = fromjson(
        "{$_internalUnpackBucket: { include: ['meta1'],  timeField: 't', metaField: "
        "'meta1', bucketMaxSpanSeconds: 3600}}");
    ASSERT_BSONOBJ_EQ(unpackSpecObj, serialized[0]);
    ASSERT_BSONOBJ_EQ(groupSpecObj, serialized[1]);

    // Remake the pipeline and optimize again to ensure no rewrites are expected.
    makePipelineOptimizeAssertNoRewrites(getExpCtx(), serialized);
}

TEST_F(InternalUnpackBucketGroupReorder, ProjectBeforeGroupAccessSubFieldPositive) {
    auto projectSpec = fromjson("{$project: {'meta1': 1}}");
    auto groupSpecObj =
        fromjson("{$group: {_id: {m1: '$meta1.m1'}, accmin: {$min: '$v'}, $willBeMerged: false}}");

    auto serialized = makeAndOptimizePipeline(getExpCtx(),
                                              {projectSpec, groupSpecObj},
                                              3600 /* bucketMaxSpanSeconds */,
                                              false /* fixedBuckets */);
    ASSERT_EQ(2, serialized.size());

    auto unpackSpecObj = fromjson(
        "{$_internalUnpackBucket: { include: ['_id', 'meta1'],  timeField: 't', metaField: "
        "'meta1', bucketMaxSpanSeconds: 3600}}");
    ASSERT_BSONOBJ_EQ(unpackSpecObj, serialized[0]);
    ASSERT_BSONOBJ_EQ(groupSpecObj, serialized[1]);

    // Remake the pipeline and optimize again to ensure no rewrites are expected.
    makePipelineOptimizeAssertNoRewrites(getExpCtx(), serialized);
}

TEST_F(InternalUnpackBucketGroupReorder, ExclusionProjectBeforeGroupPositive) {
    auto projectSpec = fromjson("{$project: {'meta1': 0}}");
    auto groupSpecObj = fromjson("{$group: {_id: {m1: '$meta1.m1'}, accmin: {$min: '$v'}}}");

    auto serialized = makeAndOptimizePipeline(getExpCtx(),
                                              {projectSpec, groupSpecObj},
                                              3600 /* bucketMaxSpanSeconds */,
                                              false /* fixedBuckets */);
    ASSERT_EQ(2, serialized.size());

    auto expectedProjectStage = fromjson("{$project: {meta: false, _id: true}}");
    ASSERT_BSONOBJ_EQ(expectedProjectStage, serialized[0]);

    // Since the $project does not change the availability of any fields other than 'meta1', and the
    // 'meta1' just a rename of 'meta' field this rewrite is still allowed.
    auto expectedGroupStage = fromjson(
        "{$group: {_id: {m1: '$meta.m1'}, accmin: {$min: '$control.min.v'}, $willBeMerged: "
        "false}}");
    ASSERT_BSONOBJ_EQ(expectedGroupStage, serialized[1]);

    // Remake the pipeline and optimize again to ensure no rewrites are expected.
    makePipelineOptimizeAssertNoRewrites(getExpCtx(), serialized);
}

TEST_F(InternalUnpackBucketGroupReorder, ComputedMetaFieldNegative) {
    auto projectSpec = fromjson("{$project: {'t': '$meta1'}}");
    auto groupSpecObj = fromjson("{$group: {_id: {$const: null}, accmin: {$max: '$t'}}}");

    auto serialized = makeAndOptimizePipeline(getExpCtx(),
                                              {projectSpec, groupSpecObj},
                                              3600 /* bucketMaxSpanSeconds */,
                                              false /* fixedBuckets */);

    // The projection optimization adds an additional $addFields stage.
    ASSERT_EQ(3, serialized.size());

    auto expectedProjectStage = fromjson("{$addFields: {t: '$meta'}}");
    ASSERT_BSONOBJ_EQ(expectedProjectStage, serialized[0]);
    auto unpackSpecObj = fromjson(
        "{$_internalUnpackBucket: { include: ['_id', 't'],  timeField: 't', metaField: 'meta1', "
        "bucketMaxSpanSeconds: 3600, computedMetaProjFields: ['t']}}");
    ASSERT_BSONOBJ_EQ(unpackSpecObj, serialized[1]);

    // Remake the pipeline and optimize again to ensure no rewrites are expected.
    makePipelineOptimizeAssertNoRewrites(getExpCtx(), serialized);
}

}  // namespace
}  // namespace mongo
