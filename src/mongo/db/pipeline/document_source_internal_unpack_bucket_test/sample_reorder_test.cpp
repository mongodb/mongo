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

#include "mongo/bson/unordered_fields_bsonobj_comparator.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/query/util/make_data_structure.h"

namespace mongo {
namespace {

using InternalUnpackBucketSampleReorderTest = AggregationContextFixture;

TEST_F(InternalUnpackBucketSampleReorderTest, SampleThenSimpleProject) {
    auto unpackSpec = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'foo', metaField: 'myMeta', "
        "bucketMaxSpanSeconds: 3600}}");
    auto sampleSpec = fromjson("{$sample: {size: 500}}");
    auto projectSpec = fromjson("{$project: {_id: false, x: false, y: false}}");

    auto pipeline = Pipeline::parse(makeVector(unpackSpec, sampleSpec, projectSpec), getExpCtx());
    pipeline->optimizePipeline();

    auto serialized = pipeline->serializeToBson();

    ASSERT_EQ(3, serialized.size());
    ASSERT_BSONOBJ_EQ(unpackSpec, serialized[0]);
    ASSERT_BSONOBJ_EQ(sampleSpec, serialized[1]);
    const UnorderedFieldsBSONObjComparator kComparator;
    ASSERT_EQ(kComparator.compare(projectSpec, serialized[2]), 0);
}

TEST_F(InternalUnpackBucketSampleReorderTest, SampleThenComputedProject) {
    auto unpackSpec = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'foo', metaField: 'myMeta', "
        "bucketMaxSpanSeconds: 3600}}");
    auto sampleSpec = fromjson("{$sample: {size: 500}}");
    auto projectSpec =
        fromjson("{$project: {_id: true, city: '$myMeta.address.city', temp: '$temp.celsius'}}");

    auto pipeline = Pipeline::parse(makeVector(unpackSpec, sampleSpec, projectSpec), getExpCtx());
    pipeline->optimizePipeline();

    auto serialized = pipeline->serializeToBson();

    ASSERT_EQ(3, serialized.size());
    ASSERT_BSONOBJ_EQ(
        fromjson("{$_internalUnpackBucket: { include: ['_id', 'temp', 'myMeta'], "
                 "timeField: 'foo', metaField: 'myMeta', bucketMaxSpanSeconds: 3600}}"),
        serialized[0]);
    ASSERT_BSONOBJ_EQ(sampleSpec, serialized[1]);
    const UnorderedFieldsBSONObjComparator kComparator;
    ASSERT_EQ(kComparator.compare(projectSpec, serialized[2]), 0);
}

TEST_F(InternalUnpackBucketSampleReorderTest, SimpleProjectThenSample) {
    auto unpackSpec = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'foo', metaField: 'myMeta', "
        "bucketMaxSpanSeconds: 3600}}");
    auto projectSpec = fromjson("{$project: {_id: true, x: true}}");
    auto sampleSpec = fromjson("{$sample: {size: 500}}");

    auto pipeline = Pipeline::parse(makeVector(unpackSpec, projectSpec, sampleSpec), getExpCtx());
    pipeline->optimizePipeline();

    auto serialized = pipeline->serializeToBson();

    ASSERT_EQ(3, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$_internalUnpackBucket: { include: ['_id', 'x'], timeField: "
                               "'foo', metaField: 'myMeta', bucketMaxSpanSeconds: 3600}}"),
                      serialized[0]);
    ASSERT_BSONOBJ_EQ(sampleSpec, serialized[1]);
    const UnorderedFieldsBSONObjComparator kComparator;
    ASSERT_EQ(kComparator.compare(projectSpec, serialized[2]), 0);
}

TEST_F(InternalUnpackBucketSampleReorderTest, ComputedProjectThenSample) {
    auto unpackSpec = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'foo', metaField: 'myMeta', "
        "bucketMaxSpanSeconds: 3600}}");
    auto projectSpec =
        fromjson("{$project: {_id: true, city: '$myMeta.address.city', temp: '$temp.celsius'}}");
    auto sampleSpec = fromjson("{$sample: {size: 500}}");

    auto pipeline = Pipeline::parse(makeVector(unpackSpec, projectSpec, sampleSpec), getExpCtx());
    pipeline->optimizePipeline();

    auto serialized = pipeline->serializeToBson();

    ASSERT_EQ(3, serialized.size());
    ASSERT_BSONOBJ_EQ(
        fromjson("{$_internalUnpackBucket: { include: ['_id', 'temp', 'myMeta'], timeField: "
                 "'foo', metaField: 'myMeta', bucketMaxSpanSeconds: 3600}}"),
        serialized[0]);
    ASSERT_BSONOBJ_EQ(sampleSpec, serialized[1]);
    const UnorderedFieldsBSONObjComparator kComparator;
    ASSERT_EQ(kComparator.compare(projectSpec, serialized[2]), 0);
}
}  // namespace
}  // namespace mongo
