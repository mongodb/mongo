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

#include "mongo/bson/unordered_fields_bsonobj_comparator.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/util/make_data_structure.h"

namespace mongo {
namespace {

using InternalUnpackBucketBuildProjectToInternalizeTest = AggregationContextFixture;

TEST_F(InternalUnpackBucketBuildProjectToInternalizeTest,
       BuildsIncludeProjectForGroupDependencies) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'foo'}}"),
                   fromjson("{$group: {_id: '$x', f: {$first: '$y'}}}")),
        getExpCtx());
    auto& container = pipeline->getSources();
    ASSERT_EQ(2u, container.size());

    auto project = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.begin()->get())
                       ->buildProjectToInternalize(container.begin(), &container);

    const UnorderedFieldsBSONObjComparator kComparator;
    ASSERT_EQ(kComparator.compare(fromjson("{_id: 0, x: 1, y: 1}"), project), 0);
}

TEST_F(InternalUnpackBucketBuildProjectToInternalizeTest,
       BuildsIncludeProjectForProjectDependencies) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'foo'}}"),
                   fromjson("{$project: {x: {f: '$y'}}}")),
        getExpCtx());
    auto& container = pipeline->getSources();
    ASSERT_EQ(2u, container.size());

    auto project = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.begin()->get())
                       ->buildProjectToInternalize(container.begin(), &container);

    const UnorderedFieldsBSONObjComparator kComparator;
    ASSERT_EQ(kComparator.compare(fromjson("{_id: 1, x: 1, y: 1}"), project), 0);
}

TEST_F(InternalUnpackBucketBuildProjectToInternalizeTest,
       BuildsIncludeProjectWhenInMiddleOfPipeline) {
    auto pipeline = Pipeline::parse(
        makeVector(
            fromjson("{$match: {'meta.source': 'primary'}}"),
            fromjson(
                "{$_internalUnpackBucket: { exclude: [], timeField: 'foo', metaField: 'meta'}}"),
            fromjson("{$group: {_id: '$x', f: {$first: '$y'}}}")),
        getExpCtx());
    auto& container = pipeline->getSources();
    ASSERT_EQ(3u, container.size());

    auto project =
        dynamic_cast<DocumentSourceInternalUnpackBucket*>(std::next(container.begin())->get())
            ->buildProjectToInternalize(std::next(container.begin()), &container);

    const UnorderedFieldsBSONObjComparator kComparator;
    ASSERT_EQ(kComparator.compare(fromjson("{_id: 0, x: 1, y: 1}"), project), 0);
}

TEST_F(InternalUnpackBucketBuildProjectToInternalizeTest,
       BuildsIncludeProjectWhenGroupDependenciesAreDotted) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'foo'}}"),
                   fromjson("{$group: {_id: '$x.y', f: {$first: '$a.b'}}}")),
        getExpCtx());
    auto& container = pipeline->getSources();
    ASSERT_EQ(2u, container.size());

    auto project = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.begin()->get())
                       ->buildProjectToInternalize(container.begin(), &container);

    const UnorderedFieldsBSONObjComparator kComparator;
    ASSERT_EQ(kComparator.compare(fromjson("{_id: 0, x: 1, a: 1}"), project), 0);
}

TEST_F(InternalUnpackBucketBuildProjectToInternalizeTest,
       BuildsIncludeProjectWhenProjectDependenciesAreDotted) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'foo'}}"),
                   fromjson("{$project: {'_id.a': true}}")),
        getExpCtx());
    auto& container = pipeline->getSources();
    ASSERT_EQ(2u, container.size());

    auto project = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.begin()->get())
                       ->buildProjectToInternalize(container.begin(), &container);

    const UnorderedFieldsBSONObjComparator kComparator;
    ASSERT_EQ(kComparator.compare(fromjson("{_id: 1}"), project), 0);
}

