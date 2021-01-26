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
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/util/make_data_structure.h"

namespace mongo {
namespace {

constexpr auto kUserDefinedTimeName = "time"_sd;
constexpr auto kUserDefinedMetaName = "myMeta"_sd;

using InternalUnpackBucketStageTest = AggregationContextFixture;

TEST_F(InternalUnpackBucketStageTest, UnpackBasicIncludeAllMeasurementFields) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$_internalUnpackBucket"
                     << BSON("include" << BSON_ARRAY("_id"
                                                     << "time" << kUserDefinedMetaName << "a"
                                                     << "b")
                                       << DocumentSourceInternalUnpackBucket::kTimeFieldName
                                       << kUserDefinedTimeName
                                       << DocumentSourceInternalUnpackBucket::kMetaFieldName
                                       << kUserDefinedMetaName));
    auto unpack = DocumentSourceInternalUnpackBucket::createFromBson(spec.firstElement(), expCtx);
    // This source will produce two buckets.
    auto source = DocumentSourceMock::createForTest(
        {"{meta: {'m1': 999, 'm2': 9999}, data: {_id: {'0':1, '1':2}, time: {'0':1, '1':2}, "
         "a:{'0':1, '1':2}, b:{'1':1}}}",
         "{meta: {'m1': 9, 'm2': 9, 'm3': 9}, data: {_id: {'0':3, '1':4}, time: {'0':3, '1':4}, "
         "a:{'0':1, '1':2}, b:{'1':1}}}}"},
        expCtx);
    unpack->setSource(source.get());
    // The first result exists and is as expected.
    auto next = unpack->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(),
                       Document(fromjson("{time: 1, myMeta: {m1: 999, m2: 9999}, _id: 1, a: 1}")));

    next = unpack->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(
        next.getDocument(),
        Document(fromjson("{time: 2, myMeta: {m1: 999, m2: 9999}, _id: 2, a: 2, b: 1}")));

    // Second bucket
    next = unpack->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(
        next.getDocument(),
        Document(fromjson("{time: 3, myMeta: {m1: 9, m2: 9, m3: 9}, _id: 3, a: 1}")));

    next = unpack->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(
        next.getDocument(),
        Document(fromjson("{time: 4, myMeta: {m1: 9, m2: 9, m3: 9}, _id: 4, a: 2, b: 1}")));

    next = unpack->getNext();
    ASSERT_TRUE(next.isEOF());
}

TEST_F(InternalUnpackBucketStageTest, UnpackExcludeASingleField) {
    auto expCtx = getExpCtx();
    auto spec = BSON(
        "$_internalUnpackBucket" << BSON(
            "exclude" << BSON_ARRAY("b") << DocumentSourceInternalUnpackBucket::kTimeFieldName
                      << kUserDefinedTimeName << DocumentSourceInternalUnpackBucket::kMetaFieldName
                      << kUserDefinedMetaName));

    auto unpack = DocumentSourceInternalUnpackBucket::createFromBson(spec.firstElement(), expCtx);

    auto source = DocumentSourceMock::createForTest(
        {"{meta: {'m1': 999, 'm2': 9999}, data: {_id: {'0':1, '1':2}, time: {'0':1, '1':2}, "
         "a:{'0':1, '1':2}, b:{'1':1}}}",
         "{meta: {m1: 9, m2: 9, m3: 9}, data: {_id: {'0':3, '1':4}, time: {'0':3, '1':4}, "
         "a:{'0':1, '1':2}, b:{'1':1}}}}"},
        expCtx);
    unpack->setSource(source.get());
    // The first result exists and is as expected.
    auto next = unpack->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(),
                       Document(fromjson("{time: 1, myMeta: {m1: 999, m2: 9999}, _id: 1, a: 1}")));

    next = unpack->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(),
                       Document(fromjson("{time: 2, myMeta: {m1: 999, m2: 9999}, _id: 2, a: 2}")));

    // Second bucket
    next = unpack->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(
        next.getDocument(),
        Document(fromjson("{time: 3, myMeta: {m1: 9, m2: 9, m3: 9}, _id: 3, a: 1}")));

    next = unpack->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(
        next.getDocument(),
        Document(fromjson("{time: 4, myMeta: {m1: 9, m2: 9, m3: 9}, _id: 4, a: 2}")));

    next = unpack->getNext();
    ASSERT_TRUE(next.isEOF());
}

