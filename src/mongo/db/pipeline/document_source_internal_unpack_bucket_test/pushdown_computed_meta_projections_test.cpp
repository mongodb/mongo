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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"

#include <list>
#include <memory>
#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

using InternalUnpackBucketPushdownProjectionsTest = AggregationContextFixture;

/************ Optimize $addFields stage with computed meta projections *******************/
TEST_F(InternalUnpackBucketPushdownProjectionsTest,
       OptimizeAddFieldsWithMetaProjectionSingleField) {
    auto unpackSpecObj = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'foo', metaField: 'userMeta', "
        "bucketMaxSpanSeconds: 3600}}");
    auto addFieldsSpecObj = fromjson("{$addFields: {newMeta: {$toUpper : '$userMeta'}}}");

    auto pipeline = Pipeline::parse(makeVector(unpackSpecObj, addFieldsSpecObj), getExpCtx());
    auto& container = pipeline->getSources();
    auto unpack = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.begin()->get());
    auto pushedDownAddFieldsStage =
        unpack->pushDownComputedMetaProjection(container.begin(), &container);

    ASSERT(pushedDownAddFieldsStage);
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$addFields: {newMeta: {$toUpper: ['$meta']}}}"), serialized[0]);
    auto extraField = fromjson("{computedMetaProjFields: ['newMeta']}");
    ASSERT_BSONOBJ_EQ(BSON("$_internalUnpackBucket" << unpackSpecObj.firstElement().Obj().addField(
                               extraField.firstElement())),
                      serialized[1]);
}

TEST_F(InternalUnpackBucketPushdownProjectionsTest, OptimizeAddFieldsWithMetaProjectionDocument) {
    auto unpackSpecObj = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'foo', metaField: 'myMeta', "
        "bucketMaxSpanSeconds: 3600}}");
    auto addFieldsSpecObj =
        fromjson("{$addFields: {newMeta: {$concat: ['$myMeta.a', '$myMeta.b']}}}");

    auto pipeline = Pipeline::parse(makeVector(unpackSpecObj, addFieldsSpecObj), getExpCtx());
    auto& container = pipeline->getSources();
    auto unpack = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.begin()->get());
    auto pushedDownAddFieldsStage =
        unpack->pushDownComputedMetaProjection(container.begin(), &container);

    ASSERT(pushedDownAddFieldsStage);
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$addFields: {newMeta: {$concat: ['$meta.a', '$meta.b']}}}"),
                      serialized[0]);
    auto extraField = fromjson("{computedMetaProjFields: ['newMeta']}");
    ASSERT_BSONOBJ_EQ(BSON("$_internalUnpackBucket" << unpackSpecObj.firstElement().Obj().addField(
                               extraField.firstElement())),
                      serialized[1]);
}

TEST_F(InternalUnpackBucketPushdownProjectionsTest, OptimizeAddFieldsWith2MetaProjections) {
    auto unpackSpecObj = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'foo', metaField: 'myMeta', "
        "bucketMaxSpanSeconds: 3600}}");
    auto addFieldsSpecObj =
        fromjson("{$addFields: {device: '$myMeta.a', deviceType: '$myMeta.b'}}");

    auto pipeline = Pipeline::parse(makeVector(unpackSpecObj, addFieldsSpecObj), getExpCtx());
    auto& container = pipeline->getSources();
    auto unpack = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.begin()->get());
    auto pushedDownAddFieldsStage =
        unpack->pushDownComputedMetaProjection(container.begin(), &container);

    ASSERT(pushedDownAddFieldsStage);
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$addFields: {device: '$meta.a', deviceType: '$meta.b'}}"),
                      serialized[0]);
    auto extraField = fromjson("{computedMetaProjFields: ['device', 'deviceType']}");
    ASSERT_BSONOBJ_EQ(BSON("$_internalUnpackBucket" << unpackSpecObj.firstElement().Obj().addField(
                               extraField.firstElement())),
                      serialized[1]);
}

