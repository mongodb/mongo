/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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
#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/idl/server_parameter_test_util.h"

namespace mongo {
namespace {

using InternalUnpackBucketOptimizeLastpointTest = AggregationContextFixture;

TEST_F(InternalUnpackBucketOptimizeLastpointTest, NonLastpointDoesNotParticipateInOptimization) {
    RAIIServerParameterControllerForTest controller("featureFlagLastPointQuery", true);
    {
        // $sort must contain a time field.
        auto unpackSpec = fromjson(
            "{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: 'tags', "
            "bucketMaxSpanSeconds: 3600}}");
        auto sortSpec = fromjson("{$sort: {'tags.a': 1}}");
        auto groupSpec =
            fromjson("{$group: {_id: '$tags.a', b: {$first: '$b'}, c: {$first: '$c'}}}");
        auto pipeline = Pipeline::parse(makeVector(unpackSpec, sortSpec, groupSpec), getExpCtx());
        auto& container = pipeline->getSources();

        ASSERT_EQ(container.size(), 3U);

        auto success = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                           ->optimizeLastpoint(container.begin(), &container);
        ASSERT_FALSE(success);

        // The pipeline is unchanged.
        auto serialized = pipeline->serializeToBson();
        ASSERT_EQ(serialized.size(), 3u);
        ASSERT_BSONOBJ_EQ(serialized[0], unpackSpec);
        ASSERT_BSONOBJ_EQ(serialized[1], sortSpec);
        ASSERT_BSONOBJ_EQ(serialized[2], groupSpec);
    }
    {
        // $sort must have the time field as the last field in the sort key pattern.
        auto unpackSpec = fromjson(
            "{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: 'tags', "
            "bucketMaxSpanSeconds: 3600}}");
        auto sortSpec = fromjson("{$sort: {t: -1, 'tags.a': 1}}");
        auto groupSpec =
            fromjson("{$group: {_id: '$tags.a', b: {$first: '$b'}, c: {$first: '$c'}}}");
        auto pipeline = Pipeline::parse(makeVector(unpackSpec, sortSpec, groupSpec), getExpCtx());
        auto& container = pipeline->getSources();

        ASSERT_EQ(container.size(), 3U);

        auto success = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                           ->optimizeLastpoint(container.begin(), &container);
        ASSERT_FALSE(success);

        // The pipeline is unchanged.
        auto serialized = pipeline->serializeToBson();
        ASSERT_EQ(serialized.size(), 3u);
        ASSERT_BSONOBJ_EQ(serialized[0], unpackSpec);
        ASSERT_BSONOBJ_EQ(serialized[1], sortSpec);
        ASSERT_BSONOBJ_EQ(serialized[2], groupSpec);
    }
    {
        // $group's _id must be a meta field.
        auto unpackSpec = fromjson(
            "{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: 'tags', "
            "bucketMaxSpanSeconds: 3600}}");
        auto sortSpec = fromjson("{$sort: {'tags.a': 1, t: -1}}");
        auto groupSpec =
            fromjson("{$group: {_id: '$nonMeta', b: {$first: '$b'}, c: {$first: '$c'}}}");
        auto pipeline = Pipeline::parse(makeVector(unpackSpec, sortSpec, groupSpec), getExpCtx());
        auto& container = pipeline->getSources();

        ASSERT_EQ(container.size(), 3U);

        auto success = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                           ->optimizeLastpoint(container.begin(), &container);
        ASSERT_FALSE(success);

        // The pipeline is unchanged.
        auto serialized = pipeline->serializeToBson();
        ASSERT_EQ(serialized.size(), 3u);
        ASSERT_BSONOBJ_EQ(serialized[0], unpackSpec);
        ASSERT_BSONOBJ_EQ(serialized[1], sortSpec);
        ASSERT_BSONOBJ_EQ(serialized[2], groupSpec);
    }
    {
        // For now, $group can only contain $first accumulators.
        auto unpackSpec = fromjson(
            "{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: 'tags', "
            "bucketMaxSpanSeconds: 3600}}");
        auto sortSpec = fromjson("{$sort: {'tags.a': 1, t: -1}}");
        auto groupSpec =
            fromjson("{$group: {_id: '$tags.a', b: {$first: '$b'}, c: {$last: '$c'}}}");
        auto pipeline = Pipeline::parse(makeVector(unpackSpec, sortSpec, groupSpec), getExpCtx());
        auto& container = pipeline->getSources();

        ASSERT_EQ(container.size(), 3U);

        auto success = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                           ->optimizeLastpoint(container.begin(), &container);
        ASSERT_FALSE(success);

        // The pipeline is unchanged.
        auto serialized = pipeline->serializeToBson();
        ASSERT_EQ(serialized.size(), 3u);
        ASSERT_BSONOBJ_EQ(serialized[0], unpackSpec);
        ASSERT_BSONOBJ_EQ(serialized[1], sortSpec);
        ASSERT_BSONOBJ_EQ(serialized[2], groupSpec);
    }
}

TEST_F(InternalUnpackBucketOptimizeLastpointTest,
       LastpointWithMetaSubfieldAscendingTimeDescending) {
    RAIIServerParameterControllerForTest controller("featureFlagLastPointQuery", true);
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: "
                            "'tags', bucketMaxSpanSeconds: 3600}}"),
                   fromjson("{$sort: {'tags.a': 1, t: -1}}"),
                   fromjson("{$group: {_id: '$tags.a', b: {$first: '$b'}, c: {$first: '$c'}}}")),
        getExpCtx());
    auto& container = pipeline->getSources();

    ASSERT_EQ(container.size(), 3U);

    auto success = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                       ->optimizeLastpoint(container.begin(), &container);
    ASSERT_TRUE(success);

    auto serialized = pipeline->serializeToBson();

    ASSERT_EQ(serialized.size(), 5u);
    ASSERT_BSONOBJ_EQ(serialized[0],
                      fromjson("{$sort: {'meta.a': 1, 'control.max.t': -1, 'control.min.t': -1}}"));
    ASSERT_BSONOBJ_EQ(serialized[1],
                      fromjson("{$group: {_id: '$meta.a', bucket: {$first: '$_id'}}}"));
    ASSERT_BSONOBJ_EQ(
        serialized[2],
        fromjson(
            "{$lookup: {from: 'pipeline_test', as: 'metrics', localField: 'bucket', foreignField: "
            "'_id', let: {}, pipeline: [{$_internalUnpackBucket: {exclude: [], timeField: 't', "
            "metaField: 'tags', bucketMaxSpanSeconds: 3600}}, {$sort: {t: -1}}, {$limit: 1}]}}"));
    ASSERT_BSONOBJ_EQ(serialized[3], fromjson("{$unwind: {path: '$metrics'}}"));
    ASSERT_BSONOBJ_EQ(
        serialized[4],
        fromjson("{$replaceRoot: {newRoot: {_id: '$_id', b: {$ifNull: ['$metrics.b', {$const: "
                 "null}]}, c: {$ifNull: ['$metrics.c', {$const: null}]}}}}"));
}