TEST_F(InternalUnpackBucketStageTest, UnpackEmptyInclude) {
    auto expCtx = getExpCtx();
    auto spec =
        BSON("$_internalUnpackBucket"
             << BSON("include" << BSONArray() << DocumentSourceInternalUnpackBucket::kTimeFieldName
                               << kUserDefinedTimeName
                               << DocumentSourceInternalUnpackBucket::kMetaFieldName
                               << kUserDefinedMetaName));
    auto unpack = DocumentSourceInternalUnpackBucket::createFromBson(spec.firstElement(), expCtx);

    auto source = DocumentSourceMock::createForTest(
        {"{meta: {'m1': 999, 'm2': 9999}, data: {_id: {'0':1, '1':2}, time: {'0':1, '1':2}, "
         "a:{'0':1, '1':2}, b:{'1':1}}}",
         "{meta: {m1: 9, m2: 9, m3: 9}, data: {_id: {'0':3, '1':4}, time: {'0':3, '1':4}, "
         "a:{'0':1, '1':2}, b:{'1':1}}}}"},
        expCtx);
    unpack->setSource(source.get());

    // We should produce empty documents, one per measurement in the bucket.
    for (auto idx = 0; idx < 2; ++idx) {
        auto next = unpack->getNext();
        ASSERT_TRUE(next.isAdvanced());
        ASSERT_DOCUMENT_EQ(next.getDocument(), Document(fromjson("{}")));
    }

    for (auto idx = 0; idx < 2; ++idx) {
        auto next = unpack->getNext();
        ASSERT_TRUE(next.isAdvanced());
        ASSERT_DOCUMENT_EQ(next.getDocument(), Document(fromjson("{}")));
    }

    auto next = unpack->getNext();
    ASSERT_TRUE(next.isEOF());
}

TEST_F(InternalUnpackBucketStageTest, UnpackEmptyExclude) {
    auto expCtx = getExpCtx();
    auto spec =
        BSON("$_internalUnpackBucket"
             << BSON("exclude" << BSONArray() << DocumentSourceInternalUnpackBucket::kTimeFieldName
                               << kUserDefinedTimeName
                               << DocumentSourceInternalUnpackBucket::kMetaFieldName
                               << kUserDefinedMetaName));
    auto unpack = DocumentSourceInternalUnpackBucket::createFromBson(spec.firstElement(), expCtx);

    auto source = DocumentSourceMock::createForTest(
        {"{meta: {'m1': 999, 'm2': 9999}, data: {_id: {'0':1, '1':2}, time: {'0':1, '1':2}, "
         "a:{'0':1, '1':2}, b:{'1':1}}}",
         "{meta: {m1: 9, m2: 9, m3: 9}, data: {_id: {'0':3, '1':4}, time: {'0':3, '1':4}, "
         "a:{'0':1, '1':2}, b:{'1':1}}}}"},
        expCtx);
    unpack->setSource(source.get());

    auto next = unpack->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(),
                       Document(fromjson("{time: 1, myMeta: {m1: 999, m2: 9999}, _id: 1, a: 1}")));

    next = unpack->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(
        next.getDocument(),
        Document(fromjson("{time: 2, myMeta: {m1: 999, m2: 9999}, _id: 2, a: 2, b: 1}")));

    // Second bucket
    next = unpack->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(
        next.getDocument(),
        Document(fromjson("{time: 3, myMeta: {m1: 9, m2: 9, m3: 9}, _id: 3, a: 1}")));

    next = unpack->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(
        next.getDocument(),
        Document(fromjson("{time: 4, myMeta: {m1: 9, m2: 9, m3: 9}, _id: 4, a: 2, b: 1}")));

    next = unpack->getNext();
    ASSERT_TRUE(next.isEOF());
}

