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

using InternalUnpackBucketSortReorderTest = AggregationContextFixture;

TEST_F(InternalUnpackBucketSortReorderTest, OptimizeForMetaSort) {
    auto unpackSpecObj = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'foo', metaField: 'meta1', "
        "bucketMaxSpanSeconds: 3600}}");
    auto sortSpecObj = fromjson("{$sort: {'meta1.a': 1, 'meta1.b': -1}}");

    auto pipeline = Pipeline::parse(makeVector(unpackSpecObj, sortSpecObj), getExpCtx());
    pipeline->optimizePipeline();

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

    auto pipeline = Pipeline::parse(makeVector(unpackSpecObj, sortSpecObj), getExpCtx());
    pipeline->optimizePipeline();

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

    auto pipeline = Pipeline::parse(
        makeVector(unpackSpecObj, matchSpecObj, sortSpecObj, limitSpecObj), getExpCtx());
    pipeline->optimizePipeline();

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

    auto pipeline = Pipeline::parse(
        makeVector(unpackSpecObj, projectSpecObj, sortSpecObj, skipSpecObj, limitSpecObj),
        getExpCtx());
    pipeline->optimizePipeline();

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
    auto optimizedPipeline = Pipeline::parse(serialized, getExpCtx());
    optimizedPipeline->optimizePipeline();
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

    auto pipeline = Pipeline::parse(makeVector(unpackSpecObj,
                                               projectSpecObj,
                                               limitFourSpecObj,
                                               sortSpecObj,
                                               skipSpecObj,
                                               limitTwoSpecObj),
                                    getExpCtx());
    pipeline->optimizePipeline();

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