TEST_F(InternalUnpackBucketOptimizeLastpointTest,
       LastpointWithMetaSubfieldDescendingTimeDescending) {
    RAIIServerParameterControllerForTest controller("featureFlagLastPointQuery", true);
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: "
                            "'tags', bucketMaxSpanSeconds: 3600}}"),
                   fromjson("{$sort: {'tags.a': -1, t: -1}}"),
                   fromjson("{$group: {_id: '$tags.a', b: {$first: '$b'}, c: {$first: '$c'}}}")),
        getExpCtx());
    auto& container = pipeline->getSources();

    ASSERT_EQ(container.size(), 3U);

    auto success = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                       ->optimizeLastpoint(container.begin(), &container);
    ASSERT_TRUE(success);

    auto serialized = pipeline->serializeToBson();

    ASSERT_EQ(serialized.size(), 5u);
    ASSERT_BSONOBJ_EQ(
        serialized[0],
        fromjson("{$sort: {'meta.a': -1, 'control.max.t': -1, 'control.min.t': -1}}"));
    ASSERT_BSONOBJ_EQ(serialized[1],
                      fromjson("{$group: {_id: '$meta.a', bucket: {$first: '$_id'}}}"));
    ASSERT_BSONOBJ_EQ(
        serialized[2],
        fromjson(
            "{$lookup: {from: 'pipeline_test', as: 'metrics', localField: 'bucket', foreignField: "
            "'_id', let: {}, pipeline: [{$_internalUnpackBucket: {exclude: [], timeField: 't', "
            "metaField: 'tags', bucketMaxSpanSeconds: 3600}}, {$sort: {t: -1}}, {$limit: 1}]}}"));
    ASSERT_BSONOBJ_EQ(serialized[3], fromjson("{$unwind: {path: '$metrics'}}"));
    ASSERT_BSONOBJ_EQ(
        serialized[4],
        fromjson("{$replaceRoot: {newRoot: {_id: '$_id', b: {$ifNull: ['$metrics.b', {$const: "
                 "null}]}, c: {$ifNull: ['$metrics.c', {$const: null}]}}}}"));
}