TEST_F(InternalUnpackBucketStageTest, SparseColumnsWhereOneColumnIsExhaustedBeforeTheOther) {
    auto expCtx = getExpCtx();
    auto spec =
        BSON("$_internalUnpackBucket"
             << BSON("exclude" << BSONArray() << DocumentSourceInternalUnpackBucket::kTimeFieldName
                               << kUserDefinedTimeName
                               << DocumentSourceInternalUnpackBucket::kMetaFieldName
                               << kUserDefinedMetaName));
    auto unpack = DocumentSourceInternalUnpackBucket::createFromBson(spec.firstElement(), expCtx);

    auto source = DocumentSourceMock::createForTest(
        {"{meta: {'m1': 999, 'm2': 9999}, data: {_id: {'0':1, '1':2}, time: {'0':1, '1':2}, "
         "a:{'0':1}, b:{'1':1}}}"},
        expCtx);
    unpack->setSource(source.get());

    auto next = unpack->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(),
                       Document(fromjson("{time: 1, myMeta: {m1: 999, m2: 9999}, _id: 1, a: 1}")));

    next = unpack->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(),
                       Document(fromjson("{time: 2, myMeta: {m1: 999, m2: 9999}, _id: 2, b: 1}")));

    next = unpack->getNext();
    ASSERT_TRUE(next.isEOF());
}

TEST_F(InternalUnpackBucketStageTest, UnpackBasicIncludeWithDollarPrefix) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$_internalUnpackBucket"
                     << BSON("include" << BSON_ARRAY("_id"
                                                     << "time" << kUserDefinedMetaName << "$a"
                                                     << "b")
                                       << DocumentSourceInternalUnpackBucket::kTimeFieldName
                                       << kUserDefinedTimeName
                                       << DocumentSourceInternalUnpackBucket::kMetaFieldName
                                       << kUserDefinedMetaName));
    auto unpack = DocumentSourceInternalUnpackBucket::createFromBson(spec.firstElement(), expCtx);
    // This source will produce two buckets.
    auto source = DocumentSourceMock::createForTest(
        {"{meta: {'m1': 999, 'm2': 9999}, data: {_id: {'0':1, '1':2}, time: {'0':1, '1':2}, "
         "$a:{'0':1, '1':2}, b:{'1':1}}}",
         "{meta: {m1: 9, m2: 9, m3: 9}, data: {_id: {'0':3, '1':4}, time: {'0':3, '1':4}, "
         "$a:{'0':1, '1':2}, b:{'1':1}}}}"},
        expCtx);
    unpack->setSource(source.get());
    // The first result exists and is as expected.
    auto next = unpack->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(),
                       Document(fromjson("{time: 1, myMeta: {m1: 999, m2: 9999}, _id: 1, $a: 1}")));

    next = unpack->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(
        next.getDocument(),
        Document(fromjson("{time: 2, myMeta: {m1: 999, m2: 9999}, _id: 2, $a: 2, b: 1}")));

    // Second bucket
    next = unpack->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(
        next.getDocument(),
        Document(fromjson("{time: 3, myMeta: {m1: 9, m2: 9, m3: 9}, _id: 3, $a: 1}")));

    next = unpack->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(
        next.getDocument(),
        Document(fromjson("{time: 4, myMeta: {m1: 9, m2: 9, m3: 9}, _id: 4, $a: 2, b: 1}")));

    next = unpack->getNext();
    ASSERT_TRUE(next.isEOF());
}

