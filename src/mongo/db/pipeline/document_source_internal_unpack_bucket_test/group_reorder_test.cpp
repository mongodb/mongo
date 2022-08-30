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

TEST_F(InternalUnpackBucketGroupReorder, OptimizeForCount) {
    auto unpackSpecObj = fromjson(
        "{$_internalUnpackBucket: { include: ['a', 'b', 'c'], metaField: 'meta', timeField: 't', "
        "bucketMaxSpanSeconds: 3600}}");
    auto countSpecObj = fromjson("{$count: 'foo'}");

    auto pipeline = Pipeline::parse(makeVector(unpackSpecObj, countSpecObj), getExpCtx());
    pipeline->optimizePipeline();

    auto serialized = pipeline->serializeToBson();
    // $count gets rewritten to $group + $project.
    ASSERT_EQ(3, serialized.size());

    auto optimized = fromjson(
        "{$_internalUnpackBucket: { include: [], timeField: 't', metaField: 'meta', "
        "bucketMaxSpanSeconds: 3600}}");
    ASSERT_BSONOBJ_EQ(optimized, serialized[0]);
}

TEST_F(InternalUnpackBucketGroupReorder, OptimizeForCountNegative) {
    auto unpackSpecObj = fromjson(
        "{$_internalUnpackBucket: { include: ['a', 'b', 'c'], metaField: 'meta', timeField: 't', "
        "bucketMaxSpanSeconds: 3600}}");
    auto groupSpecObj = fromjson("{$group: {_id: '$a', s: {$sum: '$b'}}}");

    auto pipeline = Pipeline::parse(makeVector(unpackSpecObj, groupSpecObj), getExpCtx());
    pipeline->optimizePipeline();

    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2, serialized.size());

    // We do not get the reorder since we are grouping on a field.
    auto optimized = fromjson(
        "{$_internalUnpackBucket: { include: ['a', 'b', 'c'], timeField: 't', metaField: 'meta', "
        "bucketMaxSpanSeconds: 3600}}");
    ASSERT_BSONOBJ_EQ(optimized, serialized[0]);
}

TEST_F(InternalUnpackBucketGroupReorder, MinMaxGroupOnMetadata) {
    auto unpackSpecObj = fromjson(
        "{$_internalUnpackBucket: { include: ['a', 'b', 'c'], metaField: 'meta1', timeField: 't', "
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

TEST_F(InternalUnpackBucketGroupReorder, MinMaxGroupOnMetafield) {
    auto unpackSpecObj = fromjson(
        "{$_internalUnpackBucket: { include: ['a', 'b', 'c'], metaField: 'meta1', timeField: 't', "
        "bucketMaxSpanSeconds: 3600}}");
    auto groupSpecObj = fromjson("{$group: {_id: '$meta1.a.b', accmin: {$sum: '$meta1.f1'}}}");

    auto pipeline = Pipeline::parse(makeVector(unpackSpecObj, groupSpecObj), getExpCtx());
    pipeline->optimizePipeline();

    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(1, serialized.size());

    auto optimized = fromjson("{$group: {_id: '$meta.a.b', accmin: {$sum: '$meta.f1'}}}");
    ASSERT_BSONOBJ_EQ(optimized, serialized[0]);
}

TEST_F(InternalUnpackBucketGroupReorder, MinMaxGroupOnMetadataNegative) {
    auto unpackSpecObj = fromjson(
        "{$_internalUnpackBucket: { include: ['a', 'b', 'c'], timeField: 't', metaField: 'meta', "
        "bucketMaxSpanSeconds: 3600}}");
    auto groupSpecObj = fromjson("{$group: {_id: '$meta', accmin: {$min: '$b'}, s: {$sum: '$c'}}}");

    auto pipeline = Pipeline::parse(makeVector(unpackSpecObj, groupSpecObj), getExpCtx());
    pipeline->optimizePipeline();

    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2, serialized.size());

    ASSERT_BSONOBJ_EQ(unpackSpecObj, serialized[0]);
    ASSERT_BSONOBJ_EQ(groupSpecObj, serialized[1]);
}

TEST_F(InternalUnpackBucketGroupReorder, MinMaxGroupOnMetadataNegative1) {
    auto unpackSpecObj = fromjson(
        "{$_internalUnpackBucket: { include: ['a', 'b', 'c'], timeField: 't', metaField: 'meta', "
        "bucketMaxSpanSeconds: 3600}}");
    auto groupSpecObj = fromjson("{$group: {_id: '$meta', accmin: {$min: '$t.a'}}}");

    auto pipeline = Pipeline::parse(makeVector(unpackSpecObj, groupSpecObj), getExpCtx());
    pipeline->optimizePipeline();

    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2, serialized.size());

    ASSERT_BSONOBJ_EQ(unpackSpecObj, serialized[0]);
    ASSERT_BSONOBJ_EQ(groupSpecObj, serialized[1]);
}

}  // namespace
}  // namespace mongo