TEST_F(InternalUnpackBucketPushdownProjectionsTest, SplitAddFieldsWithMixedProjectionFields) {
    auto unpackSpecObj = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'foo', metaField: "
        "'myMeta',bucketMaxSpanSeconds: 3600}}");
    auto addFieldsSpecObj =
        fromjson("{$addFields: {device: '$myMeta.a', temp: {$add: ['$temperature', '$offset']}}}");

    auto pipeline = Pipeline::parse(makeVector(unpackSpecObj, addFieldsSpecObj), getExpCtx());
    auto& container = pipeline->getSources();
    auto unpack = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.begin()->get());
    auto pushedDownAddFieldsStage =
        unpack->pushDownComputedMetaProjection(container.begin(), &container);
    ASSERT(pushedDownAddFieldsStage);

    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(3u, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$addFields: {device: '$meta.a'}}"), serialized[0]);
    auto extraField = fromjson("{computedMetaProjFields: ['device']}");
    ASSERT_BSONOBJ_EQ(BSON("$_internalUnpackBucket" << unpackSpecObj.firstElement().Obj().addField(
                               extraField.firstElement())),
                      serialized[1]);
    ASSERT_BSONOBJ_EQ(fromjson("{$addFields: {temp: {$add: ['$temperature', '$offset']}}}"),
                      serialized[2]);
}

TEST_F(InternalUnpackBucketPushdownProjectionsTest, DoNotSplitAddFieldsWithMetaProjectionInSuffix) {
    auto unpackSpecObj = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'foo', metaField: 'myMeta', "
        "bucketMaxSpanSeconds: 3600}}");
    auto addFieldsSpecObj = fromjson("{$addFields: {temp: '$temperature', device: '$myMeta.a'}}");

    auto pipeline = Pipeline::parse(makeVector(unpackSpecObj, addFieldsSpecObj), getExpCtx());
    auto& container = pipeline->getSources();
    auto unpack = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.begin()->get());
    auto pushedDownAddFieldsStage =
        unpack->pushDownComputedMetaProjection(container.begin(), &container);

    ASSERT_FALSE(pushedDownAddFieldsStage);
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());
    ASSERT_BSONOBJ_EQ(unpackSpecObj, serialized[0]);
    ASSERT_BSONOBJ_EQ(addFieldsSpecObj, serialized[1]);
}

TEST_F(InternalUnpackBucketPushdownProjectionsTest, DoNotOptimizeAddFieldsWithMixedProjection) {
    auto unpackSpecObj = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'foo', metaField: 'myMeta', "
        "bucketMaxSpanSeconds: 3600}}");
    auto addFieldsSpecObj =
        fromjson("{$addFields: {newMeta: {$add: ['$myMeta.a', '$temperature']}}}");

    auto pipeline = Pipeline::parse(makeVector(unpackSpecObj, addFieldsSpecObj), getExpCtx());
    auto& container = pipeline->getSources();
    auto unpack = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.begin()->get());
    auto pushedDownAddFieldsStage =
        unpack->pushDownComputedMetaProjection(container.begin(), &container);

    ASSERT_FALSE(pushedDownAddFieldsStage);
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());
    ASSERT_BSONOBJ_EQ(unpackSpecObj, serialized[0]);
    ASSERT_BSONOBJ_EQ(addFieldsSpecObj, serialized[1]);
}

TEST_F(InternalUnpackBucketPushdownProjectionsTest, DoNotOptimizeAddFieldsWithMissingMetaField) {
    auto unpackSpecObj = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'foo', bucketMaxSpanSeconds: 3600}}");
    auto addFieldsSpecObj = fromjson("{$addFields: {newMeta: '$myMeta'}}");

    auto pipeline = Pipeline::parse(makeVector(unpackSpecObj, addFieldsSpecObj), getExpCtx());
    auto& container = pipeline->getSources();
    auto unpack = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.begin()->get());
    auto pushedDownAddFieldsStage =
        unpack->pushDownComputedMetaProjection(container.begin(), &container);

    ASSERT_FALSE(pushedDownAddFieldsStage);
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());
    ASSERT_BSONOBJ_EQ(unpackSpecObj, serialized[0]);
    ASSERT_BSONOBJ_EQ(addFieldsSpecObj, serialized[1]);
}