TEST_F(InternalUnpackBucketStageTest, UnpackMetadataOnly) {
    auto expCtx = getExpCtx();
    auto spec =
        BSON("$_internalUnpackBucket"
             << BSON("exclude" << BSONArray() << DocumentSourceInternalUnpackBucket::kTimeFieldName
                               << kUserDefinedTimeName
                               << DocumentSourceInternalUnpackBucket::kMetaFieldName
                               << kUserDefinedMetaName));
    auto unpack = DocumentSourceInternalUnpackBucket::createFromBson(spec.firstElement(), expCtx);

    auto source = DocumentSourceMock::createForTest(
        {"{meta: {'m1': 999, 'm2': 9999}, data: {_id: {'0':1, '1':2}, time: {'0':1, '1':2}}}",
         "{meta: {m1: 9, m2: 9, m3: 9}, data: {_id: {'0':3, '1':4}, time: {'0':3, '1':4}}}"},
        expCtx);
    unpack->setSource(source.get());

    auto next = unpack->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(),
                       Document(fromjson("{time: 1, myMeta: {m1: 999, m2: 9999}, _id: 1}")));

    next = unpack->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(),
                       Document(fromjson("{time: 2, myMeta: {m1: 999, m2: 9999}, _id: 2}")));

    // Second bucket
    next = unpack->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(),
                       Document(fromjson("{time: 3, myMeta: {m1: 9, m2: 9, m3: 9}, _id: 3}")));

    next = unpack->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(),
                       Document(fromjson("{time: 4, myMeta: {m1: 9, m2: 9, m3: 9}, _id: 4}")));

    next = unpack->getNext();
    ASSERT_TRUE(next.isEOF());
}

TEST_F(InternalUnpackBucketStageTest, UnpackWithStrangeTimestampOrdering) {
    auto expCtx = getExpCtx();
    auto spec =
        BSON("$_internalUnpackBucket"
             << BSON("exclude" << BSONArray() << DocumentSourceInternalUnpackBucket::kTimeFieldName
                               << kUserDefinedTimeName
                               << DocumentSourceInternalUnpackBucket::kMetaFieldName
                               << kUserDefinedMetaName));
    auto unpack = DocumentSourceInternalUnpackBucket::createFromBson(spec.firstElement(), expCtx);

    auto source = DocumentSourceMock::createForTest(
        {"{meta: {'m1': 999, 'm2': 9999}, data: {_id: {'1':1, "
         "'0':2, '2': 3}, time: {'1':1, '0': 2, '2': 3}}}",
         "{meta: {'m1': 9, 'm2': 9, 'm3': 9}, data: {_id: {'1':4, "
         "'0':5, '2':6}, time: {'1':4, '0': 5, '2': 6}}}"},
        expCtx);
    unpack->setSource(source.get());

    auto next = unpack->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(),
                       Document(fromjson("{time: 1, myMeta: {m1: 999, m2: 9999}, _id: 1}")));

    next = unpack->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(),
                       Document(fromjson("{time: 2, myMeta: {m1: 999, m2: 9999}, _id: 2}")));

    next = unpack->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(),
                       Document(fromjson("{time: 3, myMeta: {m1: 999, m2: 9999}, _id: 3}")));

    // Second bucket
    next = unpack->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(),
                       Document(fromjson("{time: 4, myMeta: {m1: 9, m2: 9, m3: 9}, _id: 4}")));

    next = unpack->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(),
                       Document(fromjson("{time: 5, myMeta: {m1: 9, m2: 9, m3: 9}, _id: 5}")));

    next = unpack->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(),
                       Document(fromjson("{time: 6, myMeta: {m1: 9, m2: 9, m3: 9}, _id: 6}")));

    next = unpack->getNext();
    ASSERT_TRUE(next.isEOF());
}

