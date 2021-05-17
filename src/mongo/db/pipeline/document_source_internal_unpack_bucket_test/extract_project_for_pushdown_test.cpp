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
#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using InternalUnpackBucketExtractProjectForPushdownTest = AggregationContextFixture;

TEST_F(InternalUnpackBucketExtractProjectForPushdownTest, DoesNotExtractWhenNoMetaField) {
    auto unpack = DocumentSourceInternalUnpackBucket::createFromBsonInternal(
        fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'foo', bucketMaxSpanSeconds: "
                 "3600}}")
            .firstElement(),
        getExpCtx());
    auto project = DocumentSourceProject::createFromBson(
        fromjson("{$project: {meta: 0}}").firstElement(), getExpCtx());

    auto [extractedProj, deleteRemainder] =
        dynamic_cast<DocumentSourceInternalUnpackBucket*>(unpack.get())
            ->extractProjectForPushDown(project.get());

    ASSERT_TRUE(extractedProj.isEmpty());

    ASSERT_FALSE(deleteRemainder);
    std::vector<Value> serializedArray;
    project->serializeToArray(serializedArray);
    ASSERT_BSONOBJ_EQ(fromjson("{$project: {meta: false, _id: true}}"),
                      serializedArray[0].getDocument().toBson());
}

TEST_F(InternalUnpackBucketExtractProjectForPushdownTest, ExtractsEntireProjectOnMetaField) {
    auto unpack = DocumentSourceInternalUnpackBucket::createFromBsonInternal(
        fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'foo', metaField: 'myMeta', "
                 "bucketMaxSpanSeconds: 3600}}")
            .firstElement(),
        getExpCtx());
    auto project = DocumentSourceProject::createFromBson(
        fromjson("{$project: {myMeta: 0}}").firstElement(), getExpCtx());

    auto [extractedProj, deleteRemainder] =
        dynamic_cast<DocumentSourceInternalUnpackBucket*>(unpack.get())
            ->extractProjectForPushDown(project.get());

    ASSERT_BSONOBJ_EQ(fromjson("{meta: false}"), extractedProj);
    ASSERT_TRUE(deleteRemainder);
}

TEST_F(InternalUnpackBucketExtractProjectForPushdownTest, ExtractsEntireProjectOnSubfieldsOfMeta) {
    auto unpack = DocumentSourceInternalUnpackBucket::createFromBsonInternal(
        fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'foo', metaField: 'myMeta', "
                 "bucketMaxSpanSeconds: 3600}}")
            .firstElement(),
        getExpCtx());
    auto project = DocumentSourceProject::createFromBson(
        fromjson("{$project: {myMeta: {a: 0, b: 0}}}").firstElement(), getExpCtx());

    auto [extractedProj, deleteRemainder] =
        dynamic_cast<DocumentSourceInternalUnpackBucket*>(unpack.get())
            ->extractProjectForPushDown(project.get());

    const UnorderedFieldsBSONObjComparator kComparator;
    ASSERT_EQ(kComparator.compare(fromjson("{meta: {a: false, b: false}}"), extractedProj), 0);
    ASSERT_TRUE(deleteRemainder);
}

TEST_F(InternalUnpackBucketExtractProjectForPushdownTest, ExtractsPartOfProjectOnMetaField) {
    auto unpack = DocumentSourceInternalUnpackBucket::createFromBsonInternal(
        fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'foo', metaField: 'myMeta', "
                 "bucketMaxSpanSeconds: 3600}}")
            .firstElement(),
        getExpCtx());
    auto project = DocumentSourceProject::createFromBson(
        fromjson("{$project: {a: 0, 'myMeta.a': 0, b: 0, 'myMeta.b': 0, c: 0}}").firstElement(),
        getExpCtx());

    auto [extractedProj, deleteRemainder] =
        dynamic_cast<DocumentSourceInternalUnpackBucket*>(unpack.get())
            ->extractProjectForPushDown(project.get());

    const UnorderedFieldsBSONObjComparator kComparator;
    ASSERT_EQ(kComparator.compare(fromjson("{meta: {a: false, b: false}}"), extractedProj), 0);

    ASSERT_FALSE(deleteRemainder);
    std::vector<Value> serializedArray;
    project->serializeToArray(serializedArray);
    ASSERT_EQ(kComparator.compare(fromjson("{$project: {_id: true, a: false, b: false, c: false}}"),
                                  serializedArray[0].getDocument().toBson()),
              0);
}
}  // namespace
}  // namespace mongo
