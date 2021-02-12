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

using InternalUnpackBucketInternalizeProjectTest = AggregationContextFixture;

TEST_F(InternalUnpackBucketInternalizeProjectTest, InternalizesInclusionProject) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'foo'}}"),
                   fromjson("{$project: {x: true, y: true, _id: true}}")),
        getExpCtx());
    ASSERT_EQ(2u, pipeline->getSources().size());
    auto& container = pipeline->getSources();

    dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.begin()->get())
        ->internalizeProject(container.begin(), &container);

    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(1u, serialized.size());
    ASSERT_BSONOBJ_EQ(
        fromjson("{$_internalUnpackBucket: { include: ['_id', 'x', 'y'], timeField: 'foo'}}"),
        serialized[0]);
}

TEST_F(InternalUnpackBucketInternalizeProjectTest, InternalizesInclusionButExcludesId) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'foo'}}"),
                   fromjson("{$project: {x: true, y: true, _id: false}}")),
        getExpCtx());
    ASSERT_EQ(2u, pipeline->getSources().size());
    auto& container = pipeline->getSources();

    dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.begin()->get())
        ->internalizeProject(container.begin(), &container);

    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(1u, serialized.size());
    ASSERT_BSONOBJ_EQ(
        fromjson("{$_internalUnpackBucket: { include: ['x', 'y'], timeField: 'foo'}}"),
        serialized[0]);
}

TEST_F(InternalUnpackBucketInternalizeProjectTest, InternalizesInclusionThatImplicitlyIncludesId) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'foo'}}"),
                   fromjson("{$project: {x: true, y: true}}")),
        getExpCtx());
    ASSERT_EQ(2u, pipeline->getSources().size());
    auto& container = pipeline->getSources();

    dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.begin()->get())
        ->internalizeProject(container.begin(), &container);

    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(1u, serialized.size());
    ASSERT_BSONOBJ_EQ(
        fromjson("{$_internalUnpackBucket: { include: ['_id', 'x', 'y'], timeField: 'foo'}}"),
        serialized[0]);
}

TEST_F(InternalUnpackBucketInternalizeProjectTest, InternalizesPartOfInclusionProject) {
    auto projectSpecObj = fromjson("{$project: {_id: true, x: {y: true}, a: {b: '$c'}}}");
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'foo'}}"),
                   projectSpecObj),
        getExpCtx());
    ASSERT_EQ(2u, pipeline->getSources().size());
    auto& container = pipeline->getSources();

    dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.begin()->get())
        ->internalizeProject(container.begin(), &container);

    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());
    ASSERT_BSONOBJ_EQ(
        fromjson("{$_internalUnpackBucket: { include: ['_id', 'a', 'c', 'x'], timeField: 'foo'}}"),
        serialized[0]);
    ASSERT_BSONOBJ_EQ(projectSpecObj, serialized[1]);
}

TEST_F(InternalUnpackBucketInternalizeProjectTest,
       InternalizesPartOfInclusionProjectButExcludesId) {
    auto projectSpecObj = fromjson("{$project: {x: {y: true}, a: {b: '$c'}, _id: false}}");
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'foo'}}"),
                   projectSpecObj),
        getExpCtx());
    ASSERT_EQ(2u, pipeline->getSources().size());
    auto& container = pipeline->getSources();

    dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.begin()->get())
        ->internalizeProject(container.begin(), &container);

    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());
    ASSERT_BSONOBJ_EQ(
        fromjson("{$_internalUnpackBucket: { include: ['a', 'c', 'x'], timeField: 'foo'}}"),
        serialized[0]);
    ASSERT_BSONOBJ_EQ(projectSpecObj, serialized[1]);
}

TEST_F(InternalUnpackBucketInternalizeProjectTest, InternalizesExclusionProject) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'foo'}}"),
                   fromjson("{$project: {_id: false, x: false}}")),
        getExpCtx());
    ASSERT_EQ(2u, pipeline->getSources().size());
    auto& container = pipeline->getSources();

    dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.begin()->get())
        ->internalizeProject(container.begin(), &container);

    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(1u, serialized.size());
    ASSERT_BSONOBJ_EQ(
        fromjson("{$_internalUnpackBucket: { exclude: ['_id', 'x'], timeField: 'foo'}}"),
        serialized[0]);
}

