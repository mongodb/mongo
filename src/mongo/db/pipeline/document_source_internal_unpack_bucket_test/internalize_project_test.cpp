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
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"

#include <memory>
#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

using InternalUnpackBucketInternalizeProjectTest = AggregationContextFixture;

TEST_F(InternalUnpackBucketInternalizeProjectTest, InternalizesInclusionProject) {
    auto unpack = DocumentSourceInternalUnpackBucket::createFromBsonInternal(
        fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'foo', bucketMaxSpanSeconds: "
                 "3600}}")
            .firstElement(),
        getExpCtx());

    dynamic_cast<DocumentSourceInternalUnpackBucket*>(unpack.get())
        ->internalizeProject(fromjson("{x: true, y: true, _id: true}"), true);

    std::vector<Value> serializedArray;
    unpack->serializeToArray(serializedArray);
    ASSERT_BSONOBJ_EQ(fromjson("{$_internalUnpackBucket: { include: ['_id', 'x', 'y'], timeField: "
                               "'foo', bucketMaxSpanSeconds: 3600}}"),
                      serializedArray[0].getDocument().toBson());
}

TEST_F(InternalUnpackBucketInternalizeProjectTest, InternalizesInclusionButExcludesId) {
    auto unpack = DocumentSourceInternalUnpackBucket::createFromBsonInternal(
        fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'foo', bucketMaxSpanSeconds: "
                 "3600}}")
            .firstElement(),
        getExpCtx());

    dynamic_cast<DocumentSourceInternalUnpackBucket*>(unpack.get())
        ->internalizeProject(fromjson("{x: true, y: true, _id: false}"), true);

    std::vector<Value> serializedArray;
    unpack->serializeToArray(serializedArray);
    ASSERT_BSONOBJ_EQ(fromjson("{$_internalUnpackBucket: { include: ['x', 'y'], timeField: 'foo', "
                               "bucketMaxSpanSeconds: 3600}}"),
                      serializedArray[0].getDocument().toBson());
}


TEST_F(InternalUnpackBucketInternalizeProjectTest, InternalizesNonBoolInclusionProject) {
    auto unpack = DocumentSourceInternalUnpackBucket::createFromBsonInternal(
        fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'foo', bucketMaxSpanSeconds: "
                 "3600}}")
            .firstElement(),
        getExpCtx());

    dynamic_cast<DocumentSourceInternalUnpackBucket*>(unpack.get())
        ->internalizeProject(fromjson("{_id: 0, x: 1, y: 1}"), true);

    std::vector<Value> serializedArray;
    unpack->serializeToArray(serializedArray);
    ASSERT_BSONOBJ_EQ(fromjson("{$_internalUnpackBucket: { include: ['x', 'y'], timeField: 'foo', "
                               "bucketMaxSpanSeconds: 3600}}"),
                      serializedArray[0].getDocument().toBson());
}

TEST_F(InternalUnpackBucketInternalizeProjectTest, InternalizesExclusionProject) {
    auto unpack = DocumentSourceInternalUnpackBucket::createFromBsonInternal(
        fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'foo', bucketMaxSpanSeconds: "
                 "3600}}")
            .firstElement(),
        getExpCtx());

    dynamic_cast<DocumentSourceInternalUnpackBucket*>(unpack.get())
        ->internalizeProject(fromjson("{_id: false, x: false}"), false);

    std::vector<Value> serializedArray;
    unpack->serializeToArray(serializedArray);
    ASSERT_BSONOBJ_EQ(fromjson("{$_internalUnpackBucket: { exclude: ['_id', 'x'], timeField: "
                               "'foo', bucketMaxSpanSeconds: 3600}}"),
                      serializedArray[0].getDocument().toBson());
}

TEST_F(InternalUnpackBucketInternalizeProjectTest, InternalizesExclusionProjectButIncludesId) {
    auto unpack = DocumentSourceInternalUnpackBucket::createFromBsonInternal(
        fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'foo', bucketMaxSpanSeconds: "
                 "3600}}")
            .firstElement(),
        getExpCtx());

    dynamic_cast<DocumentSourceInternalUnpackBucket*>(unpack.get())
        ->internalizeProject(fromjson("{_id: true, x: false}"), false);

    std::vector<Value> serializedArray;
    unpack->serializeToArray(serializedArray);
    ASSERT_BSONOBJ_EQ(fromjson("{$_internalUnpackBucket: { exclude: ['x'], timeField: 'foo', "
                               "bucketMaxSpanSeconds: 3600}}"),
                      serializedArray[0].getDocument().toBson());
}


TEST_F(InternalUnpackBucketInternalizeProjectTest, InternalizesNonBoolExclusionProject) {
    auto unpack = DocumentSourceInternalUnpackBucket::createFromBsonInternal(
        fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'foo', bucketMaxSpanSeconds: "
                 "3600}}")
            .firstElement(),
        getExpCtx());

    dynamic_cast<DocumentSourceInternalUnpackBucket*>(unpack.get())
        ->internalizeProject(fromjson("{_id: 1, x: 0, y: 0}"), false);

    std::vector<Value> serializedArray;
    unpack->serializeToArray(serializedArray);
    ASSERT_BSONOBJ_EQ(fromjson("{$_internalUnpackBucket: { exclude: ['x', 'y'], timeField: 'foo', "
                               "bucketMaxSpanSeconds: 3600}}"),
                      serializedArray[0].getDocument().toBson());
}

