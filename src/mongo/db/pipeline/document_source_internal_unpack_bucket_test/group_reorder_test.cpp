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

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/query/util/make_data_structure.h"

namespace mongo {
namespace {

using InternalUnpackBucketGroupReorder = AggregationContextFixture;

std::vector<BSONObj> makeAndOptimizePipeline(
    boost::intrusive_ptr<mongo::ExpressionContextForTest> expCtx,
    std::vector<BSONObj> stages,
    int bucketMaxSpanSeconds,
    BSONArray fields = BSONArray(),
    bool exclude = true) {
    BSONObjBuilder unpackSpecBuilder;
    if (exclude) {
        unpackSpecBuilder.append("exclude", fields);
    } else {
        unpackSpecBuilder.append("include", fields);
    }
    unpackSpecBuilder.append("timeField", "t");
    unpackSpecBuilder.append("metaField", "meta1");
    unpackSpecBuilder.append("bucketMaxSpanSeconds", bucketMaxSpanSeconds);
    auto unpackSpecObj = BSON("$_internalUnpackBucket" << unpackSpecBuilder.obj());

    stages.insert(stages.begin(), unpackSpecObj);
    auto pipeline = Pipeline::parse(stages, expCtx);
    pipeline->optimizePipeline();
    return pipeline->serializeToBson();
}

TEST_F(InternalUnpackBucketGroupReorder, OptimizeForCount) {
    auto unpackSpecObj = fromjson(
        "{$_internalUnpackBucket: { include: ['a', 'b', 'c'], timeField: 't', metaField: 'meta',"
        "bucketMaxSpanSeconds: 3600}}");
    auto countSpecObj = fromjson("{$count: 'foo'}");

    auto pipeline = Pipeline::parse(makeVector(unpackSpecObj, countSpecObj), getExpCtx());
    pipeline->optimizePipeline();

    auto serialized = pipeline->serializeToBson();
    // $count gets rewritten to $group + $project.
    ASSERT_EQ(3, serialized.size());

    ASSERT_BSONOBJ_EQ(unpackSpecObj, serialized[0]);
}

TEST_F(InternalUnpackBucketGroupReorder, OptimizeForCountNegative) {
    auto unpackSpecObj = fromjson(
        "{$_internalUnpackBucket: { exclude: [], metaField: 'meta', timeField: 't', "
        "bucketMaxSpanSeconds: 3600}}");
    auto groupSpecObj = fromjson("{$group: {_id: '$a', s: {$sum: '$b'}}}");

    auto pipeline = Pipeline::parse(makeVector(unpackSpecObj, groupSpecObj), getExpCtx());
    pipeline->optimizePipeline();

    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2, serialized.size());

    // We do not get the reorder since we are grouping on a field.
    auto optimized = fromjson(
        "{$_internalUnpackBucket: { include: ['a', 'b'], timeField: 't', metaField: 'meta', "
        "bucketMaxSpanSeconds: 3600}}");
    ASSERT_BSONOBJ_EQ(optimized, serialized[0]);
}

TEST_F(InternalUnpackBucketGroupReorder, MinMaxGroupOnMetadata) {
    auto unpackSpecObj = fromjson(
        "{$_internalUnpackBucket: { exclude: [], metaField: 'meta1', timeField: 't', "
        "bucketMaxSpanSeconds: 3600}}");
    auto groupSpecObj =
        fromjson("{$group: {_id: '$meta1.a.b', accmin: {$min: '$b'}, accmax: {$max: '$c'}}}");

    auto pipeline = Pipeline::parse(makeVector(unpackSpecObj, groupSpecObj), getExpCtx());
    pipeline->optimizePipeline();

    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(1, serialized.size());

    auto optimized = fromjson(
        "{$group: {_id: '$meta.a.b', accmin: {$min: '$control.min.b'}, accmax: {$max: "
        "'$control.max.c'}}}");
    ASSERT_BSONOBJ_EQ(optimized, serialized[0]);
}

// Test SERVER-73822 fix: complex $min and $max (i.e. not just straight field refs) work correctly.
TEST_F(InternalUnpackBucketGroupReorder, MinMaxComplexGroupOnMetadata) {
    auto unpackSpecObj = fromjson(
        "{$_internalUnpackBucket: { exclude: [], metaField: 'meta1', timeField: 't', "
        "bucketMaxSpanSeconds: 3600}}");
    auto groupSpecObj = fromjson(
        "{$group: {_id: '$meta1.a.b', accmin: {$min: {$add: ['$b', {$const: 0}]}}, accmax: {$max: "
        "{$add: [{$const: 0}, '$c']}}}}");

    auto pipeline = Pipeline::parse(makeVector(unpackSpecObj, groupSpecObj), getExpCtx());
    pipeline->optimizePipeline();

    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2, serialized.size());
    // Order of fields may be different between original 'unpackSpecObj' and 'serialized[0]'.
    //   ASSERT_BSONOBJ_EQ(unpackSpecObj, serialized[0]);
    ASSERT_BSONOBJ_EQ(groupSpecObj, serialized[1]);
}

