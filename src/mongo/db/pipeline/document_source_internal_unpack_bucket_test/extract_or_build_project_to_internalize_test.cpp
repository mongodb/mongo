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
#include "mongo/bson/unordered_fields_bsonobj_comparator.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"

#include <iterator>
#include <list>
#include <memory>
#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

using InternalUnpackBucketBuildProjectToInternalizeTest = AggregationContextFixture;

TEST_F(InternalUnpackBucketBuildProjectToInternalizeTest,
       BuildsIncludeProjectForGroupDependencies) {
    auto unpackSpec = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'foo', bucketMaxSpanSeconds: 3600}}");
    auto groupSpec = fromjson("{$group: {_id: '$x', f: {$first: '$y'}, $willBeMerged: false}}");
    auto pipeline = Pipeline::parse(makeVector(unpackSpec, groupSpec), getExpCtx());
    auto& container = pipeline->getSources();
    ASSERT_EQ(2u, container.size());

    // Dependency analysis produces an inclusion projection for the $group.
    auto [project, isInclusion] =
        dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.begin()->get())
            ->extractOrBuildProjectToInternalize(container.begin(), &container);
    const UnorderedFieldsBSONObjComparator kComparator;
    ASSERT_EQ(kComparator.compare(fromjson("{_id: 0, x: 1, y: 1}"), project), 0);
    ASSERT_TRUE(isInclusion);

    // The pipeline is unchanged.
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());
    ASSERT_BSONOBJ_EQ(unpackSpec, serialized[0]);
    ASSERT_BSONOBJ_EQ(groupSpec, serialized[1]);
}

TEST_F(InternalUnpackBucketBuildProjectToInternalizeTest,
       BuildsIncludeProjectForProjectDependencies) {
    auto unpackSpec = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'foo', bucketMaxSpanSeconds: 3600}}");
    auto projectSpec = fromjson("{$project: {_id: true, x: {f: '$y'}}}");
    auto pipeline = Pipeline::parse(makeVector(unpackSpec, projectSpec), getExpCtx());
    auto& container = pipeline->getSources();
    ASSERT_EQ(2u, container.size());

    // Dependency analysis produces an inclusion projection for the $project.
    auto [project, isInclusion] =
        dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.begin()->get())
            ->extractOrBuildProjectToInternalize(container.begin(), &container);
    const UnorderedFieldsBSONObjComparator kComparator;
    ASSERT_EQ(kComparator.compare(fromjson("{_id: 1, x: 1, y: 1}"), project), 0);
    ASSERT_TRUE(isInclusion);

    // The pipeline is unchanged.
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());
    ASSERT_BSONOBJ_EQ(unpackSpec, serialized[0]);
    ASSERT_BSONOBJ_EQ(projectSpec, serialized[1]);
}

TEST_F(InternalUnpackBucketBuildProjectToInternalizeTest,
       BuildsIncludeProjectWhenInMiddleOfPipeline) {
    auto matchSpec = fromjson("{$match: {'meta.source': 'primary'}}");
    auto unpackSpec = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'foo', metaField: 'meta', "
        "bucketMaxSpanSeconds: 3600}}");
    auto groupSpec = fromjson("{$group: {_id: '$x', f: {$first: '$y'}, $willBeMerged: false}}");
    auto pipeline = Pipeline::parse(makeVector(matchSpec, unpackSpec, groupSpec), getExpCtx());
    auto& container = pipeline->getSources();
    ASSERT_EQ(3u, container.size());

    // Dependency analysis produces an inclusion projection for the $group.
    auto [project, isInclusion] =
        dynamic_cast<DocumentSourceInternalUnpackBucket*>(std::next(container.begin())->get())
            ->extractOrBuildProjectToInternalize(std::next(container.begin()), &container);
    const UnorderedFieldsBSONObjComparator kComparator;
    ASSERT_EQ(kComparator.compare(fromjson("{_id: 0, x: 1, y: 1}"), project), 0);
    ASSERT_TRUE(isInclusion);

    // The pipeline is unchanged.
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(3u, serialized.size());
    ASSERT_BSONOBJ_EQ(matchSpec, serialized[0]);
    ASSERT_BSONOBJ_EQ(unpackSpec, serialized[1]);
    ASSERT_BSONOBJ_EQ(groupSpec, serialized[2]);
}