TEST_F(InternalUnpackBucketInternalizeProjectTest,
       InternalizeProjectUpdatesMetaAndTimeFieldStateInclusionProj) {
    auto unpackSpec = DocumentSourceInternalUnpackBucket::createFromBsonInternal(
        fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: 'meta', "
                 "bucketMaxSpanSeconds: 3600}}")
            .firstElement(),
        getExpCtx());

    auto unpack = dynamic_cast<DocumentSourceInternalUnpackBucket*>(unpackSpec.get());
    unpack->internalizeProject(fromjson("{meta: true, _id: true}"), true);

    std::vector<Value> serializedArray;
    unpack->serializeToArray(serializedArray);
    ASSERT_BSONOBJ_EQ(fromjson("{$_internalUnpackBucket: { include: ['_id', 'meta'], timeField: "
                               "'time', metaField: 'meta', bucketMaxSpanSeconds: 3600}}"),
                      serializedArray[0].getDocument().toBson());
    ASSERT_TRUE(unpack->includeMetaField());
    ASSERT_FALSE(unpack->includeTimeField());
}

TEST_F(InternalUnpackBucketInternalizeProjectTest,
       InternalizeProjectUpdatesMetaAndTimeFieldStateExclusionProj) {
    auto unpackSpec = DocumentSourceInternalUnpackBucket::createFromBsonInternal(
        fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: 'myMeta', "
                 "bucketMaxSpanSeconds: 3600}}")
            .firstElement(),
        getExpCtx());

    auto unpack = dynamic_cast<DocumentSourceInternalUnpackBucket*>(unpackSpec.get());
    unpack->internalizeProject(fromjson("{myMeta: false}"), false);

    std::vector<Value> serializedArray;
    unpack->serializeToArray(serializedArray);
    ASSERT_BSONOBJ_EQ(fromjson("{$_internalUnpackBucket: { exclude: ['myMeta'], timeField: 'time', "
                               "metaField: 'myMeta', bucketMaxSpanSeconds: 3600}}"),
                      serializedArray[0].getDocument().toBson());
    ASSERT_FALSE(unpack->includeMetaField());
    ASSERT_TRUE(unpack->includeTimeField());
}

TEST_F(InternalUnpackBucketInternalizeProjectTest, OptimizeCorrectlyInternalizesAndRemovesProject) {
    auto pipeline =
        Pipeline::parse(makeVector(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: "
                                            "'foo', bucketMaxSpanSeconds: 3600}}"),
                                   fromjson("{$project: {x: true, y: true}}")),
                        getExpCtx());

    pipeline->optimizePipeline();

    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(1u, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$_internalUnpackBucket: { include: ['_id', 'x', 'y'], timeField: "
                               "'foo', bucketMaxSpanSeconds: 3600}}"),
                      serialized[0]);
}

TEST_F(InternalUnpackBucketInternalizeProjectTest, OptimizeCorrectlyInternalizesDependencyProject) {
    auto projectSpec = fromjson("{$project: {_id: false, x: false}}");
    auto sortSpec = fromjson("{$sort: {y: 1}}");
    auto groupSpec = fromjson("{$group: {_id: '$y', f: {$first: '$z'}, $willBeMerged: false}}");
    auto pipeline =
        Pipeline::parse(makeVector(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: "
                                            "'foo', bucketMaxSpanSeconds: 3600}}"),
                                   projectSpec,
                                   sortSpec,
                                   groupSpec),
                        getExpCtx());

    pipeline->optimizePipeline();

    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(4u, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$_internalUnpackBucket: { include: ['y', 'z'], timeField: 'foo', "
                               "bucketMaxSpanSeconds: 3600}}"),
                      serialized[0]);
    ASSERT_BSONOBJ_EQ(projectSpec, serialized[1]);
    ASSERT_BSONOBJ_EQ(sortSpec, serialized[2]);
    ASSERT_BSONOBJ_EQ(groupSpec, serialized[3]);
}

TEST_F(InternalUnpackBucketInternalizeProjectTest,
       OptimizeDoesNotInternalizeWhenUnpackBucketAlreadyExcludes) {
    auto unpackSpec = fromjson(
        "{$_internalUnpackBucket: { exclude: ['a'], timeField: 'foo', bucketMaxSpanSeconds: "
        "3600}}");
    auto projectSpec = fromjson("{$project: {a: true, _id: false}}");
    auto pipeline = Pipeline::parse(makeVector(unpackSpec, projectSpec), getExpCtx());

    pipeline->optimizePipeline();

    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());
    ASSERT_BSONOBJ_EQ(unpackSpec, serialized[0]);
    ASSERT_BSONOBJ_EQ(projectSpec, serialized[1]);
}

TEST_F(InternalUnpackBucketInternalizeProjectTest,
       OptimizeDoesNotInternalizeWhenUnpackBucketAlreadyIncludes) {
    auto unpackSpec = fromjson(
        "{$_internalUnpackBucket: { include: ['a'], timeField: 'foo', bucketMaxSpanSeconds: "
        "3600}}");
    auto projectSpec = fromjson("{$project: {_id: true, a: true}}");
    auto pipeline = Pipeline::parse(makeVector(unpackSpec, projectSpec), getExpCtx());

    pipeline->optimizePipeline();

    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());
    ASSERT_BSONOBJ_EQ(unpackSpec, serialized[0]);
    ASSERT_BSONOBJ_EQ(projectSpec, serialized[1]);
}
}  // namespace
}  // namespace mongo