TEST_F(InternalUnpackBucketStageTest,
       BucketUnpackerHandlesMissingMetadataWhenMetaFieldUnspecified) {
    auto expCtx = getExpCtx();
    auto spec =
        BSON("$_internalUnpackBucket"
             << BSON("exclude" << BSONArray() << DocumentSourceInternalUnpackBucket::kTimeFieldName
                               << kUserDefinedTimeName));
    auto unpack = DocumentSourceInternalUnpackBucket::createFromBson(spec.firstElement(), expCtx);
    auto source =
        DocumentSourceMock::createForTest({"{data: {_id: {'1':1, "
                                           "'0':2, '2': 3}, time: {'1':1, '0': 2, '2': 3}}}",
                                           "{data: {_id: {'1':4, "
                                           "'0':5, '2':6}, time: {'1':4, '0': 5, '2': 6}}}"},
                                          expCtx);

    unpack->setSource(source.get());

    auto next = unpack->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), Document(fromjson("{time: 1, _id: 1}")));

    next = unpack->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), Document(fromjson("{time: 2, _id: 2}")));

    next = unpack->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), Document(fromjson("{time: 3, _id: 3}")));

    // Second bucket
    next = unpack->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), Document(fromjson("{time: 4, _id: 4}")));

    next = unpack->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), Document(fromjson("{time: 5, _id: 5}")));

    next = unpack->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), Document(fromjson("{time: 6, _id: 6}")));

    next = unpack->getNext();
    ASSERT_TRUE(next.isEOF());
}

TEST_F(InternalUnpackBucketStageTest, BucketUnpackerHandlesExcludedMetadataWhenBucketHasMetadata) {
    auto expCtx = getExpCtx();
    auto spec = BSON("$_internalUnpackBucket"
                     << BSON("exclude" << BSON_ARRAY(kUserDefinedMetaName)
                                       << DocumentSourceInternalUnpackBucket::kTimeFieldName
                                       << kUserDefinedTimeName
                                       << DocumentSourceInternalUnpackBucket::kMetaFieldName
                                       << kUserDefinedMetaName));
    auto unpack = DocumentSourceInternalUnpackBucket::createFromBson(spec.firstElement(), expCtx);
    auto source = DocumentSourceMock::createForTest(
        {"{meta: {'m1': 999, 'm2': 9999}, data: {_id: {'1':1, "
         "'0':2, '2': 3}, time: {'1':1, '0': 2, '2': 3}}}",
         "{meta: {'m1': 9, 'm2': 9, 'm3': 9}, data: {_id: {'1':4, "
         "'0':5, '2':6}, time: {'1':4, '0': 5, '2': 6}}}"},
        expCtx);

    unpack->setSource(source.get());

    auto next = unpack->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), Document(fromjson("{time: 1, _id: 1}")));

    next = unpack->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), Document(fromjson("{time: 2, _id: 2}")));

    next = unpack->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), Document(fromjson("{time: 3, _id: 3}")));

    // Second bucket
    next = unpack->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), Document(fromjson("{time: 4, _id: 4}")));

    next = unpack->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), Document(fromjson("{time: 5, _id: 5}")));

    next = unpack->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), Document(fromjson("{time: 6, _id: 6}")));

    next = unpack->getNext();
    ASSERT_TRUE(next.isEOF());
}

TEST_F(InternalUnpackBucketStageTest, BucketUnpackerThrowsOnUndefinedMetadata) {
    auto expCtx = getExpCtx();
    auto spec =
        BSON("$_internalUnpackBucket"
             << BSON("exclude" << BSONArray() << DocumentSourceInternalUnpackBucket::kTimeFieldName
                               << kUserDefinedTimeName
                               << DocumentSourceInternalUnpackBucket::kMetaFieldName
                               << kUserDefinedMetaName));
    auto unpack = DocumentSourceInternalUnpackBucket::createFromBson(spec.firstElement(), expCtx);

    auto source =
        DocumentSourceMock::createForTest({"{meta: undefined, data: {_id: {'1':1, "
                                           "'0':2, '2': 3}, time: {'1':1, '0': 2, '2': 3}}}"},
                                          expCtx);
    unpack->setSource(source.get());
    ASSERT_THROWS_CODE(unpack->getNext(), AssertionException, 5369600);
}