TEST_F(InternalUnpackBucketBuildProjectToInternalizeTest,
       BuildsIncludeProjectWhenGroupDependenciesAreDotted) {
    auto unpackSpec = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'foo', bucketMaxSpanSeconds: 3600}}");
    auto groupSpec = fromjson("{$group: {_id: '$x.y', f: {$first: '$a.b'}, $willBeMerged: false}}");
    auto pipeline = Pipeline::parse(makeVector(unpackSpec, groupSpec), getExpCtx());
    auto& container = pipeline->getSources();
    ASSERT_EQ(2u, container.size());

    // Dependency analysis produces an inclusion projection with top-level fields for the $group.
    auto [project, isInclusion] =
        dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.begin()->get())
            ->extractOrBuildProjectToInternalize(container.begin(), &container);
    const UnorderedFieldsBSONObjComparator kComparator;
    ASSERT_EQ(kComparator.compare(fromjson("{_id: 0, x: 1, a: 1}"), project), 0);
    ASSERT_TRUE(isInclusion);

    // The pipeline is unchanged.
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());
    ASSERT_BSONOBJ_EQ(unpackSpec, serialized[0]);
    ASSERT_BSONOBJ_EQ(groupSpec, serialized[1]);
}

TEST_F(InternalUnpackBucketBuildProjectToInternalizeTest,
       BuildsIncludeProjectWhenProjectDependenciesAreDotted) {
    auto unpackSpec = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'foo', bucketMaxSpanSeconds: 3600}}");
    auto projectSpec = fromjson("{$project: {_id: {a : true}}}");
    auto pipeline = Pipeline::parse(makeVector(unpackSpec, projectSpec), getExpCtx());
    auto& container = pipeline->getSources();
    ASSERT_EQ(2u, container.size());

    // Dependency analysis produces an inclusion projection with top-level fields for the $project.
    auto [project, isInclusion] =
        dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.begin()->get())
            ->extractOrBuildProjectToInternalize(container.begin(), &container);
    const UnorderedFieldsBSONObjComparator kComparator;
    ASSERT_EQ(kComparator.compare(fromjson("{_id: 1}"), project), 0);
    ASSERT_TRUE(isInclusion);

    // The pipeline is unchanged.
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());
    ASSERT_BSONOBJ_EQ(unpackSpec, serialized[0]);
    ASSERT_BSONOBJ_EQ(projectSpec, serialized[1]);
}

TEST_F(InternalUnpackBucketBuildProjectToInternalizeTest,
       DoesNotBuildProjectWhenThereAreNoDependencies) {
    auto unpackSpec = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'foo', bucketMaxSpanSeconds: 3600}}");
    auto groupSpec = fromjson(
        "{$group: {_id: {$const: null}, count: { $sum: {$const: 1 }}, $willBeMerged: false}}");
    auto pipeline = Pipeline::parse(makeVector(unpackSpec, groupSpec), getExpCtx());
    auto& container = pipeline->getSources();
    ASSERT_EQ(2u, container.size());

    // Dependency analysis produces an empty project because there are no dependencies.
    auto [project, isInclusion] =
        dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.begin()->get())
            ->extractOrBuildProjectToInternalize(container.begin(), &container);
    ASSERT(project.isEmpty());

    // The pipeline is unchanged.
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());
    ASSERT_BSONOBJ_EQ(unpackSpec, serialized[0]);
    ASSERT_BSONOBJ_EQ(groupSpec, serialized[1]);
}

TEST_F(InternalUnpackBucketBuildProjectToInternalizeTest,
       DoesNotBuildProjectWhenSortDependenciesAreNotFinite) {
    auto unpackSpec = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'foo', bucketMaxSpanSeconds: 3600}}");
    auto sortSpec = fromjson("{$sort: {x: 1}}");
    auto pipeline = Pipeline::parse(makeVector(unpackSpec, sortSpec), getExpCtx());
    auto& container = pipeline->getSources();
    ASSERT_EQ(2u, container.size());

    // Dependency analysis produces an empty project because the dependency set is not finite.
    auto [project, isInclusion] =
        dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.begin()->get())
            ->extractOrBuildProjectToInternalize(container.begin(), &container);
    ASSERT(project.isEmpty());

    // The pipeline is unchanged.
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());
    ASSERT_BSONOBJ_EQ(unpackSpec, serialized[0]);
    ASSERT_BSONOBJ_EQ(sortSpec, serialized[1]);
}

TEST_F(InternalUnpackBucketBuildProjectToInternalizeTest,
       DoesNotBuildProjectWhenProjectDependenciesAreNotFinite) {
    auto unpackSpec = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'foo', bucketMaxSpanSeconds: 3600}}");
    auto sortSpec = fromjson("{$sort: {x: 1}}");
    auto projectSpec = fromjson("{$project: {_id: false, x: false}}");
    auto pipeline = Pipeline::parse(makeVector(unpackSpec, sortSpec, projectSpec), getExpCtx());
    auto& container = pipeline->getSources();
    ASSERT_EQ(3u, container.size());

    // Dependency analysis produces an empty project because the dependency set is not finite.
    auto [project, isInclusion] =
        dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.begin()->get())
            ->extractOrBuildProjectToInternalize(container.begin(), &container);
    ASSERT(project.isEmpty());

    // The pipeline is unchanged.
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(3u, serialized.size());
    ASSERT_BSONOBJ_EQ(unpackSpec, serialized[0]);
    ASSERT_BSONOBJ_EQ(sortSpec, serialized[1]);
    ASSERT_BSONOBJ_EQ(projectSpec, serialized[2]);
}