TEST_F(InternalUnpackBucketOptimizeLastpointTest, LastpointWithMetaSubfieldAscendingTimeAscending) {
    RAIIServerParameterControllerForTest controller("featureFlagLastPointQuery", true);
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: "
                            "'tags', bucketMaxSpanSeconds: 3600}}"),
                   fromjson("{$sort: {'tags.a': 1, t: 1}}"),
                   fromjson("{$group: {_id: '$tags.a', b: {$last: '$b'}, c: {$last: '$c'}}}")),
        getExpCtx());
    auto& container = pipeline->getSources();

    ASSERT_EQ(container.size(), 3U);

    auto success = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                       ->optimizeLastpoint(container.begin(), &container);
    ASSERT_TRUE(success);

    auto serialized = pipeline->serializeToBson();

    ASSERT_EQ(serialized.size(), 5u);
    ASSERT_BSONOBJ_EQ(
        serialized[0],
        fromjson("{$sort: {'meta.a': -1, 'control.max.t': -1, 'control.min.t': -1}}"));
    ASSERT_BSONOBJ_EQ(serialized[1],
                      fromjson("{$group: {_id: '$meta.a', bucket: {$first: '$_id'}}}"));
    ASSERT_BSONOBJ_EQ(
        serialized[2],
        fromjson(
            "{$lookup: {from: 'pipeline_test', as: 'metrics', localField: 'bucket', foreignField: "
            "'_id', let: {}, pipeline: [{$_internalUnpackBucket: {exclude: [], timeField: 't', "
            "metaField: 'tags', bucketMaxSpanSeconds: 3600}}, {$sort: {t: -1}}, {$limit: 1}]}}"));
    ASSERT_BSONOBJ_EQ(serialized[3], fromjson("{$unwind: {path: '$metrics'}}"));
    ASSERT_BSONOBJ_EQ(
        serialized[4],
        fromjson("{$replaceRoot: {newRoot: {_id: '$_id', b: {$ifNull: ['$metrics.b', {$const: "
                 "null}]}, c: {$ifNull: ['$metrics.c', {$const: null}]}}}}"));
}

TEST_F(InternalUnpackBucketOptimizeLastpointTest,
       LastpointWithMetaSubfieldDescendingTimeAscending) {
    RAIIServerParameterControllerForTest controller("featureFlagLastPointQuery", true);
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: "
                            "'tags', bucketMaxSpanSeconds: 3600}}"),
                   fromjson("{$sort: {'tags.a': -1, t: 1}}"),
                   fromjson("{$group: {_id: '$tags.a', b: {$last: '$b'}, c: {$last: '$c'}}}")),
        getExpCtx());
    auto& container = pipeline->getSources();

    ASSERT_EQ(container.size(), 3U);

    auto success = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                       ->optimizeLastpoint(container.begin(), &container);
    ASSERT_TRUE(success);

    auto serialized = pipeline->serializeToBson();

    ASSERT_EQ(serialized.size(), 5u);
    ASSERT_BSONOBJ_EQ(serialized[0],
                      fromjson("{$sort: {'meta.a': 1, 'control.max.t': -1, 'control.min.t': -1}}"));
    ASSERT_BSONOBJ_EQ(serialized[1],
                      fromjson("{$group: {_id: '$meta.a', bucket: {$first: '$_id'}}}"));
    ASSERT_BSONOBJ_EQ(
        serialized[2],
        fromjson(
            "{$lookup: {from: 'pipeline_test', as: 'metrics', localField: 'bucket', foreignField: "
            "'_id', let: {}, pipeline: [{$_internalUnpackBucket: {exclude: [], timeField: 't', "
            "metaField: 'tags', bucketMaxSpanSeconds: 3600}}, {$sort: {t: -1}}, {$limit: 1}]}}"));
    ASSERT_BSONOBJ_EQ(serialized[3], fromjson("{$unwind: {path: '$metrics'}}"));
    ASSERT_BSONOBJ_EQ(
        serialized[4],
        fromjson("{$replaceRoot: {newRoot: {_id: '$_id', b: {$ifNull: ['$metrics.b', {$const: "
                 "null}]}, c: {$ifNull: ['$metrics.c', {$const: null}]}}}}"));
}

}  // namespace
}  // namespace mongo
