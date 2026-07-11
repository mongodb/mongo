// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/bson/unordered_fields_bsonobj_comparator.h"
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

using InternalUnpackBucketSampleReorderTest = AggregationContextFixture;

TEST_F(InternalUnpackBucketSampleReorderTest, SampleThenSimpleProject) {
    auto unpackSpec = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'foo', metaField: 'myMeta', "
        "bucketMaxSpanSeconds: 3600}}");
    auto sampleSpec = fromjson("{$sample: {size: 500}}");
    auto projectSpec = fromjson("{$project: {_id: false, x: false, y: false}}");

    auto pipeline = pipeline_factory::makePipeline(makeVector(unpackSpec, sampleSpec, projectSpec),
                                                   getExpCtx(),
                                                   pipeline_factory::kOptionsMinimal);
    pipeline_optimization::optimizePipeline(*pipeline);

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

    auto pipeline = pipeline_factory::makePipeline(makeVector(unpackSpec, sampleSpec, projectSpec),
                                                   getExpCtx(),
                                                   pipeline_factory::kOptionsMinimal);
    pipeline_optimization::optimizePipeline(*pipeline);

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

    auto pipeline = pipeline_factory::makePipeline(makeVector(unpackSpec, projectSpec, sampleSpec),
                                                   getExpCtx(),
                                                   pipeline_factory::kOptionsMinimal);
    pipeline_optimization::optimizePipeline(*pipeline);

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

    auto pipeline = pipeline_factory::makePipeline(makeVector(unpackSpec, projectSpec, sampleSpec),
                                                   getExpCtx(),
                                                   pipeline_factory::kOptionsMinimal);
    pipeline_optimization::optimizePipeline(*pipeline);

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
