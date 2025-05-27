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
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"

#include <memory>
#include <vector>

namespace mongo {
namespace {

using InternalUnpackBucketLimitReorderTest = AggregationContextFixture;

auto unpackSpecObj = fromjson(R"({
            $_internalUnpackBucket: { 
                exclude: [], timeField: 'foo', 
                metaField: 'meta1', 
                bucketMaxSpanSeconds: 3600
            }
        })");
auto limitObj2 = fromjson("{$limit: 2}");
auto limitObj10 = fromjson("{$limit: 10}");
auto limitObj5 = fromjson("{$limit: 5}");
auto matchObj = fromjson("{$match: {'_id': 2}}");
auto sortObj = fromjson("{$sort: {'meta1.a': 1, 'meta1.b': -1}}");

// Simple test to push limit down.
TEST_F(InternalUnpackBucketLimitReorderTest, OptimizeForOnlyLimit) {
    auto pipeline = Pipeline::parse(makeVector(unpackSpecObj, limitObj2), getExpCtx());
    pipeline->optimizePipeline();

    auto serialized = pipeline->serializeToBson();

    // $limit is now before unpack bucket.
    ASSERT_EQ(3, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$limit: 2}"), serialized[0]);
    ASSERT_BSONOBJ_EQ(unpackSpecObj, serialized[1]);
    ASSERT_BSONOBJ_EQ(fromjson("{$limit: 2}"), serialized[2]);

    // Optimize the optimized pipeline again. We do not expect anymore rewrites to happen.
    makePipelineOptimizeAssertNoRewrites(getExpCtx(), serialized);
}

// Test that when there are multiple limits in a row, they are merged into one taking the smallest
// limit value ({$limit: 2} in this case) and pushed down.
TEST_F(InternalUnpackBucketLimitReorderTest, OptimizeForMultipleLimits) {
    auto pipeline =
        Pipeline::parse(makeVector(unpackSpecObj, limitObj10, limitObj2, limitObj5), getExpCtx());
    pipeline->optimizePipeline();

    auto serialized = pipeline->serializeToBson();

    ASSERT_EQ(3, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$limit: 2}"), serialized[0]);
    ASSERT_BSONOBJ_EQ(unpackSpecObj, serialized[1]);
    ASSERT_BSONOBJ_EQ(fromjson("{$limit: 2}"), serialized[2]);

    makePipelineOptimizeAssertNoRewrites(getExpCtx(), serialized);
}

// Test that the stages after $limit are also preserved.
TEST_F(InternalUnpackBucketLimitReorderTest, OptimizeForLimitWithMatch) {
    auto pipeline = Pipeline::parse(makeVector(unpackSpecObj, limitObj2, matchObj), getExpCtx());
    pipeline->optimizePipeline();

    auto serialized = pipeline->serializeToBson();

    // $limit is before unpack bucket stage.
    ASSERT_EQ(4, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$limit: 2}"), serialized[0]);
    ASSERT_BSONOBJ_EQ(unpackSpecObj, serialized[1]);
    ASSERT_BSONOBJ_EQ(fromjson("{$limit: 2}"), serialized[2]);
    ASSERT_BSONOBJ_EQ(fromjson("{$match: {'_id': 2}}"), serialized[3]);

    makePipelineOptimizeAssertNoRewrites(getExpCtx(), serialized);
}

// Test that limit is not pushed down if it comes after match.
TEST_F(InternalUnpackBucketLimitReorderTest, NoOptimizeForMatchBeforeLimit) {
    auto pipeline = Pipeline::parse(makeVector(unpackSpecObj, matchObj, limitObj2), getExpCtx());
    pipeline->optimizePipeline();

    auto serialized = pipeline->serializeToBson();

    // Using hasField rather than matching whole json to check that the stages are what we expect
    // because the match push down changes the shape of the original $match and
    // $_internalUnpackBucket.
    ASSERT_EQ(3, serialized.size());
    ASSERT(serialized[0].hasField("$match"));
    ASSERT(serialized[1].hasField("$_internalUnpackBucket"));
    ASSERT_BSONOBJ_EQ(fromjson("{$limit: 2}"), serialized[2]);

    makePipelineOptimizeAssertNoRewrites(getExpCtx(), serialized);
}

// Test that the sort that was pushed up absorbs the limit, while preserving the original limit.
TEST_F(InternalUnpackBucketLimitReorderTest, OptimizeForLimitWithSort) {

    auto pipeline = Pipeline::parse(makeVector(unpackSpecObj, sortObj, limitObj2), getExpCtx());
    pipeline->optimizePipeline();

    auto serialized = pipeline->serializeToBson();
    const auto& container = pipeline->getSources();

    // The following assertions ensure that the first limit is absorbed by the sort. The serialized
    // array has 4 stages even though the first limit is absorbed by the sort, because
    // serializeToArray adds a limit stage when the $sort has a $limit.
    ASSERT_EQ(4, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$sort: {'meta.a': 1, 'meta.b': -1}}"), serialized[0]);
    ASSERT_BSONOBJ_EQ(fromjson("{$limit: 2}"), serialized[1]);
    ASSERT_BSONOBJ_EQ(unpackSpecObj, serialized[2]);
    ASSERT_BSONOBJ_EQ(fromjson("{$limit: 2}"), serialized[3]);

    ASSERT_EQ(3, container.size());
    auto firstSort = dynamic_cast<DocumentSourceSort*>(container.cbegin()->get());
    ASSERT(firstSort->hasLimit());
    ASSERT_EQ(2, *firstSort->getLimit());

    makePipelineOptimizeAssertNoRewrites(getExpCtx(), serialized);
}

// Test for sort with multiple limits in increasing limit values.
TEST_F(InternalUnpackBucketLimitReorderTest, OptimizeForLimitWithSortAndTwoLimitsIncreasing) {
    auto pipeline =
        Pipeline::parse(makeVector(unpackSpecObj, sortObj, limitObj5, limitObj10), getExpCtx());
    pipeline->optimizePipeline();

    auto serialized = pipeline->serializeToBson();
    const auto& container = pipeline->getSources();

    ASSERT_EQ(4, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$sort: {'meta.a': 1, 'meta.b': -1}}"), serialized[0]);
    ASSERT_BSONOBJ_EQ(fromjson("{$limit: 5}"), serialized[1]);
    ASSERT_BSONOBJ_EQ(unpackSpecObj, serialized[2]);
    ASSERT_BSONOBJ_EQ(fromjson("{$limit: 5}"), serialized[3]);

    ASSERT_EQ(3, container.size());
    auto firstSort = dynamic_cast<DocumentSourceSort*>(container.cbegin()->get());
    ASSERT(firstSort->hasLimit());
    ASSERT_EQ(5, *firstSort->getLimit());

    makePipelineOptimizeAssertNoRewrites(getExpCtx(), serialized);
}

// Test for sort with multiple limits in decreasing limit values. In this case, the last limit
// {$limit: 2} would eventually replace the {$limit: 10} after {$limit: 10} is pushed up.
TEST_F(InternalUnpackBucketLimitReorderTest, OptimizeForLimitWithSortAndTwoLimitsDecreasing) {
    auto pipeline =
        Pipeline::parse(makeVector(unpackSpecObj, sortObj, limitObj10, limitObj2), getExpCtx());
    pipeline->optimizePipeline();

    auto serialized = pipeline->serializeToBson();
    const auto& container = pipeline->getSources();

    ASSERT_EQ(4, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$sort: {'meta.a': 1, 'meta.b': -1}}"), serialized[0]);
    ASSERT_BSONOBJ_EQ(fromjson("{$limit: 2}"), serialized[1]);
    ASSERT_BSONOBJ_EQ(unpackSpecObj, serialized[2]);
    ASSERT_BSONOBJ_EQ(fromjson("{$limit: 2}"), serialized[3]);

    ASSERT_EQ(3, container.size());
    auto firstSort = dynamic_cast<DocumentSourceSort*>(container.cbegin()->get());
    ASSERT(firstSort->hasLimit());
    ASSERT_EQ(2, *firstSort->getLimit());

    makePipelineOptimizeAssertNoRewrites(getExpCtx(), serialized);
}

}  // namespace
}  // namespace mongo