TEST_F(InternalUnpackBucketBuildProjectToInternalizeTest,
       DoesNotBuildProjectWhenThereAreNoDependencies) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'foo'}}"),
                   fromjson("{$group: {_id: {$const: null}, count: { $sum: {$const: 1 }}}}")),
        getExpCtx());
    auto& container = pipeline->getSources();
    ASSERT_EQ(2u, container.size());

    auto project = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.begin()->get())
                       ->buildProjectToInternalize(container.begin(), &container);
    ASSERT(project.isEmpty());
}

TEST_F(InternalUnpackBucketBuildProjectToInternalizeTest,
       DoesNotBuildProjectWhenSortDependenciesAreNotFinite) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'foo'}}"),
                   fromjson("{$sort: {x: 1}}")),
        getExpCtx());
    auto& container = pipeline->getSources();
    ASSERT_EQ(2u, container.size());

    auto project = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.begin()->get())
                       ->buildProjectToInternalize(container.begin(), &container);
    ASSERT(project.isEmpty());
}

TEST_F(InternalUnpackBucketBuildProjectToInternalizeTest,
       DoesNotBuildProjectWhenProjectDependenciesAreNotFinite) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'foo'}}"),
                   fromjson("{$sort: {x: 1}}"),
                   fromjson("{$project: {_id: false, x: false}}")),
        getExpCtx());
    auto& container = pipeline->getSources();
    ASSERT_EQ(3u, container.size());

    auto project = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.begin()->get())
                       ->buildProjectToInternalize(container.begin(), &container);
    ASSERT(project.isEmpty());
}

TEST_F(InternalUnpackBucketBuildProjectToInternalizeTest,
       DoesNotBuildProjectWhenViableInclusionProjectExists) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'foo'}}"),
                   fromjson("{$project: {_id: true, x: true}}")),
        getExpCtx());
    auto& container = pipeline->getSources();
    ASSERT_EQ(2u, container.size());

    auto project = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.begin()->get())
                       ->buildProjectToInternalize(container.begin(), &container);
    ASSERT(project.isEmpty());
}

TEST_F(InternalUnpackBucketBuildProjectToInternalizeTest,
       DoesNotBuildProjectWhenViableNonBoolInclusionProjectExists) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'foo'}}"),
                   fromjson("{$project: {_id: 1, x: 1.0, y: 1.5}}")),
        getExpCtx());
    auto& container = pipeline->getSources();
    ASSERT_EQ(2u, container.size());

    auto project = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.begin()->get())
                       ->buildProjectToInternalize(container.begin(), &container);
    ASSERT(project.isEmpty());
}

TEST_F(InternalUnpackBucketBuildProjectToInternalizeTest,
       DoesNotBuildProjectWhenViableExclusionProjectExists) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'foo'}}"),
                   fromjson("{$project: {_id: false, x: false}}")),
        getExpCtx());
    auto& container = pipeline->getSources();
    ASSERT_EQ(2u, container.size());

    auto project = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.begin()->get())
                       ->buildProjectToInternalize(container.begin(), &container);
    ASSERT(project.isEmpty());
}

TEST_F(InternalUnpackBucketBuildProjectToInternalizeTest,
       BuildsInclusionProjectInsteadOfViableExclusionProject) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'foo'}}"),
                   fromjson("{$project: {_id: false, x: false}}"),
                   fromjson("{$sort: {y: 1}}"),
                   fromjson("{$group: {_id: '$y', f: {$first: '$z'}}}")),
        getExpCtx());
    auto& container = pipeline->getSources();
    ASSERT_EQ(4u, container.size());

    auto project = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.begin()->get())
                       ->buildProjectToInternalize(container.begin(), &container);

    const UnorderedFieldsBSONObjComparator kComparator;
    ASSERT_EQ(kComparator.compare(fromjson("{_id: 0, y: 1, z: 1}"), project), 0);
}
}  // namespace
}  // namespace mongo