TEST_F(InternalUnpackBucketInternalizeProjectTest, InternalizesExclusionProjectButIncludesId) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'foo'}}"),
                   fromjson("{$project: {_id: true, x: false}}")),
        getExpCtx());
    ASSERT_EQ(2u, pipeline->getSources().size());
    auto& container = pipeline->getSources();

    dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.begin()->get())
        ->internalizeProject(container.begin(), &container);

    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(1u, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$_internalUnpackBucket: { exclude: ['x'], timeField: 'foo'}}"),
                      serialized[0]);
}

TEST_F(InternalUnpackBucketInternalizeProjectTest,
       InternalizesExclusionProjectThatImplicitlyIncludesId) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'foo'}}"),
                   fromjson("{$project: {x: false}}")),
        getExpCtx());
    ASSERT_EQ(2u, pipeline->getSources().size());
    auto& container = pipeline->getSources();

    dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.begin()->get())
        ->internalizeProject(container.begin(), &container);

    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(1u, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$_internalUnpackBucket: { exclude: ['x'], timeField: 'foo'}}"),
                      serialized[0]);
}

TEST_F(InternalUnpackBucketInternalizeProjectTest, InternalizesPartOfExclusionProjectExcludesId) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'foo'}}"),
                   fromjson("{$project: {x: {y: false}, _id: false}}")),
        getExpCtx());
    ASSERT_EQ(2u, pipeline->getSources().size());
    auto& container = pipeline->getSources();

    dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.begin()->get())
        ->internalizeProject(container.begin(), &container);

    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$_internalUnpackBucket: { exclude: ['_id'], timeField: 'foo'}}"),
                      serialized[0]);
    ASSERT_BSONOBJ_EQ(fromjson("{$project: {x: {y: false}, _id: true}}"), serialized[1]);
}

TEST_F(InternalUnpackBucketInternalizeProjectTest,
       InternalizesPartOfExclusionProjectImplicitlyIncludesId) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'foo'}}"),
                   fromjson("{$project: {x: {y: false}, z: false}}")),
        getExpCtx());
    ASSERT_EQ(2u, pipeline->getSources().size());
    auto& container = pipeline->getSources();

    dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.begin()->get())
        ->internalizeProject(container.begin(), &container);

    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$_internalUnpackBucket: { exclude: ['z'], timeField: 'foo'}}"),
                      serialized[0]);
    ASSERT_BSONOBJ_EQ(fromjson("{$project: {x: {y: false}, _id: true}}"), serialized[1]);
}

TEST_F(InternalUnpackBucketInternalizeProjectTest,
       InternalizesPartOfExclusionProjectIncludesNestedId) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'foo'}}"),
                   fromjson("{$project: {x: false, _id: {y: false}}}")),
        getExpCtx());
    ASSERT_EQ(2u, pipeline->getSources().size());
    auto& container = pipeline->getSources();

    dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.begin()->get())
        ->internalizeProject(container.begin(), &container);

    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$_internalUnpackBucket: { exclude: ['x'], timeField: 'foo'}}"),
                      serialized[0]);
    ASSERT_BSONOBJ_EQ(fromjson("{$project: {_id: {y: false}}}"), serialized[1]);
}

TEST_F(InternalUnpackBucketInternalizeProjectTest, InternalizesNonBoolInclusionProject) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'foo'}}"),
                   fromjson("{$project: {_id: 1, x: 1.0, y: 1.5}}")),
        getExpCtx());
    ASSERT_EQ(2u, pipeline->getSources().size());
    auto& container = pipeline->getSources();

    dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.begin()->get())
        ->internalizeProject(container.begin(), &container);

    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(1u, serialized.size());
    ASSERT_BSONOBJ_EQ(
        fromjson("{$_internalUnpackBucket: { include: ['_id', 'x', 'y'], timeField: 'foo'}}"),
        serialized[0]);
}

TEST_F(InternalUnpackBucketInternalizeProjectTest, InternalizesWhenInMiddleOfPipeline) {
    auto matchSpecObj = fromjson("{$match: {'meta.source': 'primary'}}");
    auto pipeline = Pipeline::parse(
        makeVector(matchSpecObj,
                   fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'foo'}}"),
                   fromjson("{$project: {_id: false, x: true, y: true}}")),
        getExpCtx());
    ASSERT_EQ(3u, pipeline->getSources().size());
    auto& container = pipeline->getSources();

    dynamic_cast<DocumentSourceInternalUnpackBucket*>(std::next(container.begin())->get())
        ->internalizeProject(std::next(container.begin()), &container);

    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());
    ASSERT_BSONOBJ_EQ(matchSpecObj, serialized[0]);
    ASSERT_BSONOBJ_EQ(
        fromjson("{$_internalUnpackBucket: { include: ['x', 'y'], timeField: 'foo'}}"),
        serialized[1]);
}