TEST_F(InternalUnpackBucketGroupReorder, MinMaxGroupOnMetafield) {
    auto unpackSpecObj = fromjson(
        "{$_internalUnpackBucket: { exclude: [], metaField: 'meta1', timeField: 't', "
        "bucketMaxSpanSeconds: 3600}}");
    auto groupSpecObj = fromjson("{$group: {_id: '$meta1.a.b', accmin: {$min: '$meta1.f1'}}}");

    auto pipeline = Pipeline::parse(makeVector(unpackSpecObj, groupSpecObj), getExpCtx());
    pipeline->optimizePipeline();

    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(1, serialized.size());

    auto optimized = fromjson("{$group: {_id: '$meta.a.b', accmin: {$min: '$meta.f1'}}}");
    ASSERT_BSONOBJ_EQ(optimized, serialized[0]);
}

TEST_F(InternalUnpackBucketGroupReorder, MinMaxGroupOnMetafieldIdObj) {
    auto groupSpecObj =
        fromjson("{$group: {_id: { d: '$meta1.a.b' }, accmin: {$min: '$meta1.f1'}}}");

    auto serialized =
        makeAndOptimizePipeline(getExpCtx(), {groupSpecObj}, 3600 /* bucketMaxSpanSeconds */);
    ASSERT_EQ(1, serialized.size());

    auto optimized = fromjson("{$group: {_id: {d: '$meta.a.b'}, accmin: {$min: '$meta.f1'}}}");
    ASSERT_BSONOBJ_EQ(optimized, serialized[0]);
}


// The following tests demonstrate that $group rewrites for the _id field will not recurse into
// arbitrary expressions.
TEST_F(InternalUnpackBucketGroupReorder, MinMaxGroupOnMetaFieldsExpressionNegative) {
    {
        auto groupSpecObj =
            fromjson("{$group: {_id: {m1: {$toUpper: '$meta1.m1'}}, accmin: {$min: '$val'}}}");
        auto serialized = makeAndOptimizePipeline(getExpCtx(), {groupSpecObj}, 3600);
        ASSERT_EQ(2, serialized.size());
    }
    {
        auto groupSpecObj = fromjson(
            "{$group: {_id: {m1: {$concat: [{$trim: {input: {$toUpper: '$meta1.m1'}}}, '-', "
            "{$trim: {input: {$toUpper: '$meta1.m2'}}}]}}, accmin: {$min: '$val'}}}");
        auto serialized = makeAndOptimizePipeline(getExpCtx(), {groupSpecObj}, 3600);
        ASSERT_EQ(2, serialized.size());
    }
}

// The following tests confirms the $group rewrite does not apply when some requirements are not
// met.
TEST_F(InternalUnpackBucketGroupReorder, MinMaxGroupOnMetadataNegative) {
    auto groupSpecObj =
        fromjson("{$group: {_id: '$meta1', accmin: {$min: '$b'}, s: {$sum: '$c'}}}");

    auto serialized =
        makeAndOptimizePipeline(getExpCtx(), {groupSpecObj}, 3600 /* bucketMaxSpanSeconds */);
    ASSERT_EQ(2, serialized.size());

    auto unpackSpecObj = fromjson(
        "{$_internalUnpackBucket: { include: ['b', 'c', 'meta1'], timeField: 't', metaField: "
        "'meta1', bucketMaxSpanSeconds: 3600}}");
    ASSERT_BSONOBJ_EQ(unpackSpecObj, serialized[0]);
    ASSERT_BSONOBJ_EQ(groupSpecObj, serialized[1]);
}

TEST_F(InternalUnpackBucketGroupReorder, MinMaxGroupOnMetadataNegative1) {
    auto groupSpecObj = fromjson("{$group: {_id: '$meta1', accmin: {$min: '$t.a'}}}");

    auto serialized =
        makeAndOptimizePipeline(getExpCtx(), {groupSpecObj}, 3600 /* bucketMaxSpanSeconds */);
    ASSERT_EQ(2, serialized.size());

    auto unpackSpecObj = fromjson(
        "{$_internalUnpackBucket: { include: ['t', 'meta1'], timeField: 't', metaField: 'meta1', "
        "bucketMaxSpanSeconds: 3600}}");
    ASSERT_BSONOBJ_EQ(unpackSpecObj, serialized[0]);
    ASSERT_BSONOBJ_EQ(groupSpecObj, serialized[1]);
}