TEST_F(InternalUnpackBucketPushdownProjectionsTest,
       DoNotPushdownAddFieldsWithReservedBucketFieldName) {
    auto unpackSpecObj = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'foo', metaField: 'myMeta', "
        "bucketMaxSpanSeconds: 3600}}");
    auto addFieldsSpecObj = fromjson("{$addFields: {data: '$myMeta'}}");

    auto pipeline = Pipeline::parse(makeVector(unpackSpecObj, addFieldsSpecObj), getExpCtx());
    auto& container = pipeline->getSources();
    auto unpack = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.begin()->get());
    auto pushedDownAddFieldsStage =
        unpack->pushDownComputedMetaProjection(container.begin(), &container);

    ASSERT_FALSE(pushedDownAddFieldsStage);
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());
    ASSERT_BSONOBJ_EQ(unpackSpecObj, serialized[0]);
    ASSERT_BSONOBJ_EQ(addFieldsSpecObj, serialized[1]);
}

TEST_F(InternalUnpackBucketPushdownProjectionsTest,
       DoNotPushdownNestedFieldWithReservedBucketFieldName) {
    auto unpackSpecObj = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'foo', metaField: 'myMeta', "
        "bucketMaxSpanSeconds: 3600}}");
    auto addFieldsSpecObj = fromjson("{$addFields: {data : {x: '$myMeta'}}}");

    auto pipeline = Pipeline::parse(makeVector(unpackSpecObj, addFieldsSpecObj), getExpCtx());
    auto& container = pipeline->getSources();
    auto unpack = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.begin()->get());
    auto pushedDownAddFieldsStage =
        unpack->pushDownComputedMetaProjection(container.begin(), &container);

    ASSERT_FALSE(pushedDownAddFieldsStage);
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());
    ASSERT_BSONOBJ_EQ(unpackSpecObj, serialized[0]);
    ASSERT_BSONOBJ_EQ(addFieldsSpecObj, serialized[1]);
}

/************ Optimize $project stage with computed meta projections *******************/

TEST_F(InternalUnpackBucketPushdownProjectionsTest,
       PushDownComputedMetaProjectionReplaceWithProjField) {
    auto unpackSpecObj = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: 'myMeta', "
        "bucketMaxSpanSeconds: 3600}}");
    auto projectSpecObj = fromjson("{$project: {_id : true, device: '$myMeta.a'}}");

    auto pipeline = Pipeline::parse(makeVector(unpackSpecObj, projectSpecObj), getExpCtx());
    auto& container = pipeline->getSources();

    auto unpack = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.begin()->get());
    auto pushedDownAddFieldsStage =
        unpack->pushDownComputedMetaProjection(container.begin(), &container);

    ASSERT(pushedDownAddFieldsStage);
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(3u, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$addFields: { device: '$meta.a'}}"), serialized[0]);
    ASSERT_BSONOBJ_EQ(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'time', "
                               "metaField: 'myMeta', bucketMaxSpanSeconds: 3600, "
                               "computedMetaProjFields: ['device']}}"),
                      serialized[1]);
    ASSERT_BSONOBJ_EQ(fromjson("{$project: {_id: true, device: true}}"), serialized[2]);
}

TEST_F(InternalUnpackBucketPushdownProjectionsTest,
       PushDownComputedMetaProjectionReplaceWithIdentityProj) {
    auto unpackSpecObj = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: 'myMeta', "
        "bucketMaxSpanSeconds: 3600}}");
    auto projectSpecObj =
        fromjson("{$project: {_id: true, x: true, 'y.z' : true, device: '$myMeta.a'}}");

    auto pipeline = Pipeline::parse(makeVector(unpackSpecObj, projectSpecObj), getExpCtx());
    auto& container = pipeline->getSources();

    auto unpack = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.begin()->get());
    auto pushedDownAddFieldsStage =
        unpack->pushDownComputedMetaProjection(container.begin(), &container);

    ASSERT(pushedDownAddFieldsStage);
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(3u, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$addFields: { device: '$meta.a'}}"), serialized[0]);
    ASSERT_BSONOBJ_EQ(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'time', "
                               "metaField: 'myMeta', bucketMaxSpanSeconds: 3600, "
                               "computedMetaProjFields: ['device']}}"),
                      serialized[1]);
    ASSERT_BSONOBJ_EQ(
        fromjson("{$project: {_id: true, x: true, y : {z: true}, device: '$device'}}"),
        serialized[2]);
}