TEST_F(InternalUnpackBucketBuildProjectToInternalizeTest, UsesViableInclusionProject) {
    auto unpackSpec = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'foo', bucketMaxSpanSeconds: 3600}}");
    auto projectSpec = fromjson("{$project: {_id: true, x: true}}");
    auto pipeline = Pipeline::parse(makeVector(unpackSpec, projectSpec), getExpCtx());
    auto& container = pipeline->getSources();
    ASSERT_EQ(2u, container.size());

    // A viable inclusion $project is found in the pipeline.
    auto [project, isInclusion] =
        dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.begin()->get())
            ->extractOrBuildProjectToInternalize(container.begin(), &container);
    ASSERT_BSONOBJ_EQ(fromjson("{_id: true, x: true}"), project);
    ASSERT_TRUE(isInclusion);

    // The $project is deleted from the pipeline.
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(1u, serialized.size());
    ASSERT_BSONOBJ_EQ(unpackSpec, serialized[0]);
}

TEST_F(InternalUnpackBucketBuildProjectToInternalizeTest, UsesViableNonBoolInclusionProject) {
    auto unpackSpec = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'foo', bucketMaxSpanSeconds: 3600}}");
    auto projectSpec = fromjson("{$project: {_id: 1, x: 1.0, y: 1.5}}");
    auto pipeline = Pipeline::parse(makeVector(unpackSpec, projectSpec), getExpCtx());
    auto& container = pipeline->getSources();
    ASSERT_EQ(2u, container.size());

    // A viable inclusion $project is found in the pipeline.
    auto [project, isInclusion] =
        dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.begin()->get())
            ->extractOrBuildProjectToInternalize(container.begin(), &container);
    const UnorderedFieldsBSONObjComparator kComparator;
    ASSERT_EQ(kComparator.compare(fromjson("{_id: true, x: true, y: true}"), project), 0);
    ASSERT_TRUE(isInclusion);

    // The $project is deleted from the pipeline.
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(1u, serialized.size());
    ASSERT_BSONOBJ_EQ(unpackSpec, serialized[0]);
}

TEST_F(InternalUnpackBucketBuildProjectToInternalizeTest, UsesViableExclusionProject) {
    auto unpackSpec = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'foo', bucketMaxSpanSeconds: 3600}}");
    auto projectSpec = fromjson("{$project: {_id: false, x: false}}");
    auto pipeline = Pipeline::parse(makeVector(unpackSpec, projectSpec), getExpCtx());
    auto& container = pipeline->getSources();
    ASSERT_EQ(2u, container.size());

    // A viable exclusion $project is found in the pipeline.
    auto [project, isInclusion] =
        dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.begin()->get())
            ->extractOrBuildProjectToInternalize(container.begin(), &container);
    ASSERT_BSONOBJ_EQ(fromjson("{_id: false, x: false}"), project);
    ASSERT_FALSE(isInclusion);

    // The $project is deleted from the pipeline.
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(1u, serialized.size());
    ASSERT_BSONOBJ_EQ(unpackSpec, serialized[0]);
}

TEST_F(InternalUnpackBucketBuildProjectToInternalizeTest,
       BuildsInclusionProjectInsteadOfViableExclusionProject) {
    auto unpackSpec = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'foo', bucketMaxSpanSeconds: 3600}}");
    auto projectSpec = fromjson("{$project: {_id: false, x: false}}");
    auto sortSpec = fromjson("{$sort: {y: 1}}");
    auto groupSpec = fromjson("{$group: {_id: '$y', f: {$first: '$z'}, $willBeMerged: false}}");
    auto pipeline =
        Pipeline::parse(makeVector(unpackSpec, projectSpec, sortSpec, groupSpec), getExpCtx());
    auto& container = pipeline->getSources();
    ASSERT_EQ(4u, container.size());

    // A viable exclusion $project is found in the pipeline, but the inclusion project from the
    // dependency analysis should be used instead.
    auto [project, isInclusion] =
        dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.begin()->get())
            ->extractOrBuildProjectToInternalize(container.begin(), &container);
    const UnorderedFieldsBSONObjComparator kComparator;
    ASSERT_EQ(kComparator.compare(fromjson("{_id: 0, y: 1, z: 1}"), project), 0);
    ASSERT_TRUE(isInclusion);

    // Pipeline is unchanged.
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(4u, serialized.size());
    ASSERT_BSONOBJ_EQ(unpackSpec, serialized[0]);
    ASSERT_BSONOBJ_EQ(projectSpec, serialized[1]);
    ASSERT_BSONOBJ_EQ(sortSpec, serialized[2]);
    ASSERT_BSONOBJ_EQ(groupSpec, serialized[3]);
}
}  // namespace
}  // namespace mongo