TEST_F(InternalUnpackBucketGroupReorder, MinMaxGroupOnMetadataExpressionNegative) {
    // This rewrite does not apply because we are grouping on an expression that references a field.
    {
        auto groupSpecObj =
            fromjson("{$group: {_id: {m1: {$toUpper: [ '$val.a' ]}}, accmin: {$min: '$val.b'}}}");
        auto serialized = makeAndOptimizePipeline(getExpCtx(), {groupSpecObj}, 3600);
        ASSERT_EQ(2, serialized.size());

        // The dependency analysis optimization will modify the the unpack spec to have include
        // field 'val'.
        auto unpackSpecObj = fromjson(
            "{$_internalUnpackBucket: { include: ['val'], timeField: 't', metaField: "
            "'meta1', bucketMaxSpanSeconds: 3600}}");
        ASSERT_BSONOBJ_EQ(unpackSpecObj, serialized[0]);
        ASSERT_BSONOBJ_EQ(groupSpecObj, serialized[1]);
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
            "}, accmin: {$min: '$val'}}}");
        auto serialized = makeAndOptimizePipeline(getExpCtx(), {groupSpecObj}, 3600);
        ASSERT_EQ(2, serialized.size());

        auto unpackSpecObj = fromjson(
            "{$_internalUnpackBucket: { include: ['val', 'meta1'], timeField: 't', metaField: "
            "'meta1', bucketMaxSpanSeconds: 3600}}");
        ASSERT_BSONOBJ_EQ(unpackSpecObj, serialized[0]);
        ASSERT_BSONOBJ_EQ(groupSpecObj, serialized[1]);
    }
    // When there is no metaField, any field path prevents rewriting the $group stage.
    {
        auto unpackSpecObj = fromjson(
            "{$_internalUnpackBucket: { include: ['a', 'meta1', 'x'], timeField: 't', "
            "bucketMaxSpanSeconds: 3600}}");
        auto groupSpecObj =
            fromjson("{$group: {_id: {g0: {$toUpper: [ '$x' ] }}, accmin: {$min: '$meta1.f1'}}}");
        auto pipeline = Pipeline::parse(makeVector(unpackSpecObj, groupSpecObj), getExpCtx());
        pipeline->optimizePipeline();
        auto serialized = pipeline->serializeToBson();
        ASSERT_EQ(2, serialized.size());


        ASSERT_BSONOBJ_EQ(unpackSpecObj, serialized[0]);
        ASSERT_BSONOBJ_EQ(groupSpecObj, serialized[1]);
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
        auto wantGroupSpecObj =
            fromjson("{$group: {_id: {g0: {$toUpper: [ '$x' ] }}, accmin: {$min: '$meta1.f1'}}}");
        ASSERT_BSONOBJ_EQ(unpackSpecObj, serialized[0]);
        ASSERT_BSONOBJ_EQ(wantGroupSpecObj, serialized[1]);
    }
    // When there is no metaField, any field path prevents rewriting the $group stage, even if the
    // field path starts with $$ROOT.
    {
        auto unpackSpecObj = fromjson(
            "{$_internalUnpackBucket: { exclude: [], timeField: 't', "
            "bucketMaxSpanSeconds: 3600}}");
        auto groupSpecObj = fromjson(
            "{$group: {_id: {g0: {$toUpper: [ '$$ROOT.x' ] }}, accmin: {$min: '$meta1.f1'}}}");

        auto pipeline = Pipeline::parse(makeVector(unpackSpecObj, groupSpecObj), getExpCtx());
        pipeline->optimizePipeline();
        auto serialized = pipeline->serializeToBson();

        ASSERT_EQ(2, serialized.size());

        auto optimizedUnpackSpec = fromjson(
            "{$_internalUnpackBucket: { include: ['meta1', 'x'], timeField: 't', "
            "bucketMaxSpanSeconds: 3600}}");
        ASSERT_BSONOBJ_EQ(optimizedUnpackSpec, serialized[0]);
        ASSERT_BSONOBJ_EQ(groupSpecObj, serialized[1]);
    }
}


TEST_F(InternalUnpackBucketGroupReorder, MinMaxGroupOnMultipleMetaFieldsNegative) {
    // The rewrite does not apply, because some fields in the group key are not referencing the
    // metaField.
    auto groupSpecObj =
        fromjson("{$group: {_id: {m1: '$meta1.m1', m2: '$val' }, accmin: {$min: '$meta1.f1'}}}");

    auto serialized =
        makeAndOptimizePipeline(getExpCtx(), {groupSpecObj}, 3600 /* bucketMaxSpanSeconds */);
    ASSERT_EQ(2, serialized.size());

    auto unpackSpecObj = fromjson(
        "{$_internalUnpackBucket: { include: ['val', 'meta1'],  timeField: 't', metaField: "
        "'meta1', bucketMaxSpanSeconds: 3600}}");
    ASSERT_BSONOBJ_EQ(unpackSpecObj, serialized[0]);
    ASSERT_BSONOBJ_EQ(groupSpecObj, serialized[1]);
}