TEST_F(InternalUnpackBucketPushdownProjectionsTest, DoNotPushDownMixedProjection) {
    auto unpackSpecObj = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: 'myMeta', "
        "bucketMaxSpanSeconds: 3600}}");
    auto projectSpecObj = fromjson(
        "{$project: {_id: true, x: true, newMeta: {$add: ['$myMeta.a', '$temperature']}}}");

    auto pipeline = Pipeline::parse(makeVector(unpackSpecObj, projectSpecObj), getExpCtx());
    auto& container = pipeline->getSources();

    auto unpack = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.begin()->get());
    auto pushedDownAddFieldsStage =
        unpack->pushDownComputedMetaProjection(container.begin(), &container);

    ASSERT_FALSE(pushedDownAddFieldsStage);
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());
    ASSERT_BSONOBJ_EQ(projectSpecObj, serialized[1]);
}

TEST_F(InternalUnpackBucketPushdownProjectionsTest,
       DoNotPushDownProjectionWithReservedBucketField) {
    auto unpackSpecObj = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: 'myMeta', "
        "bucketMaxSpanSeconds: 3600}}");
    auto projectSpecObj =
        fromjson("{$project: {_id: true, x: true, meta: {$add: ['$myMeta.a', '$myMeta.b']}}}");

    auto pipeline = Pipeline::parse(makeVector(unpackSpecObj, projectSpecObj), getExpCtx());
    auto& container = pipeline->getSources();
    auto unpack = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.begin()->get());
    auto pushedDownAddFieldsStage =
        unpack->pushDownComputedMetaProjection(container.begin(), &container);

    ASSERT_FALSE(pushedDownAddFieldsStage);
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());
    ASSERT_BSONOBJ_EQ(projectSpecObj, serialized[1]);
}

TEST_F(InternalUnpackBucketPushdownProjectionsTest, DoNotPushDownNestedProjectionWithReservedName) {
    auto unpackSpecObj = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: 'myMeta', "
        "bucketMaxSpanSeconds: 3600}}");
    auto projectSpecObj =
        fromjson("{$project: {_id: true, x: true, data: {z: {$add: ['$myMeta.a', '$myMeta.b']}}}}");

    auto pipeline = Pipeline::parse(makeVector(unpackSpecObj, projectSpecObj), getExpCtx());
    auto& container = pipeline->getSources();
    auto unpack = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.begin()->get());
    auto pushedDownAddFieldsStage =
        unpack->pushDownComputedMetaProjection(container.begin(), &container);

    ASSERT_FALSE(pushedDownAddFieldsStage);
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());
    ASSERT_BSONOBJ_EQ(projectSpecObj, serialized[1]);
}

/****************** $project stage with $getField expression ****************************/

// We do not push down projections with the '$getField' expression when the input to '$getField' is
// just a string. In this case $getField will always prepend the $$CURRENT field path for string
// inputs and thus also require the 'needWholeDocument' dependency. So for all values of {$getField:
// "string"} we will not perform this rewrite. Even though, we could perform the rewrite here when
// the string is the metaField, the server cannot differentiate between 'meta' and '$meta' field
// paths, where one is the metaField and the other is not in the expression dependencies. To avoid
// incorrect query results in this edge case, we restrict all rewrites with {$getField: "string"}.
// Note that we do not expect users to use $getField to query their metaField.
TEST_F(InternalUnpackBucketPushdownProjectionsTest,
       DoNotPushDownNestedProjectionWithGetFieldJustString) {
    auto unpackSpecObj = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: 'myMeta', "
        "bucketMaxSpanSeconds: 3600}}");
    auto projectSpecObj = fromjson(
        "{$project: {_id: true, x: true, data: {z: {$add: [{$getField: 'myMeta'}, "
        "'$myMeta.b']}}}}");

    auto pipeline = Pipeline::parse(makeVector(unpackSpecObj, projectSpecObj), getExpCtx());
    auto& container = pipeline->getSources();
    auto unpack = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.begin()->get());
    auto pushedDownAddFieldsStage =
        unpack->pushDownComputedMetaProjection(container.begin(), &container);

    ASSERT_FALSE(pushedDownAddFieldsStage);
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());
    auto projectSerialized = fromjson(
        "{$project: {_id: true, x: true, data: {z: {$add: [{$getField: { field: { $const: "
        "'myMeta'}, input:'$$CURRENT' } },'$myMeta.b']}}}}");

    ASSERT_BSONOBJ_EQ(projectSerialized, serialized[1]);
}

