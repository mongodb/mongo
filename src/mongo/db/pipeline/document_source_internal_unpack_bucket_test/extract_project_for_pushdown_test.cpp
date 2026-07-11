// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/bson/unordered_fields_bsonobj_comparator.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"

#include <memory>
#include <vector>


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