TEST_F(InternalUnpackBucketStageTest, BucketUnpackerThrowsOnMissingMetadataWhenExpectedInBuckets) {
    auto expCtx = getExpCtx();
    auto spec =
        BSON("$_internalUnpackBucket"
             << BSON("exclude" << BSONArray() << DocumentSourceInternalUnpackBucket::kTimeFieldName
                               << kUserDefinedTimeName
                               << DocumentSourceInternalUnpackBucket::kMetaFieldName
                               << kUserDefinedMetaName));
    auto unpack = DocumentSourceInternalUnpackBucket::createFromBson(spec.firstElement(), expCtx);

    auto source =
        DocumentSourceMock::createForTest({"{data: {_id: {'1':1, "
                                           "'0':2, '2': 3}, time: {'1':1, '0': 2, '2': 3}}}"},
                                          expCtx);
    unpack->setSource(source.get());
    ASSERT_THROWS_CODE(unpack->getNext(), AssertionException, 5369600);
}

TEST_F(InternalUnpackBucketStageTest, BucketUnpackerThrowsWhenMetadataIsPresentUnexpectedly) {
    auto expCtx = getExpCtx();
    auto spec =
        BSON("$_internalUnpackBucket"
             << BSON("exclude" << BSONArray() << DocumentSourceInternalUnpackBucket::kTimeFieldName
                               << kUserDefinedTimeName));
    auto unpack = DocumentSourceInternalUnpackBucket::createFromBson(spec.firstElement(), expCtx);

    auto source =
        DocumentSourceMock::createForTest({"{meta: {'m1': 999, 'm2': 9999}, data: {_id: {'1':1, "
                                           "'0':2, '2': 3}, time: {'1':1, '0': 2, '2': 3}}}",
                                           "{meta: null, data: {_id: {'1':4, "
                                           "'0':5, '2':6}, time: {'1':4, '0': 5, '2': 6}}}"},
                                          expCtx);
    unpack->setSource(source.get());

    ASSERT_THROWS_CODE(unpack->getNext(), AssertionException, 5369601);
}

TEST_F(InternalUnpackBucketStageTest, BucketUnpackerHandlesNullMetadata) {
    auto expCtx = getExpCtx();
    auto spec =
        BSON("$_internalUnpackBucket"
             << BSON("exclude" << BSONArray() << DocumentSourceInternalUnpackBucket::kTimeFieldName
                               << kUserDefinedTimeName
                               << DocumentSourceInternalUnpackBucket::kMetaFieldName
                               << kUserDefinedMetaName));
    auto unpack = DocumentSourceInternalUnpackBucket::createFromBson(spec.firstElement(), expCtx);

    auto source =
        DocumentSourceMock::createForTest({"{meta: {'m1': 999, 'm2': 9999}, data: {_id: {'1':1, "
                                           "'0':2, '2': 3}, time: {'1':1, '0': 2, '2': 3}}}",
                                           "{meta: null, data: {_id: {'1':4, "
                                           "'0':5, '2':6}, time: {'1':4, '0': 5, '2': 6}}}"},
                                          expCtx);
    unpack->setSource(source.get());

    auto next = unpack->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(),
                       Document(fromjson("{time: 1, myMeta: {m1: 999, m2: 9999}, _id: 1}")));

    next = unpack->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(),
                       Document(fromjson("{time: 2, myMeta: {m1: 999, m2: 9999}, _id: 2}")));

    next = unpack->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(),
                       Document(fromjson("{time: 3, myMeta: {m1: 999, m2: 9999}, _id: 3}")));

    // Second bucket
    next = unpack->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), Document(fromjson("{time: 4, _id: 4}")));

    next = unpack->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), Document(fromjson("{time: 5, _id: 5}")));

    next = unpack->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), Document(fromjson("{time: 6, _id: 6}")));

    next = unpack->getNext();
    ASSERT_TRUE(next.isEOF());
}

