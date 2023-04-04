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

#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/timeseries/timeseries_constants.h"

namespace mongo {
namespace {

constexpr auto kUserDefinedTimeName = "time"_sd;
constexpr auto kUserDefinedMetaName = "myMeta"_sd;

using InternalUnpackBucketExecTest = AggregationContextFixture;

TEST_F(InternalUnpackBucketExecTest, UnpackBasicIncludeAllMeasurementFields) {
    auto expCtx = getExpCtx();

    auto spec = BSON(DocumentSourceInternalUnpackBucket::kStageNameInternal << BSON(
                         DocumentSourceInternalUnpackBucket::kInclude
                         << BSON_ARRAY("_id" << kUserDefinedTimeName << kUserDefinedMetaName << "a"
                                             << "b")
                         << timeseries::kTimeFieldName << kUserDefinedTimeName
                         << timeseries::kMetaFieldName << kUserDefinedMetaName
                         << DocumentSourceInternalUnpackBucket::kBucketMaxSpanSeconds << 3600));
    auto unpack =
        DocumentSourceInternalUnpackBucket::createFromBsonInternal(spec.firstElement(), expCtx);
    // This source will produce two buckets.
    auto source = DocumentSourceMock::createForTest(
        {"{control: {'version': 1}, meta: {'m1': 999, 'm2': 9999}, data: {_id: {'0':1, '1':2}, "
         "time: {'0':1, '1':2}, "
         "a:{'0':1, '1':2}, b:{'1':1}}}",
         "{control: {'version': 1}, meta: {'m1': 9, 'm2': 9, 'm3': 9}, data: {_id: {'0':3, '1':4}, "
         "time: {'0':3, '1':4}, "
         "a:{'0':1, '1':2}, b:{'1':1}}}"},
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

TEST_F(InternalUnpackBucketExecTest, UnpackExcludeASingleField) {
    auto expCtx = getExpCtx();
    auto spec = BSON(DocumentSourceInternalUnpackBucket::kStageNameInternal << BSON(
                         DocumentSourceInternalUnpackBucket::kExclude
                         << BSON_ARRAY("b") << timeseries::kTimeFieldName << kUserDefinedTimeName
                         << timeseries::kMetaFieldName << kUserDefinedMetaName
                         << DocumentSourceInternalUnpackBucket::kBucketMaxSpanSeconds << 3600));

    auto unpack =
        DocumentSourceInternalUnpackBucket::createFromBsonInternal(spec.firstElement(), expCtx);

    auto source = DocumentSourceMock::createForTest(
        {"{control: {'version': 1}, meta: {'m1': 999, 'm2': 9999}, data: {_id: {'0':1, '1':2}, "
         "time: {'0':1, '1':2}, "
         "a:{'0':1, '1':2}, b:{'1':1}}}",
         "{control: {'version': 1}, meta: {m1: 9, m2: 9, m3: 9}, data: {_id: {'0':3, '1':4}, time: "
         "{'0':3, '1':4}, "
         "a:{'0':1, '1':2}, b:{'1':1}}}"},
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

TEST_F(InternalUnpackBucketExecTest, UnpackEmptyInclude) {
    auto expCtx = getExpCtx();
    auto spec = BSON(DocumentSourceInternalUnpackBucket::kStageNameInternal
                     << BSON(DocumentSourceInternalUnpackBucket::kInclude
                             << BSONArray() << timeseries::kTimeFieldName << kUserDefinedTimeName
                             << timeseries::kMetaFieldName << kUserDefinedMetaName
                             << DocumentSourceInternalUnpackBucket::kBucketMaxSpanSeconds << 3600));
    auto unpack =
        DocumentSourceInternalUnpackBucket::createFromBsonInternal(spec.firstElement(), expCtx);

    auto source = DocumentSourceMock::createForTest(
        {"{control: {'version': 1}, meta: {'m1': 999, 'm2': 9999}, data: {_id: {'0':1, '1':2}, "
         "time: {'0':1, '1':2}, "
         "a:{'0':1, '1':2}, b:{'1':1}}}",
         "{control: {'version': 1}, meta: {m1: 9, m2: 9, m3: 9}, data: {_id: {'0':3, '1':4}, time: "
         "{'0':3, '1':4}, "
         "a:{'0':1, '1':2}, b:{'1':1}}}"},
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

TEST_F(InternalUnpackBucketExecTest, UnpackEmptyExclude) {
    auto expCtx = getExpCtx();
    auto spec = BSON(DocumentSourceInternalUnpackBucket::kStageNameInternal
                     << BSON(DocumentSourceInternalUnpackBucket::kExclude
                             << BSONArray() << timeseries::kTimeFieldName << kUserDefinedTimeName
                             << timeseries::kMetaFieldName << kUserDefinedMetaName
                             << DocumentSourceInternalUnpackBucket::kBucketMaxSpanSeconds << 3600));
    auto unpack =
        DocumentSourceInternalUnpackBucket::createFromBsonInternal(spec.firstElement(), expCtx);

    auto source = DocumentSourceMock::createForTest(
        {"{control: {'version': 1}, meta: {'m1': 999, 'm2': 9999}, data: {_id: {'0':1, '1':2}, "
         "time: {'0':1, '1':2}, "
         "a:{'0':1, '1':2}, b:{'1':1}}}",
         "{control: {'version': 1}, meta: {m1: 9, m2: 9, m3: 9}, data: {_id: {'0':3, '1':4}, time: "
         "{'0':3, '1':4}, "
         "a:{'0':1, '1':2}, b:{'1':1}}}"},
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

TEST_F(InternalUnpackBucketExecTest, UnpackNeitherIncludeNorExcludeDefaultsToEmptyExclude) {
    auto expCtx = getExpCtx();
    auto spec =
        BSON(DocumentSourceInternalUnpackBucket::kStageNameInternal
             << BSON(timeseries::kTimeFieldName
                     << kUserDefinedTimeName << timeseries::kMetaFieldName << kUserDefinedMetaName
                     << DocumentSourceInternalUnpackBucket::kBucketMaxSpanSeconds << 3600));
    auto unpack =
        DocumentSourceInternalUnpackBucket::createFromBsonInternal(spec.firstElement(), expCtx);

    auto source = DocumentSourceMock::createForTest(
        {
            R"({
    control: {'version': 1},
    meta: {'m1': 999, 'm2': 9999},
    data: {
        _id: {'0':1, '1':2},
        time: {'0':1, '1':2},
        a:{'0':1, '1':2},
        b:{'1':1}
    }
})",
            R"({
    control: {'version': 1},
    meta: {m1: 9, m2: 9, m3: 9},
    data: {
        _id: {'0':3, '1':4},
        time: {'0':3, '1':4},
        a:{'0':1, '1':2},
        b:{'1':1}
    }
})"},
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

TEST_F(InternalUnpackBucketExecTest, SparseColumnsWhereOneColumnIsExhaustedBeforeTheOther) {
    auto expCtx = getExpCtx();
    auto spec = BSON(DocumentSourceInternalUnpackBucket::kStageNameInternal
                     << BSON(DocumentSourceInternalUnpackBucket::kExclude
                             << BSONArray() << timeseries::kTimeFieldName << kUserDefinedTimeName
                             << timeseries::kMetaFieldName << kUserDefinedMetaName
                             << DocumentSourceInternalUnpackBucket::kBucketMaxSpanSeconds << 3600));
    auto unpack =
        DocumentSourceInternalUnpackBucket::createFromBsonInternal(spec.firstElement(), expCtx);

    auto source = DocumentSourceMock::createForTest(
        {"{control: {'version': 1}, meta: {'m1': 999, 'm2': 9999}, data: {_id: {'0':1, '1':2}, "
         "time: {'0':1, '1':2}, "
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

TEST_F(InternalUnpackBucketExecTest, UnpackBasicIncludeWithDollarPrefix) {
    auto expCtx = getExpCtx();

    auto spec = BSON(DocumentSourceInternalUnpackBucket::kStageNameInternal << BSON(
                         DocumentSourceInternalUnpackBucket::kInclude
                         << BSON_ARRAY("_id" << kUserDefinedTimeName << kUserDefinedMetaName << "$a"
                                             << "b")
                         << timeseries::kTimeFieldName << kUserDefinedTimeName
                         << timeseries::kMetaFieldName << kUserDefinedMetaName
                         << DocumentSourceInternalUnpackBucket::kBucketMaxSpanSeconds << 3600));
    auto unpack =
        DocumentSourceInternalUnpackBucket::createFromBsonInternal(spec.firstElement(), expCtx);
    // This source will produce two buckets.
    auto source = DocumentSourceMock::createForTest(
        {"{control: {'version': 1}, meta: {'m1': 999, 'm2': 9999}, data: {_id: {'0':1, '1':2}, "
         "time: {'0':1, '1':2}, "
         "$a:{'0':1, '1':2}, b:{'1':1}}}",
         "{control: {'version': 1}, meta: {m1: 9, m2: 9, m3: 9}, data: {_id: {'0':3, '1':4}, time: "
         "{'0':3, '1':4}, "
         "$a:{'0':1, '1':2}, b:{'1':1}}}"},
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

TEST_F(InternalUnpackBucketExecTest, UnpackMetadataOnly) {
    auto expCtx = getExpCtx();
    auto spec = BSON(DocumentSourceInternalUnpackBucket::kStageNameInternal
                     << BSON(DocumentSourceInternalUnpackBucket::kExclude
                             << BSONArray() << timeseries::kTimeFieldName << kUserDefinedTimeName
                             << timeseries::kMetaFieldName << kUserDefinedMetaName
                             << DocumentSourceInternalUnpackBucket::kBucketMaxSpanSeconds << 3600));
    auto unpack =
        DocumentSourceInternalUnpackBucket::createFromBsonInternal(spec.firstElement(), expCtx);

    auto source = DocumentSourceMock::createForTest(
        {"{control: {'version': 1}, meta: {'m1': 999, 'm2': 9999}, data: {_id: {'0':1, '1':2}, "
         "time: {'0':1, '1':2}}}",
         "{control: {'version': 1}, meta: {m1: 9, m2: 9, m3: 9}, data: {_id: {'0':3, '1':4}, time: "
         "{'0':3, '1':4}}}"},
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

TEST_F(InternalUnpackBucketExecTest, UnpackWithStrangeTimestampOrdering) {
    auto expCtx = getExpCtx();
    auto spec = BSON(DocumentSourceInternalUnpackBucket::kStageNameInternal
                     << BSON(DocumentSourceInternalUnpackBucket::kExclude
                             << BSONArray() << timeseries::kTimeFieldName << kUserDefinedTimeName
                             << timeseries::kMetaFieldName << kUserDefinedMetaName
                             << DocumentSourceInternalUnpackBucket::kBucketMaxSpanSeconds << 3600));
    auto unpack =
        DocumentSourceInternalUnpackBucket::createFromBsonInternal(spec.firstElement(), expCtx);

    auto source = DocumentSourceMock::createForTest(
        {"{control: {'version': 1}, meta: {'m1': 999, 'm2': 9999}, data: {_id: {'1':1, "
         "'0':2, '2': 3}, time: {'1':1, '0': 2, '2': 3}}}",
         "{control: {'version': 1}, meta: {'m1': 9, 'm2': 9, 'm3': 9}, data: {_id: {'1':4, "
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

TEST_F(InternalUnpackBucketExecTest, BucketUnpackerHandlesMissingMetadataWhenMetaFieldUnspecified) {
    auto expCtx = getExpCtx();
    auto spec = BSON(DocumentSourceInternalUnpackBucket::kStageNameInternal
                     << BSON(DocumentSourceInternalUnpackBucket::kExclude
                             << BSONArray() << timeseries::kTimeFieldName << kUserDefinedTimeName
                             << DocumentSourceInternalUnpackBucket::kBucketMaxSpanSeconds << 3600));
    auto unpack =
        DocumentSourceInternalUnpackBucket::createFromBsonInternal(spec.firstElement(), expCtx);
    auto source =
        DocumentSourceMock::createForTest({"{control: {'version': 1}, data: {_id: {'1':1, "
                                           "'0':2, '2': 3}, time: {'1':1, '0': 2, '2': 3}}}",
                                           "{control: {'version': 1}, data: {_id: {'1':4, "
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

TEST_F(InternalUnpackBucketExecTest, BucketUnpackerHandlesExcludedMetadataWhenBucketHasMetadata) {
    auto expCtx = getExpCtx();
    auto spec =
        BSON(DocumentSourceInternalUnpackBucket::kStageNameInternal
             << BSON(DocumentSourceInternalUnpackBucket::kExclude
                     << BSON_ARRAY(kUserDefinedMetaName) << timeseries::kTimeFieldName
                     << kUserDefinedTimeName << timeseries::kMetaFieldName << kUserDefinedMetaName
                     << DocumentSourceInternalUnpackBucket::kBucketMaxSpanSeconds << 3600));
    auto unpack =
        DocumentSourceInternalUnpackBucket::createFromBsonInternal(spec.firstElement(), expCtx);
    auto source = DocumentSourceMock::createForTest(
        {"{control: {'version': 1}, meta: {'m1': 999, 'm2': 9999}, data: {_id: {'1':1, "
         "'0':2, '2': 3}, time: {'1':1, '0': 2, '2': 3}}}",
         "{control: {'version': 1}, meta: {'m1': 9, 'm2': 9, 'm3': 9}, data: {_id: {'1':4, "
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

TEST_F(InternalUnpackBucketExecTest, BucketUnpackerThrowsOnUndefinedMetadata) {
    auto expCtx = getExpCtx();
    auto spec = BSON(DocumentSourceInternalUnpackBucket::kStageNameInternal
                     << BSON(DocumentSourceInternalUnpackBucket::kExclude
                             << BSONArray() << timeseries::kTimeFieldName << kUserDefinedTimeName
                             << timeseries::kMetaFieldName << kUserDefinedMetaName
                             << DocumentSourceInternalUnpackBucket::kBucketMaxSpanSeconds << 3600));
    auto unpack =
        DocumentSourceInternalUnpackBucket::createFromBsonInternal(spec.firstElement(), expCtx);

    auto source = DocumentSourceMock::createForTest(
        {"{control: {'version': 1}, meta: undefined, data: {_id: {'1':1, "
         "'0':2, '2': 3}, time: {'1':1, '0': 2, '2': 3}}}"},
        expCtx);
    unpack->setSource(source.get());
    ASSERT_THROWS_CODE(unpack->getNext(), AssertionException, 5369600);
}

TEST_F(InternalUnpackBucketExecTest, BucketUnpackerThrowsWhenMetadataIsPresentUnexpectedly) {
    auto expCtx = getExpCtx();
    auto spec = BSON(DocumentSourceInternalUnpackBucket::kStageNameInternal
                     << BSON(DocumentSourceInternalUnpackBucket::kExclude
                             << BSONArray() << timeseries::kTimeFieldName << kUserDefinedTimeName
                             << DocumentSourceInternalUnpackBucket::kBucketMaxSpanSeconds << 3600));
    auto unpack =
        DocumentSourceInternalUnpackBucket::createFromBsonInternal(spec.firstElement(), expCtx);

    auto source = DocumentSourceMock::createForTest(
        {"{control: {'version': 1}, meta: {'m1': 999, 'm2': 9999}, data: {_id: {'1':1, "
         "'0':2, '2': 3}, time: {'1':1, '0': 2, '2': 3}}}",
         "{control: {'version': 1}, meta: null, data: {_id: {'1':4, "
         "'0':5, '2':6}, time: {'1':4, '0': 5, '2': 6}}}"},
        expCtx);
    unpack->setSource(source.get());

    ASSERT_THROWS_CODE(unpack->getNext(), AssertionException, 5369601);
}

TEST_F(InternalUnpackBucketExecTest, BucketUnpackerHandlesNullMetadata) {
    auto expCtx = getExpCtx();
    auto spec = BSON(DocumentSourceInternalUnpackBucket::kStageNameInternal
                     << BSON(DocumentSourceInternalUnpackBucket::kExclude
                             << BSONArray() << timeseries::kTimeFieldName << kUserDefinedTimeName
                             << timeseries::kMetaFieldName << kUserDefinedMetaName
                             << DocumentSourceInternalUnpackBucket::kBucketMaxSpanSeconds << 3600));
    auto unpack =
        DocumentSourceInternalUnpackBucket::createFromBsonInternal(spec.firstElement(), expCtx);

    auto source = DocumentSourceMock::createForTest(
        {"{control: {'version': 1}, meta: {'m1': 999, 'm2': 9999}, data: {_id: {'1':1, "
         "'0':2, '2': 3}, time: {'1':1, '0': 2, '2': 3}}}",
         "{control: {'version': 1}, meta: null, data: {_id: {'1':4, "
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
    ASSERT_DOCUMENT_EQ(next.getDocument(), Document(fromjson("{time: 4, myMeta: null, _id: 4}")));

    next = unpack->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), Document(fromjson("{time: 5, myMeta: null, _id: 5}")));

    next = unpack->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), Document(fromjson("{time: 6, myMeta: null, _id: 6}")));

    next = unpack->getNext();
    ASSERT_TRUE(next.isEOF());
}

TEST_F(InternalUnpackBucketExecTest, BucketUnpackerHandlesMissingMetadata) {
    auto expCtx = getExpCtx();
    auto spec = BSON(DocumentSourceInternalUnpackBucket::kStageNameInternal
                     << BSON(DocumentSourceInternalUnpackBucket::kExclude
                             << BSONArray() << timeseries::kTimeFieldName << kUserDefinedTimeName
                             << timeseries::kMetaFieldName << kUserDefinedMetaName
                             << DocumentSourceInternalUnpackBucket::kBucketMaxSpanSeconds << 3600));
    auto unpack =
        DocumentSourceInternalUnpackBucket::createFromBsonInternal(spec.firstElement(), expCtx);

    auto source = DocumentSourceMock::createForTest(
        {
            R"(
{
    control: {'version': 1},
    meta: {
        'm1': 999, 'm2': 9999
    },
    data: {
        _id: {'1':1, '0':2, '2': 3},
        time: {'1':1, '0': 2, '2': 3}
    }
})",
            R"(
{
    control: {'version': 1},
    data: {
        _id: {'1':4, '0':5, '2':6},
        time: {'1':4, '0': 5, '2': 6}
    }
})"},
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

TEST_F(InternalUnpackBucketExecTest, ThrowsOnEmptyDataValue) {
    auto expCtx = getExpCtx();
    auto spec = BSON(DocumentSourceInternalUnpackBucket::kStageNameInternal
                     << BSON(DocumentSourceInternalUnpackBucket::kExclude
                             << BSONArray() << timeseries::kTimeFieldName << kUserDefinedTimeName
                             << timeseries::kMetaFieldName << kUserDefinedMetaName
                             << DocumentSourceInternalUnpackBucket::kBucketMaxSpanSeconds << 3600));
    auto unpack =
        DocumentSourceInternalUnpackBucket::createFromBsonInternal(spec.firstElement(), expCtx);

    auto source = DocumentSourceMock::createForTest(
        Document{{"_id", 1}, {"meta", Document{{"m1", 999}, {"m2", 9999}}}, {"data", Document{}}},
        expCtx);
    unpack->setSource(source.get());
    ASSERT_THROWS_CODE(unpack->getNext(), AssertionException, 5346509);
}

TEST_F(InternalUnpackBucketExecTest, HandlesEmptyBucket) {
    auto expCtx = getExpCtx();
    auto spec = BSON(DocumentSourceInternalUnpackBucket::kStageNameInternal
                     << BSON(DocumentSourceInternalUnpackBucket::kExclude
                             << BSONArray() << timeseries::kTimeFieldName << kUserDefinedTimeName
                             << timeseries::kMetaFieldName << kUserDefinedMetaName
                             << DocumentSourceInternalUnpackBucket::kBucketMaxSpanSeconds << 3600));
    auto unpack =
        DocumentSourceInternalUnpackBucket::createFromBsonInternal(spec.firstElement(), expCtx);

    auto source = DocumentSourceMock::createForTest(Document{}, expCtx);
    unpack->setSource(source.get());
    ASSERT_THROWS_CODE(unpack->getNext(), AssertionException, 5346510);
}

TEST_F(InternalUnpackBucketExecTest, ParserRejectsNonObjArgment) {
    ASSERT_THROWS_CODE(DocumentSourceInternalUnpackBucket::createFromBsonInternal(
                           fromjson("{$_internalUnpackBucket: 1}").firstElement(), getExpCtx()),
                       AssertionException,
                       5346500);
}

TEST_F(InternalUnpackBucketExecTest, ParserRejectsNonArrayInclude) {
    ASSERT_THROWS_CODE(DocumentSourceInternalUnpackBucket::createFromBsonInternal(
                           fromjson("{$_internalUnpackBucket: {include: 'not array', timeField: "
                                    "'foo', metaField: 'bar', bucketMaxSpanSeconds: 3600}}")
                               .firstElement(),
                           getExpCtx()),
                       AssertionException,
                       5346501);
}

TEST_F(InternalUnpackBucketExecTest, ParserRejectsNonArrayExclude) {
    ASSERT_THROWS_CODE(DocumentSourceInternalUnpackBucket::createFromBsonInternal(
                           fromjson("{$_internalUnpackBucket: {exclude: 'not array', timeField: "
                                    "'foo', metaField: 'bar', bucketMaxSpanSeconds: 3600}}")
                               .firstElement(),
                           getExpCtx()),
                       AssertionException,
                       5346501);
}

TEST_F(InternalUnpackBucketExecTest, ParserRejectsNonStringInclude) {
    ASSERT_THROWS_CODE(DocumentSourceInternalUnpackBucket::createFromBsonInternal(
                           fromjson("{$_internalUnpackBucket: {include: [999, 1212], timeField: "
                                    "'foo', metaField: 'bar', bucketMaxSpanSeconds: 3600}}")
                               .firstElement(),
                           getExpCtx()),
                       AssertionException,
                       5346502);
}

TEST_F(InternalUnpackBucketExecTest, ParserRejectsDottedPaths) {
    ASSERT_THROWS_CODE(DocumentSourceInternalUnpackBucket::createFromBsonInternal(
                           fromjson("{$_internalUnpackBucket: {exclude: ['a.b'], timeField: 'foo', "
                                    "metaField: 'bar', bucketMaxSpanSeconds: 3600}}")
                               .firstElement(),
                           getExpCtx()),
                       AssertionException,
                       5346503);
}

TEST_F(InternalUnpackBucketExecTest, ParserRejectsBadIncludeExcludeFieldName) {
    ASSERT_THROWS_CODE(DocumentSourceInternalUnpackBucket::createFromBsonInternal(
                           fromjson("{$_internalUnpackBucket: {TYPO: [], timeField: 'foo', "
                                    "metaField: 'bar', bucketMaxSpanSeconds: 3600}}")
                               .firstElement(),
                           getExpCtx()),
                       AssertionException,
                       5346506);
}

TEST_F(InternalUnpackBucketExecTest, ParserRejectsNonStringTimeField) {
    ASSERT_THROWS_CODE(DocumentSourceInternalUnpackBucket::createFromBsonInternal(
                           fromjson("{$_internalUnpackBucket: {include: [], timeField: 999, "
                                    "metaField: 'bar', bucketMaxSpanSeconds: 3600}}")
                               .firstElement(),
                           getExpCtx()),
                       AssertionException,
                       5346504);
}

TEST_F(InternalUnpackBucketExecTest, ParserRejectsNonStringMetaField) {
    ASSERT_THROWS_CODE(DocumentSourceInternalUnpackBucket::createFromBsonInternal(
                           fromjson("{$_internalUnpackBucket: {include: [], timeField: 'foo', "
                                    "metaField: 999, bucketMaxSpanSeconds: 3600}}")
                               .firstElement(),
                           getExpCtx()),
                       AssertionException,
                       5346505);
}


TEST_F(InternalUnpackBucketExecTest, ParserRejectsDottedMetaField) {
    ASSERT_THROWS_CODE(DocumentSourceInternalUnpackBucket::createFromBsonInternal(
                           fromjson("{$_internalUnpackBucket: {include: [], timeField: 'foo', "
                                    "metaField: 'address.city', bucketMaxSpanSeconds: 3600}}")
                               .firstElement(),
                           getExpCtx()),
                       AssertionException,
                       5545700);
}

TEST_F(InternalUnpackBucketExecTest, ParserRejectsAdditionalFields) {
    ASSERT_THROWS_CODE(DocumentSourceInternalUnpackBucket::createFromBsonInternal(
                           fromjson("{$_internalUnpackBucket: {include: [], timeField: 'foo', "
                                    "metaField: 'bar', bucketMaxSpanSeconds: 3600, extra: 1}}")
                               .firstElement(),
                           getExpCtx()),
                       AssertionException,
                       5346506);
}

TEST_F(InternalUnpackBucketExecTest, ParserRejectsMissingTimeField) {
    ASSERT_THROWS(DocumentSourceInternalUnpackBucket::createFromBsonInternal(
                      fromjson("{$_internalUnpackBucket: {include: [], metaField: 'bar', "
                               "bucketMaxSpanSeconds: 3600}}")
                          .firstElement(),
                      getExpCtx()),
                  AssertionException);
}

TEST_F(InternalUnpackBucketExecTest, ParserRejectsMissingBucketMaxSpanSeconds) {
    ASSERT_THROWS(
        DocumentSourceInternalUnpackBucket::createFromBsonInternal(
            fromjson("{$_internalUnpackBucket: {include: [], metaField: 'bar', timeField: 'foo'}}")
                .firstElement(),
            getExpCtx()),
        AssertionException);
}

TEST_F(InternalUnpackBucketExecTest, ParserRejectsBothIncludeAndExcludeParameters) {
    ASSERT_THROWS_CODE(
        DocumentSourceInternalUnpackBucket::createFromBsonInternal(
            fromjson("{$_internalUnpackBucket: {include: ['_id', 'a'], exclude: "
                     "['a'], timeField: 'time', metaField: 'bar', bucketMaxSpanSeconds: 3600}}")
                .firstElement(),
            getExpCtx()),
        AssertionException,
        5408000);
}

TEST_F(InternalUnpackBucketExecTest, ParserRoundtripsIncludeMeta) {
    auto bson = fromjson(
        "{$_internalUnpackBucket: {include: ['steve', 'meta'], timeField: 'time', metaField: "
        "'meta', bucketMaxSpanSeconds: 3600}}");
    auto array = std::vector<Value>{};
    DocumentSourceInternalUnpackBucket::createFromBsonInternal(bson.firstElement(), getExpCtx())
        ->serializeToArray(array);
    ASSERT_BSONOBJ_EQ(array[0].getDocument().toBson(), bson);
}

TEST_F(InternalUnpackBucketExecTest, ParserRoundtripsComputedMetaProjFieldsInclude) {
    auto bson = fromjson(
        "{$_internalUnpackBucket: {include: [], timeField: 'time', metaField: 'meta', "
        "bucketMaxSpanSeconds: 3600, computedMetaProjFields: ['a', 'b', 'c']}}");
    auto array = std::vector<Value>{};
    DocumentSourceInternalUnpackBucket::createFromBsonInternal(bson.firstElement(), getExpCtx())
        ->serializeToArray(array);
    ASSERT_BSONOBJ_EQ(array[0].getDocument().toBson(), bson);
}

TEST_F(InternalUnpackBucketExecTest, ParserRoundtripsComputedMetaProjFieldsIncludeWithCompute) {
    auto bson = fromjson(
        "{$_internalUnpackBucket: {include: ['a', 'b', 'c'], timeField: 'time', metaField: 'meta', "
        "bucketMaxSpanSeconds: 3600, computedMetaProjFields: ['a', 'b', 'c']}}");
    auto array = std::vector<Value>{};
    DocumentSourceInternalUnpackBucket::createFromBsonInternal(bson.firstElement(), getExpCtx())
        ->serializeToArray(array);
    ASSERT_BSONOBJ_EQ(array[0].getDocument().toBson(), bson);
}

TEST_F(InternalUnpackBucketExecTest, ParserRoundtripsComputedMetaProjFieldsExclude) {
    auto bson = fromjson(
        "{$_internalUnpackBucket: {exclude: [], timeField: 'time', metaField: 'meta', "
        "bucketMaxSpanSeconds: 3600, computedMetaProjFields: ['a']}}");
    auto array = std::vector<Value>{};
    DocumentSourceInternalUnpackBucket::createFromBsonInternal(bson.firstElement(), getExpCtx())
        ->serializeToArray(array);
    ASSERT_BSONOBJ_EQ(array[0].getDocument().toBson(), bson);
}

TEST_F(InternalUnpackBucketExecTest, ParserRoundtripsComputedMetaProjFieldOverridingMeta) {
    auto bson = fromjson(
        "{$_internalUnpackBucket: {exclude: [], timeField: 'time', metaField: 'meta', "
        "bucketMaxSpanSeconds: 3600, computedMetaProjFields: ['meta']}}");
    auto unpackBucket = DocumentSourceInternalUnpackBucket::createFromBsonInternal(
        bson.firstElement(), getExpCtx());
    ASSERT_FALSE(
        static_cast<DocumentSourceInternalUnpackBucket&>(*unpackBucket).includeMetaField());
    auto array = std::vector<Value>{};
    unpackBucket->serializeToArray(array);
    ASSERT_BSONOBJ_EQ(array[0].getDocument().toBson(), bson);
}

std::string redactFieldNameForTest(StringData s) {
    return str::stream() << "HASH<" << s << ">";
}

TEST_F(InternalUnpackBucketExecTest, RedactsCorrectly) {
    auto bson = fromjson(
        "{$_internalUnpackBucket: {include: ['a', 'b', 'c'], timeField: 'time', metaField: 'meta', "
        "bucketMaxSpanSeconds: 3600, computedMetaProjFields: ['a', 'b', 'c']}}");
    auto array = std::vector<Value>{};
    SerializationOptions opts;
    opts.identifierRedactionPolicy = redactFieldNameForTest;
    opts.redactIdentifiers = true;
    opts.replacementForLiteralArgs = "?";
    DocumentSourceInternalUnpackBucket::createFromBsonInternal(bson.firstElement(), getExpCtx())
        ->serializeToArray(array, opts);
    ASSERT_VALUE_EQ_AUTO(  // NOLINT
        "{$_internalUnpackBucket: {include: [\"HASH<a>\", \"HASH<b>\", \"HASH<c>\"], timeField: "
        "\"HASH<time>\", metaField: \"HASH<meta>\", bucketMaxSpanSeconds: \"?\", "
        "computedMetaProjFields: [\"HASH<a>\", \"HASH<b>\", \"HASH<c>\"]}}",
        array[0]);
}
}  // namespace
}  // namespace mongo
