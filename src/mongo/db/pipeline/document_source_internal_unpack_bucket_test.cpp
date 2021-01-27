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

TEST_F(InternalUnpackBucketStageTest, OptimizeAddsIncludeProjectForGroupDependencies) {
    auto unpackSpecObj = fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'foo'}}");
    auto groupSpecObj = fromjson("{$group: {_id: '$x', f: {$first: '$y'}}}");

    auto pipeline = Pipeline::parse(makeVector(unpackSpecObj, groupSpecObj), getExpCtx());
    ASSERT_EQ(2u, pipeline->getSources().size());

    pipeline->optimizePipeline();
    ASSERT_EQ(3u, pipeline->getSources().size());

    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(3u, serialized.size());

    const UnorderedFieldsBSONObjComparator kComparator;
    ASSERT_BSONOBJ_EQ(unpackSpecObj, serialized[0]);
    ASSERT_EQ(
        kComparator.compare(fromjson("{$project: {_id: false, x: true, y: true}}"), serialized[1]),
        0);
    ASSERT_BSONOBJ_EQ(groupSpecObj, serialized[2]);
}

TEST_F(InternalUnpackBucketStageTest, OptimizeAddsIncludeProjectForProjectDependencies) {
    auto unpackSpecObj = fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'foo'}}");
    auto projectSpecObj = fromjson("{$project: {_id: true, x: {f: '$y'}}}");

    auto pipeline = Pipeline::parse(makeVector(unpackSpecObj, projectSpecObj), getExpCtx());
    ASSERT_EQ(2u, pipeline->getSources().size());

    pipeline->optimizePipeline();
    ASSERT_EQ(3u, pipeline->getSources().size());

    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(3u, serialized.size());

    const UnorderedFieldsBSONObjComparator kComparator;
    ASSERT_BSONOBJ_EQ(unpackSpecObj, serialized[0]);
    ASSERT_EQ(
        kComparator.compare(fromjson("{$project: {_id: true, x: true, y: true}}"), serialized[1]),
        0);
    ASSERT_BSONOBJ_EQ(projectSpecObj, serialized[2]);
}

TEST_F(InternalUnpackBucketStageTest, OptimizeAddsIncludeProjectWhenInMiddleOfPipeline) {
    auto matchSpecObj = fromjson("{$match: {'meta.source': 'primary'}}");
    auto unpackSpecObj =
        fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'foo', metaField: 'meta'}}");
    auto groupSpecObj = fromjson("{$group: {_id: '$x', f: {$first: '$y'}}}");

    auto pipeline =
        Pipeline::parse(makeVector(matchSpecObj, unpackSpecObj, groupSpecObj), getExpCtx());
    ASSERT_EQ(3u, pipeline->getSources().size());

    pipeline->optimizePipeline();
    ASSERT_EQ(4u, pipeline->getSources().size());

    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(4u, serialized.size());

    const UnorderedFieldsBSONObjComparator kComparator;
    ASSERT_BSONOBJ_EQ(matchSpecObj, serialized[0]);
    ASSERT_BSONOBJ_EQ(unpackSpecObj, serialized[1]);
    ASSERT_EQ(
        kComparator.compare(fromjson("{$project: {_id: false, x: true, y: true}}"), serialized[2]),
        0);
    ASSERT_BSONOBJ_EQ(groupSpecObj, serialized[3]);
}

TEST_F(InternalUnpackBucketStageTest, OptimizeAddsIncludeProjectWhenDependenciesAreDotted) {
    auto unpackSpecObj = fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'foo'}}");
    auto groupSpecObj = fromjson("{$group: {_id: '$x.y', f: {$first: '$a.b'}}}");

    auto pipeline = Pipeline::parse(makeVector(unpackSpecObj, groupSpecObj), getExpCtx());
    ASSERT_EQ(2u, pipeline->getSources().size());

    pipeline->optimizePipeline();
    ASSERT_EQ(3u, pipeline->getSources().size());

    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(3u, serialized.size());

    const UnorderedFieldsBSONObjComparator kComparator;
    ASSERT_BSONOBJ_EQ(unpackSpecObj, serialized[0]);
    ASSERT_EQ(
        kComparator.compare(fromjson("{$project: {_id: false, x: true, a: true}}"), serialized[1]),
        0);
    ASSERT_BSONOBJ_EQ(groupSpecObj, serialized[2]);
}

TEST_F(InternalUnpackBucketStageTest, OptimizeDoesNotAddProjectWhenThereAreNoDependencies) {
    auto unpackSpecObj = fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'foo'}}");
    auto groupSpecObj = fromjson("{$group: {_id: {$const: null}, count: { $sum: {$const: 1 }}}}");

    auto pipeline = Pipeline::parse(makeVector(unpackSpecObj, groupSpecObj), getExpCtx());
    ASSERT_EQ(2u, pipeline->getSources().size());

    pipeline->optimizePipeline();
    ASSERT_EQ(2u, pipeline->getSources().size());

    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());

    ASSERT_BSONOBJ_EQ(unpackSpecObj, serialized[0]);
    ASSERT_BSONOBJ_EQ(groupSpecObj, serialized[1]);
}