TEST_F(InternalUnpackBucketStageTest, ThrowsOnEmptyDataValue) {
    auto expCtx = getExpCtx();
    auto spec =
        BSON("$_internalUnpackBucket"
             << BSON("exclude" << BSONArray() << DocumentSourceInternalUnpackBucket::kTimeFieldName
                               << kUserDefinedTimeName
                               << DocumentSourceInternalUnpackBucket::kMetaFieldName
                               << kUserDefinedMetaName));
    auto unpack = DocumentSourceInternalUnpackBucket::createFromBson(spec.firstElement(), expCtx);

    auto source = DocumentSourceMock::createForTest(
        Document{{"_id", 1}, {"meta", Document{{"m1", 999}, {"m2", 9999}}}, {"data", Document{}}},
        expCtx);
    unpack->setSource(source.get());
    ASSERT_THROWS_CODE(unpack->getNext(), AssertionException, 5346509);
}

TEST_F(InternalUnpackBucketStageTest, HandlesEmptyBucket) {
    auto expCtx = getExpCtx();
    auto spec =
        BSON("$_internalUnpackBucket"
             << BSON("exclude" << BSONArray() << DocumentSourceInternalUnpackBucket::kTimeFieldName
                               << kUserDefinedTimeName
                               << DocumentSourceInternalUnpackBucket::kMetaFieldName
                               << kUserDefinedMetaName));
    auto unpack = DocumentSourceInternalUnpackBucket::createFromBson(spec.firstElement(), expCtx);

    auto source = DocumentSourceMock::createForTest(Document{}, expCtx);
    unpack->setSource(source.get());
    ASSERT_THROWS_CODE(unpack->getNext(), AssertionException, 5346510);
}

TEST_F(InternalUnpackBucketStageTest, ParserRejectsNonObjArgment) {
    ASSERT_THROWS_CODE(DocumentSourceInternalUnpackBucket::createFromBson(
                           fromjson("{$_internalUnpackBucket: 1}").firstElement(), getExpCtx()),
                       AssertionException,
                       5346500);
}

TEST_F(InternalUnpackBucketStageTest, ParserRejectsNonArrayInclude) {
    ASSERT_THROWS_CODE(DocumentSourceInternalUnpackBucket::createFromBson(
                           fromjson("{$_internalUnpackBucket: {include: 'not array', timeField: "
                                    "'foo', metaField: 'bar'}}")
                               .firstElement(),
                           getExpCtx()),
                       AssertionException,
                       5346501);
}

TEST_F(InternalUnpackBucketStageTest, ParserRejectsNonArrayExclude) {
    ASSERT_THROWS_CODE(DocumentSourceInternalUnpackBucket::createFromBson(
                           fromjson("{$_internalUnpackBucket: {exclude: 'not array', timeField: "
                                    "'foo', metaField: 'bar'}}")
                               .firstElement(),
                           getExpCtx()),
                       AssertionException,
                       5346501);
}

TEST_F(InternalUnpackBucketStageTest, ParserRejectsNonStringInclude) {
    ASSERT_THROWS_CODE(DocumentSourceInternalUnpackBucket::createFromBson(
                           fromjson("{$_internalUnpackBucket: {include: [999, 1212], timeField: "
                                    "'foo', metaField: 'bar'}}")
                               .firstElement(),
                           getExpCtx()),
                       AssertionException,
                       5346502);
}

TEST_F(InternalUnpackBucketStageTest, ParserRejectsDottedPaths) {
    ASSERT_THROWS_CODE(
        DocumentSourceInternalUnpackBucket::createFromBson(
            fromjson(
                "{$_internalUnpackBucket: {exclude: ['a.b'], timeField: 'foo', metaField: 'bar'}}")
                .firstElement(),
            getExpCtx()),
        AssertionException,
        5346503);
}