TEST_F(InternalUnpackBucketInternalizeProjectTest, DoesNotInternalizeWhenNoProjectFollows) {
    auto unpackBucketSpecObj =
        fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'foo'}}");
    auto groupSpecObj = fromjson("{$group: {_id: {$const: null}, count: { $sum: {$const: 1 }}}}");
    auto pipeline = Pipeline::parse(makeVector(unpackBucketSpecObj, groupSpecObj), getExpCtx());
    ASSERT_EQ(2u, pipeline->getSources().size());
    auto& container = pipeline->getSources();

    dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.begin()->get())
        ->internalizeProject(container.begin(), &container);

    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());
    ASSERT_BSONOBJ_EQ(unpackBucketSpecObj, serialized[0]);
    ASSERT_BSONOBJ_EQ(groupSpecObj, serialized[1]);
}

TEST_F(InternalUnpackBucketInternalizeProjectTest,
       DoesNotInternalizeWhenUnpackBucketAlreadyExcludes) {
    auto unpackBucketSpecObj =
        fromjson("{$_internalUnpackBucket: { exclude: ['a'], timeField: 'foo'}}");
    auto projectSpecObj = fromjson("{$project: {_id: true}}");
    auto pipeline = Pipeline::parse(makeVector(unpackBucketSpecObj, projectSpecObj), getExpCtx());
    ASSERT_EQ(2u, pipeline->getSources().size());
    auto& container = pipeline->getSources();

    dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.begin()->get())
        ->internalizeProject(container.begin(), &container);

    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());
    ASSERT_BSONOBJ_EQ(unpackBucketSpecObj, serialized[0]);
    ASSERT_BSONOBJ_EQ(projectSpecObj, serialized[1]);
}

TEST_F(InternalUnpackBucketInternalizeProjectTest,
       DoesNotInternalizeWhenUnpackBucketAlreadyIncludes) {
    auto unpackBucketSpecObj =
        fromjson("{$_internalUnpackBucket: { include: ['a'], timeField: 'foo'}}");
    auto projectSpecObj = fromjson("{$project: {_id: true}}");
    auto pipeline = Pipeline::parse(makeVector(unpackBucketSpecObj, projectSpecObj), getExpCtx());
    ASSERT_EQ(2u, pipeline->getSources().size());
    auto& container = pipeline->getSources();

    dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.begin()->get())
        ->internalizeProject(container.begin(), &container);

    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());
    ASSERT_BSONOBJ_EQ(unpackBucketSpecObj, serialized[0]);
    ASSERT_BSONOBJ_EQ(projectSpecObj, serialized[1]);
}

TEST_F(InternalUnpackBucketInternalizeProjectTest,
       InternalizeProjectUpdatesMetaAndTimeFieldStateInclusionProj) {
    auto pipeline = Pipeline::parse(
        makeVector(
            fromjson(
                "{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: 'meta'}}"),
            fromjson("{$project: {meta: true, _id: true}}")),
        getExpCtx());
    ASSERT_EQ(2u, pipeline->getSources().size());
    auto& container = pipeline->getSources();

    auto unpack = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.begin()->get());
    unpack->internalizeProject(container.begin(), &container);

    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(1u, serialized.size());
    ASSERT_BSONOBJ_EQ(
        fromjson(
            "{$_internalUnpackBucket: { include: ['_id'], timeField: 'time', metaField: 'meta'}}"),
        serialized[0]);
    ASSERT_TRUE(unpack->includeMetaField());
    ASSERT_FALSE(unpack->includeTimeField());
}

TEST_F(InternalUnpackBucketInternalizeProjectTest,
       InternalizeProjectUpdatesMetaAndTimeFieldStateExclusionProj) {
    auto unpackBucketSpecObj = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: 'myMeta'}}");
    auto pipeline = Pipeline::parse(
        makeVector(unpackBucketSpecObj, fromjson("{$project: {myMeta: false}}")), getExpCtx());
    ASSERT_EQ(2u, pipeline->getSources().size());
    auto& container = pipeline->getSources();

    auto unpack = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.begin()->get());
    unpack->internalizeProject(container.begin(), &container);

    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(1u, serialized.size());
    ASSERT_BSONOBJ_EQ(unpackBucketSpecObj, serialized[0]);
    ASSERT_FALSE(unpack->includeMetaField());
    ASSERT_TRUE(unpack->includeTimeField());
}
}  // namespace
}  // namespace mongo