TEST_F(InternalUnpackBucketGroupReorder, InclusionProjectBeforeGroupNegative) {
    auto projectSpec = fromjson("{$project: {'meta1': 1}}");
    auto groupSpecObj = fromjson("{$group: {_id: {m1: '$meta1.m1'}, accmin: {$min: '$v'}}}");

    auto serialized = makeAndOptimizePipeline(
        getExpCtx(), {projectSpec, groupSpecObj}, 3600 /* bucketMaxSpanSeconds */);
    ASSERT_EQ(2, serialized.size());

    auto unpackSpecObj = fromjson(
        "{$_internalUnpackBucket: { include: ['_id', 'meta1'],  timeField: 't', metaField: "
        "'meta1', bucketMaxSpanSeconds: 3600}}");
    ASSERT_BSONOBJ_EQ(unpackSpecObj, serialized[0]);
    ASSERT_BSONOBJ_EQ(groupSpecObj, serialized[1]);
}

TEST_F(InternalUnpackBucketGroupReorder, ProjectBeforeGroupAccessSubFieldPositive) {
    auto projectSpec = fromjson("{$project: {'meta1': 1}}");
    auto groupSpecObj = fromjson("{$group: {_id: {m1: '$meta1.m1'}, accmin: {$min: '$v'}}}");

    auto serialized = makeAndOptimizePipeline(
        getExpCtx(), {projectSpec, groupSpecObj}, 3600 /* bucketMaxSpanSeconds */);
    ASSERT_EQ(2, serialized.size());

    auto unpackSpecObj = fromjson(
        "{$_internalUnpackBucket: { include: ['_id', 'meta1'],  timeField: 't', metaField: "
        "'meta1', bucketMaxSpanSeconds: 3600}}");
    ASSERT_BSONOBJ_EQ(unpackSpecObj, serialized[0]);
    ASSERT_BSONOBJ_EQ(groupSpecObj, serialized[1]);
}

TEST_F(InternalUnpackBucketGroupReorder, ExclusionProjectBeforeGroupPositive) {
    auto projectSpec = fromjson("{$project: {'meta1': 0}}");
    auto groupSpecObj = fromjson("{$group: {_id: {m1: '$meta1.m1'}, accmin: {$min: '$v'}}}");

    auto serialized = makeAndOptimizePipeline(
        getExpCtx(), {projectSpec, groupSpecObj}, 3600 /* bucketMaxSpanSeconds */);
    ASSERT_EQ(2, serialized.size());

    auto expectedProjectStage = fromjson("{$project: {meta: false, _id: true}}");
    ASSERT_BSONOBJ_EQ(expectedProjectStage, serialized[0]);

    // Since the $project does not change the availability of any fields other than 'meta1', and the
    // 'meta1' just a rename of 'meta' field this rewrite is still allowed.
    auto expectedGroupStage =
        fromjson("{$group: {_id: {m1: '$meta.m1'}, accmin: {$min: '$control.min.v'}}}");
    ASSERT_BSONOBJ_EQ(expectedGroupStage, serialized[1]);
}

TEST_F(InternalUnpackBucketGroupReorder, ComputedMetaFieldNegative) {
    auto projectSpec = fromjson("{$project: {'t': '$meta1'}}");
    auto groupSpecObj = fromjson("{$group: {_id: {$const: null}, accmin: {$max: '$t'}}}");

    auto serialized = makeAndOptimizePipeline(
        getExpCtx(), {projectSpec, groupSpecObj}, 3600 /* bucketMaxSpanSeconds */);

    // The projection optimization adds an additional $addFields stage.
    ASSERT_EQ(3, serialized.size());

    auto expectedProjectStage = fromjson("{$addFields: {t: '$meta'}}");
    ASSERT_BSONOBJ_EQ(expectedProjectStage, serialized[0]);
    auto unpackSpecObj = fromjson(
        "{$_internalUnpackBucket: { include: ['_id', 't'],  timeField: 't', metaField: 'meta1', "
        "bucketMaxSpanSeconds: 3600, computedMetaProjFields: ['t']}}");
    ASSERT_BSONOBJ_EQ(unpackSpecObj, serialized[1]);
}

}  // namespace
}  // namespace mongo