TEST_F(InternalUnpackBucketStageTest, ParserRejectsBadIncludeExcludeFieldName) {
    ASSERT_THROWS_CODE(
        DocumentSourceInternalUnpackBucket::createFromBson(
            fromjson("{$_internalUnpackBucket: {TYPO: [], timeField: 'foo', metaField: 'bar'}}")
                .firstElement(),
            getExpCtx()),
        AssertionException,
        5346506);
}

TEST_F(InternalUnpackBucketStageTest, ParserRejectsNonStringTimeField) {
    ASSERT_THROWS_CODE(
        DocumentSourceInternalUnpackBucket::createFromBson(
            fromjson("{$_internalUnpackBucket: {include: [], timeField: 999, metaField: 'bar'}}")
                .firstElement(),
            getExpCtx()),
        AssertionException,
        5346504);
}

TEST_F(InternalUnpackBucketStageTest, ParserRejectsNonStringMetaField) {
    ASSERT_THROWS_CODE(
        DocumentSourceInternalUnpackBucket::createFromBson(
            fromjson("{$_internalUnpackBucket: {include: [], timeField: 'foo', metaField: 999}}")
                .firstElement(),
            getExpCtx()),
        AssertionException,
        5346505);
}

TEST_F(InternalUnpackBucketStageTest, ParserRejectsAdditionalFields) {
    ASSERT_THROWS_CODE(DocumentSourceInternalUnpackBucket::createFromBson(
                           fromjson("{$_internalUnpackBucket: {include: [], timeField: 'foo', "
                                    "metaField: 'bar', extra: 1}}")
                               .firstElement(),
                           getExpCtx()),
                       AssertionException,
                       5346506);
}

TEST_F(InternalUnpackBucketStageTest, ParserRejectsMissingIncludeField) {
    ASSERT_THROWS(DocumentSourceInternalUnpackBucket::createFromBson(
                      fromjson("{$_internalUnpackBucket: {timeField: 'foo', metaField: 'bar'}}")
                          .firstElement(),
                      getExpCtx()),
                  AssertionException);
}

TEST_F(InternalUnpackBucketStageTest, ParserRejectsMissingTimeField) {
    ASSERT_THROWS(
        DocumentSourceInternalUnpackBucket::createFromBson(
            fromjson("{$_internalUnpackBucket: {include: [], metaField: 'bar'}}").firstElement(),
            getExpCtx()),
        AssertionException);
}

TEST_F(InternalUnpackBucketStageTest, ParserRejectsBothIncludeAndExcludeParameters) {
    ASSERT_THROWS_CODE(DocumentSourceInternalUnpackBucket::createFromBson(
                           fromjson("{$_internalUnpackBucket: {include: ['_id', 'a'], exclude: "
                                    "['a'], timeField: 'time', metaField: 'bar'}}")
                               .firstElement(),
                           getExpCtx()),
                       AssertionException,
                       5408000);
}

/**************************** buildProjectToInternalize() tests ****************************/
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

/******************************* internalizeProject() tests *******************************/
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

TEST_F(InternalUnpackBucketStageTest, OptimizeInternalizesAndOptimizesEndOfPipeline) {
    auto sortSpecObj = fromjson("{$sort: {'a': 1}}");
    auto matchSpecObj = fromjson("{$match: {x: {$gt: 1}}}");
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'foo'}}"),
                   fromjson("{$project: {_id: false, a: true, x: true}}"),
                   sortSpecObj,
                   matchSpecObj),
        getExpCtx());
    ASSERT_EQ(4u, pipeline->getSources().size());

    pipeline->optimizePipeline();

    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(3u, serialized.size());
    ASSERT_BSONOBJ_EQ(
        fromjson("{$_internalUnpackBucket: { include: ['a', 'x'], timeField: 'foo'}}"),
        serialized[0]);
    ASSERT_BSONOBJ_EQ(matchSpecObj, serialized[1]);
    ASSERT_BSONOBJ_EQ(sortSpecObj, serialized[2]);
}
}  // namespace
}  // namespace mongo