TEST_F(InternalUnpackBucketStageTest, OptimizeDoesNotAddProjectWhenSortDependenciesAreNotFinite) {
    auto unpackSpecObj = fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'foo'}}");
    auto sortSpecObj = fromjson("{$sort: {x: 1}}");

    auto pipeline = Pipeline::parse(makeVector(unpackSpecObj, sortSpecObj), getExpCtx());
    ASSERT_EQ(2u, pipeline->getSources().size());

    pipeline->optimizePipeline();
    ASSERT_EQ(2u, pipeline->getSources().size());

    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());

    ASSERT_BSONOBJ_EQ(unpackSpecObj, serialized[0]);
    ASSERT_BSONOBJ_EQ(sortSpecObj, serialized[1]);
}

TEST_F(InternalUnpackBucketStageTest,
       OptimizeDoesNotAddProjectWhenProjectDependenciesAreNotFinite) {
    auto unpackSpecObj = fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'foo'}}");
    auto sortSpecObj = fromjson("{$sort: {x: 1}}");
    auto projectSpecObj = fromjson("{$project: {_id: false, x: false}}");

    auto pipeline =
        Pipeline::parse(makeVector(unpackSpecObj, sortSpecObj, projectSpecObj), getExpCtx());
    ASSERT_EQ(3u, pipeline->getSources().size());

    pipeline->optimizePipeline();
    ASSERT_EQ(3u, pipeline->getSources().size());

    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(3u, serialized.size());

    ASSERT_BSONOBJ_EQ(unpackSpecObj, serialized[0]);
    ASSERT_BSONOBJ_EQ(sortSpecObj, serialized[1]);
    ASSERT_BSONOBJ_EQ(projectSpecObj, serialized[2]);
}

TEST_F(InternalUnpackBucketStageTest, OptimizeDoesNotAddProjectWhenViableInclusionProjectExists) {
    auto unpackSpecObj = fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'foo'}}");
    auto projectSpecObj = fromjson("{$project: {_id: true, x: true}}");

    auto pipeline = Pipeline::parse(makeVector(unpackSpecObj, projectSpecObj), getExpCtx());
    ASSERT_EQ(2u, pipeline->getSources().size());

    pipeline->optimizePipeline();
    ASSERT_EQ(2u, pipeline->getSources().size());

    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());

    ASSERT_BSONOBJ_EQ(unpackSpecObj, serialized[0]);
    ASSERT_BSONOBJ_EQ(projectSpecObj, serialized[1]);
}

TEST_F(InternalUnpackBucketStageTest,
       OptimizeDoesNotAddProjectWhenViableNonBoolInclusionProjectExists) {
    auto unpackSpecObj = fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'foo'}}");
    auto pipeline = Pipeline::parse(
        makeVector(unpackSpecObj, fromjson("{$project: {_id: 1, x: 1.0, y: 1.5}}")), getExpCtx());
    ASSERT_EQ(2u, pipeline->getSources().size());

    pipeline->optimizePipeline();
    ASSERT_EQ(2u, pipeline->getSources().size());

    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());

    const UnorderedFieldsBSONObjComparator kComparator;
    ASSERT_BSONOBJ_EQ(unpackSpecObj, serialized[0]);
    ASSERT_EQ(
        kComparator.compare(fromjson("{$project: {_id: true, x: true, y: true}}"), serialized[1]),
        0);
}

TEST_F(InternalUnpackBucketStageTest, OptimizeDoesNotAddProjectWhenViableExclusionProjectExists) {
    auto unpackSpecObj = fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'foo'}}");
    auto projectSpecObj = fromjson("{$project: {_id: false, x: false}}");

    auto pipeline = Pipeline::parse(makeVector(unpackSpecObj, projectSpecObj), getExpCtx());
    ASSERT_EQ(2u, pipeline->getSources().size());

    pipeline->optimizePipeline();
    ASSERT_EQ(2u, pipeline->getSources().size());

    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());

    ASSERT_BSONOBJ_EQ(unpackSpecObj, serialized[0]);
    ASSERT_BSONOBJ_EQ(projectSpecObj, serialized[1]);
}

TEST_F(InternalUnpackBucketStageTest, OptimizeAddsInclusionProjectInsteadOfViableExclusionProject) {
    auto unpackSpecObj = fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'foo'}}");
    auto projectSpecObj = fromjson("{$project: {_id: false, x: false}}");
    auto sortSpecObj = fromjson("{$sort: {y: 1}}");
    auto groupSpecObj = fromjson("{$group: {_id: '$y', f: {$first: '$z'}}}");

    auto pipeline = Pipeline::parse(
        makeVector(unpackSpecObj, projectSpecObj, sortSpecObj, groupSpecObj), getExpCtx());
    ASSERT_EQ(4u, pipeline->getSources().size());

    pipeline->optimizePipeline();
    ASSERT_EQ(5u, pipeline->getSources().size());

    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(5u, serialized.size());

    const UnorderedFieldsBSONObjComparator kComparator;
    ASSERT_BSONOBJ_EQ(unpackSpecObj, serialized[0]);
    ASSERT_EQ(
        kComparator.compare(fromjson("{$project: {_id: false, y: true, z: true}}"), serialized[1]),
        0);
    ASSERT_BSONOBJ_EQ(projectSpecObj, serialized[2]);
    ASSERT_BSONOBJ_EQ(sortSpecObj, serialized[3]);
    ASSERT_BSONOBJ_EQ(groupSpecObj, serialized[4]);
}
}  // namespace
}  // namespace mongo