// However, we can push down $getField if we have the entire path and do not rely on $$CURRENT. If
// the entire path is only on the metaField, we can pushdown the projection.
TEST_F(InternalUnpackBucketPushdownProjectionsTest, DoPushDownNestedProjectionWithGetFieldInput) {
    auto unpackSpecObj = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: 'myMeta', "
        "bucketMaxSpanSeconds: 3600}}");
    auto projectSpecObj =
        fromjson("{$project: {_id : true, device: {$getField: {input: '$myMeta', field:'a'}}}}");

    auto pipeline = Pipeline::parse(makeVector(unpackSpecObj, projectSpecObj), getExpCtx());
    auto& container = pipeline->getSources();
    auto unpack = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.begin()->get());
    auto pushedDownAddFieldsStage =
        unpack->pushDownComputedMetaProjection(container.begin(), &container);

    ASSERT(pushedDownAddFieldsStage);
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(3u, serialized.size());
    ASSERT_BSONOBJ_EQ(
        fromjson(
            "{$addFields: { device: { $getField: { field: { $const: 'a' }, input: '$meta' }}}}"),
        serialized[0]);
    ASSERT_BSONOBJ_EQ(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'time', "
                               "metaField: 'myMeta', bucketMaxSpanSeconds: 3600, "
                               "computedMetaProjFields: ['device']}}"),
                      serialized[1]);
    ASSERT_BSONOBJ_EQ(fromjson("{$project: {_id : true, device: true}}"), serialized[2]);
}

TEST_F(InternalUnpackBucketPushdownProjectionsTest,
       DoNotPushDownNestedProjectionWithMeasurementGetField) {
    auto unpackSpecObj = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: 'myMeta', "
        "bucketMaxSpanSeconds: 3600}}");
    auto projectSpecObj = fromjson(
        "{$project: {_id: true, x: true, data: {z: {$add: [{$getField: {input: '$other', "
        "field:'a'}},'$myMeta.b']}}}}");

    auto pipeline = Pipeline::parse(makeVector(unpackSpecObj, projectSpecObj), getExpCtx());
    auto& container = pipeline->getSources();
    auto unpack = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.begin()->get());
    auto pushedDownAddFieldsStage =
        unpack->pushDownComputedMetaProjection(container.begin(), &container);

    ASSERT_FALSE(pushedDownAddFieldsStage);
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());
    auto projectSerialized = fromjson(
        "{$project: {_id: true, x: true, data: {z: {$add: [{$getField: { field: {$const: 'a'}, "
        "input:'$other' } },'$myMeta.b']}}}}");

    ASSERT_BSONOBJ_EQ(projectSerialized, serialized[1]);
}

TEST_F(InternalUnpackBucketPushdownProjectionsTest,
       DoNotPushDownNestedProjectionWhichNeedsWholeDoc) {
    auto unpackSpecObj = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: 'myMeta', "
        "bucketMaxSpanSeconds: 3600}}");
    auto projectSpecObj = fromjson("{$project: {_id: true, x: true, data: '$$ROOT'}}");

    auto pipeline = Pipeline::parse(makeVector(unpackSpecObj, projectSpecObj), getExpCtx());
    auto& container = pipeline->getSources();
    auto unpack = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.begin()->get());
    auto pushedDownAddFieldsStage =
        unpack->pushDownComputedMetaProjection(container.begin(), &container);

    ASSERT_FALSE(pushedDownAddFieldsStage);
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());
    ASSERT_BSONOBJ_EQ(unpackSpecObj, serialized[0]);
    ASSERT_BSONOBJ_EQ(projectSpecObj, serialized[1]);
}

}  // namespace
}  // namespace mongo
