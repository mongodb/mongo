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

#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"
#include "mongo/db/pipeline/document_source_mock.h"

namespace mongo {
namespace {

constexpr auto kUserDefinedTimeName = "time"_sd;
constexpr auto kUserDefinedMetaName = "myMeta"_sd;

using InternalUnpackBucketExecTest = AggregationContextFixture;

TEST_F(InternalUnpackBucketExecTest, UnpackBasicIncludeAllMeasurementFields) {
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
        BSON("$_internalUnpackBucket"
             << BSON(DocumentSourceInternalUnpackBucket::kTimeFieldName
                     << kUserDefinedTimeName << DocumentSourceInternalUnpackBucket::kMetaFieldName
                     << kUserDefinedMetaName));
    auto unpack = DocumentSourceInternalUnpackBucket::createFromBson(spec.firstElement(), expCtx);

    auto source = DocumentSourceMock::createForTest(
        {
            R"({
    meta: {'m1': 999, 'm2': 9999},
    data: {
        _id: {'0':1, '1':2},
        time: {'0':1, '1':2},
        a:{'0':1, '1':2},
        b:{'1':1}
    }
})",
            R"({
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

TEST_F(InternalUnpackBucketExecTest, UnpackBasicIncludeWithDollarPrefix) {
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

TEST_F(InternalUnpackBucketExecTest, UnpackWithStrangeTimestampOrdering) {
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

TEST_F(InternalUnpackBucketExecTest, BucketUnpackerHandlesMissingMetadataWhenMetaFieldUnspecified) {
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

TEST_F(InternalUnpackBucketExecTest, BucketUnpackerHandlesExcludedMetadataWhenBucketHasMetadata) {
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

TEST_F(InternalUnpackBucketExecTest, BucketUnpackerThrowsOnUndefinedMetadata) {
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

TEST_F(InternalUnpackBucketExecTest, BucketUnpackerThrowsWhenMetadataIsPresentUnexpectedly) {
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

TEST_F(InternalUnpackBucketExecTest, BucketUnpackerHandlesNullMetadata) {
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
    auto spec =
        BSON("$_internalUnpackBucket"
             << BSON("exclude" << BSONArray() << DocumentSourceInternalUnpackBucket::kTimeFieldName
                               << kUserDefinedTimeName
                               << DocumentSourceInternalUnpackBucket::kMetaFieldName
                               << kUserDefinedMetaName));
    auto unpack = DocumentSourceInternalUnpackBucket::createFromBson(spec.firstElement(), expCtx);

    auto source = DocumentSourceMock::createForTest(
        {
            R"(
{
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

TEST_F(InternalUnpackBucketExecTest, HandlesEmptyBucket) {
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

TEST_F(InternalUnpackBucketExecTest, ParserRejectsNonObjArgment) {
    ASSERT_THROWS_CODE(DocumentSourceInternalUnpackBucket::createFromBson(
                           fromjson("{$_internalUnpackBucket: 1}").firstElement(), getExpCtx()),
                       AssertionException,
                       5346500);
}

TEST_F(InternalUnpackBucketExecTest, ParserRejectsNonArrayInclude) {
    ASSERT_THROWS_CODE(DocumentSourceInternalUnpackBucket::createFromBson(
                           fromjson("{$_internalUnpackBucket: {include: 'not array', timeField: "
                                    "'foo', metaField: 'bar'}}")
                               .firstElement(),
                           getExpCtx()),
                       AssertionException,
                       5346501);
}

TEST_F(InternalUnpackBucketExecTest, ParserRejectsNonArrayExclude) {
    ASSERT_THROWS_CODE(DocumentSourceInternalUnpackBucket::createFromBson(
                           fromjson("{$_internalUnpackBucket: {exclude: 'not array', timeField: "
                                    "'foo', metaField: 'bar'}}")
                               .firstElement(),
                           getExpCtx()),
                       AssertionException,
                       5346501);
}

TEST_F(InternalUnpackBucketExecTest, ParserRejectsNonStringInclude) {
    ASSERT_THROWS_CODE(DocumentSourceInternalUnpackBucket::createFromBson(
                           fromjson("{$_internalUnpackBucket: {include: [999, 1212], timeField: "
                                    "'foo', metaField: 'bar'}}")
                               .firstElement(),
                           getExpCtx()),
                       AssertionException,
                       5346502);
}

TEST_F(InternalUnpackBucketExecTest, ParserRejectsDottedPaths) {
    ASSERT_THROWS_CODE(
        DocumentSourceInternalUnpackBucket::createFromBson(
            fromjson(
                "{$_internalUnpackBucket: {exclude: ['a.b'], timeField: 'foo', metaField: 'bar'}}")
                .firstElement(),
            getExpCtx()),
        AssertionException,
        5346503);
}

TEST_F(InternalUnpackBucketExecTest, ParserRejectsBadIncludeExcludeFieldName) {
    ASSERT_THROWS_CODE(
        DocumentSourceInternalUnpackBucket::createFromBson(
            fromjson("{$_internalUnpackBucket: {TYPO: [], timeField: 'foo', metaField: 'bar'}}")
                .firstElement(),
            getExpCtx()),
        AssertionException,
        5346506);
}

TEST_F(InternalUnpackBucketExecTest, ParserRejectsNonStringTimeField) {
    ASSERT_THROWS_CODE(
        DocumentSourceInternalUnpackBucket::createFromBson(
            fromjson("{$_internalUnpackBucket: {include: [], timeField: 999, metaField: 'bar'}}")
                .firstElement(),
            getExpCtx()),
        AssertionException,
        5346504);
}

TEST_F(InternalUnpackBucketExecTest, ParserRejectsNonStringMetaField) {
    ASSERT_THROWS_CODE(
        DocumentSourceInternalUnpackBucket::createFromBson(
            fromjson("{$_internalUnpackBucket: {include: [], timeField: 'foo', metaField: 999}}")
                .firstElement(),
            getExpCtx()),
        AssertionException,
        5346505);
}

TEST_F(InternalUnpackBucketExecTest, ParserRejectsAdditionalFields) {
    ASSERT_THROWS_CODE(DocumentSourceInternalUnpackBucket::createFromBson(
                           fromjson("{$_internalUnpackBucket: {include: [], timeField: 'foo', "
                                    "metaField: 'bar', extra: 1}}")
                               .firstElement(),
                           getExpCtx()),
                       AssertionException,
                       5346506);
}

TEST_F(InternalUnpackBucketExecTest, ParserRejectsMissingTimeField) {
    ASSERT_THROWS(
        DocumentSourceInternalUnpackBucket::createFromBson(
            fromjson("{$_internalUnpackBucket: {include: [], metaField: 'bar'}}").firstElement(),
            getExpCtx()),
        AssertionException);
}

TEST_F(InternalUnpackBucketExecTest, ParserRejectsBothIncludeAndExcludeParameters) {
    ASSERT_THROWS_CODE(DocumentSourceInternalUnpackBucket::createFromBson(
                           fromjson("{$_internalUnpackBucket: {include: ['_id', 'a'], exclude: "
                                    "['a'], timeField: 'time', metaField: 'bar'}}")
                               .firstElement(),
                           getExpCtx()),
                       AssertionException,
                       5408000);
}

TEST_F(InternalUnpackBucketExecTest, BucketUnpackerExtractSingleMeasurement) {
    auto expCtx = getExpCtx();

    std::set<std::string> fields{
        "_id", kUserDefinedMetaName.toString(), kUserDefinedTimeName.toString(), "a", "b"};
    auto spec = BucketSpec{
        kUserDefinedTimeName.toString(), kUserDefinedMetaName.toString(), std::move(fields)};
    auto unpacker = BucketUnpacker{std::move(spec), BucketUnpacker::Behavior::kInclude, true, true};

    auto d1 = dateFromISOString("2020-02-17T00:00:00.000Z").getValue();
    auto d2 = dateFromISOString("2020-02-17T01:00:00.000Z").getValue();
    auto d3 = dateFromISOString("2020-02-17T02:00:00.000Z").getValue();
    auto bucket = BSON("meta" << BSON("m1" << 999 << "m2" << 9999) << "data"
                              << BSON("_id" << BSON("0" << 1 << "1" << 2 << "2" << 3) << "time"
                                            << BSON("0" << d1 << "1" << d2 << "2" << d3) << "a"
                                            << BSON("0" << 1 << "1" << 2 << "2" << 3) << "b"
                                            << BSON("1" << 1 << "2" << 2)));

    unpacker.reset(std::move(bucket));

    auto next = unpacker.extractSingleMeasurement(0);
    auto expected = Document{
        {"myMeta", Document{{"m1", 999}, {"m2", 9999}}}, {"_id", 1}, {"time", d1}, {"a", 1}};
    ASSERT_DOCUMENT_EQ(next, expected);

    next = unpacker.extractSingleMeasurement(2);
    expected = Document{{"myMeta", Document{{"m1", 999}, {"m2", 9999}}},
                        {"_id", 3},
                        {"time", d3},
                        {"a", 3},
                        {"b", 2}};
    ASSERT_DOCUMENT_EQ(next, expected);

    next = unpacker.extractSingleMeasurement(1);
    expected = Document{{"myMeta", Document{{"m1", 999}, {"m2", 9999}}},
                        {"_id", 2},
                        {"time", d2},
                        {"a", 2},
                        {"b", 1}};
    ASSERT_DOCUMENT_EQ(next, expected);

    // Can we extract the middle element again?
    next = unpacker.extractSingleMeasurement(1);
    ASSERT_DOCUMENT_EQ(next, expected);
}

TEST_F(InternalUnpackBucketExecTest, BucketUnpackerExtractSingleMeasurementSparse) {
    auto expCtx = getExpCtx();

    std::set<std::string> fields{
        "_id", kUserDefinedMetaName.toString(), kUserDefinedTimeName.toString(), "a", "b"};
    auto spec = BucketSpec{
        kUserDefinedTimeName.toString(), kUserDefinedMetaName.toString(), std::move(fields)};
    auto unpacker = BucketUnpacker{std::move(spec), BucketUnpacker::Behavior::kInclude, true, true};

    auto d1 = dateFromISOString("2020-02-17T00:00:00.000Z").getValue();
    auto d2 = dateFromISOString("2020-02-17T01:00:00.000Z").getValue();
    auto bucket = BSON("meta" << BSON("m1" << 999 << "m2" << 9999) << "data"
                              << BSON("_id" << BSON("0" << 1 << "1" << 2) << "time"
                                            << BSON("0" << d1 << "1" << d2) << "a" << BSON("0" << 1)
                                            << "b" << BSON("1" << 1)));

    unpacker.reset(std::move(bucket));
    auto next = unpacker.extractSingleMeasurement(1);
    auto expected = Document{
        {"myMeta", Document{{"m1", 999}, {"m2", 9999}}}, {"_id", 2}, {"time", d2}, {"b", 1}};
    ASSERT_DOCUMENT_EQ(next, expected);

    // Can we extract the same element again?
    next = unpacker.extractSingleMeasurement(1);
    ASSERT_DOCUMENT_EQ(next, expected);

    next = unpacker.extractSingleMeasurement(0);
    expected = Document{
        {"myMeta", Document{{"m1", 999}, {"m2", 9999}}}, {"_id", 1}, {"time", d1}, {"a", 1}};
    ASSERT_DOCUMENT_EQ(next, expected);

    // Can we extract the same element twice in a row?
    next = unpacker.extractSingleMeasurement(0);
    ASSERT_DOCUMENT_EQ(next, expected);

    next = unpacker.extractSingleMeasurement(0);
    ASSERT_DOCUMENT_EQ(next, expected);
}

class InternalUnpackBucketRandomSampleTest : public AggregationContextFixture {
protected:
    BSONObj makeIncludeAllSpec() {
        return BSON("$_internalUnpackBucket"
                    << BSON("include" << BSON_ARRAY("_id"
                                                    << "time" << kUserDefinedMetaName << "a"
                                                    << "b")
                                      << DocumentSourceInternalUnpackBucket::kTimeFieldName
                                      << kUserDefinedTimeName
                                      << DocumentSourceInternalUnpackBucket::kMetaFieldName
                                      << kUserDefinedMetaName));
    }

    boost::intrusive_ptr<DocumentSource> makeUnpackStage(const BSONObj& spec,
                                                         long long nSample,
                                                         int bucketMaxCount) {
        auto ds =
            DocumentSourceInternalUnpackBucket::createFromBson(spec.firstElement(), getExpCtx());
        auto unpack = dynamic_cast<DocumentSourceInternalUnpackBucket*>(ds.get());
        unpack->setSampleParameters(nSample, bucketMaxCount);
        return unpack;
    }

    boost::intrusive_ptr<DocumentSource> makeInternalUnpackBucketSample(int nSample,
                                                                        int nBuckets,
                                                                        int nMeasurements) {
        auto spec = makeIncludeAllSpec();
        generateBuckets(nBuckets, nMeasurements);
        auto ds =
            DocumentSourceInternalUnpackBucket::createFromBson(spec.firstElement(), getExpCtx());
        auto unpack = dynamic_cast<DocumentSourceInternalUnpackBucket*>(ds.get());
        unpack->setSampleParameters(nSample, 1000);
        return unpack;
    }

    boost::intrusive_ptr<DocumentSource> prepareMock() {
        auto mock = DocumentSourceMock::createForTest(getExpCtx());
        for (auto&& b : _buckets) {
            mock->push_back(DocumentSource::GetNextResult{std::move(b)});
        }
        return mock;
    }

    Document makeBucketPart(int nMeasurements, std::function<Value(int)> gen) {
        auto doc = MutableDocument{};
        for (auto i = 0; i < nMeasurements; ++i) {
            doc.addField(std::to_string(i), gen(i));
        }
        return doc.freeze();
    }

    void generateBuckets(int nBuckets, int nMeasurements) {
        auto& prng = getExpCtx()->opCtx->getClient()->getPrng();
        std::vector<Document> buckets;
        for (auto m = 0; m < nBuckets; m++) {
            auto idDoc = makeBucketPart(nMeasurements, [](int i) { return Value{OID::gen()}; });
            auto timeDoc = makeBucketPart(nMeasurements, [](int i) { return Value{Date_t{}}; });
            auto aCol = makeBucketPart(nMeasurements,
                                       [&](int i) { return Value{prng.nextCanonicalDouble()}; });
            buckets.push_back({Document{
                {"_id", Value{OID::gen()}},
                {"meta", Document{{"m1", m}, {"m2", m + 1}}},
                {"data",
                 Document{{"_id", idDoc}, {"time", std::move(timeDoc)}, {"a", std::move(aCol)}}}}});
        }

        _buckets = std::move(buckets);
    }

private:
    std::vector<Document> _buckets;
};

TEST_F(InternalUnpackBucketRandomSampleTest, SampleHasExpectedStatProperties) {
    auto unpack = makeInternalUnpackBucketSample(100, 1000, 1000);
    auto mock = prepareMock();
    unpack->setSource(mock.get());

    auto next = unpack->getNext();
    ASSERT_TRUE(next.isAdvanced());

    auto avg = 0.0;
    auto nSampled = 0;
    while (next.isAdvanced()) {
        avg += next.getDocument()["a"].getDouble();
        next = unpack->getNext();
        nSampled++;
    }
    avg /= nSampled;
    ASSERT_EQ(nSampled, 100);

    // The average for the uniform distribution on [0, 1) is ~0.5, and the stdev is sqrt(1/12).
    // We will check if the avg is between +/- 2*sqrt(1/12).
    auto stddev = std::sqrt(1.0 / 12.0);
    ASSERT_GT(avg, 0.5 - 2 * stddev);
    ASSERT_LT(avg, 0.5 + 2 * stddev);
}

TEST_F(InternalUnpackBucketRandomSampleTest, SampleIgnoresDuplicates) {
    auto spec = BSON("$_internalUnpackBucket"
                     << BSON("include" << BSON_ARRAY("_id"
                                                     << "time" << kUserDefinedMetaName << "a"
                                                     << "b")
                                       << DocumentSourceInternalUnpackBucket::kTimeFieldName
                                       << kUserDefinedTimeName
                                       << DocumentSourceInternalUnpackBucket::kMetaFieldName
                                       << kUserDefinedMetaName));

    // Make an unpack bucket stage initialized with a sample size of 2 and bucketMaxCount of 1.
    auto unpack = makeUnpackStage(spec, 2, 1);

    // Fill mock with duplicate buckets to simulate random sampling the same buckets over and over
    // again until the 'kMaxAttempts' are reached in 'doGetNext'.
    auto mock = DocumentSourceMock::createForTest(getExpCtx());
    for (auto i = 0; i < 101; ++i) {
        mock->push_back(Document{{"_id", Value{OID::createFromString("000000000000000000000001")}},
                                 {"meta", Document{{"m1", 1}, {"m2", 2}}},
                                 {"data",
                                  Document{{"_id", Document{{"0", 1}}},
                                           {"time", Document{{"0", Date_t::now()}}},
                                           {"a", Document{{"0", 1}}}}}});
    }
    unpack->setSource(mock.get());

    // The sample size is 2 and there's only one unique measurement in the mock. The second
    // 'getNext' call should spin until the it reaches 'kMaxAttempts' of tries and then throw.
    ASSERT_TRUE(unpack->getNext().isAdvanced());
    ASSERT_THROWS_CODE(unpack->getNext(), AssertionException, 5422103);
}

namespace {
/**
 * Manually computes the timestamp object size for n timestamps.
 */
auto expectedTimestampObjSize(int32_t rowKeyOffset, int32_t n) {
    BSONObjBuilder bob;
    for (auto i = 0; i < n; ++i) {
        bob.appendDate(std::to_string(i + rowKeyOffset), Date_t::now());
    }
    return bob.done().objsize();
}
}  //  namespace

TEST_F(InternalUnpackBucketExecTest, ComputeMeasurementCountLowerBoundsAreCorrect) {
    // The last table entry is a sentinel for an upper bound on the interval that covers measurement
    // counts up to 16 MB.
    const auto maxTableEntry = BucketUnpacker::kTimestampObjSizeTable.size() - 1;

    // Test the case when the target size hits a table entry which represents the lower bound of an
    // interval.
    for (size_t index = 0; index < maxTableEntry; ++index) {
        auto interval = BucketUnpacker::kTimestampObjSizeTable[index];
        ASSERT_EQ(interval.first, BucketUnpacker::computeMeasurementCount(interval.second));
    }
}

TEST_F(InternalUnpackBucketExecTest, ComputeMeasurementCountUpperBoundsAreCorrect) {
    const auto maxTableEntry = BucketUnpacker::kTimestampObjSizeTable.size() - 1;

    // The lower bound sizes of each interval in the kTimestampObjSizeTable are hardcoded. Use this
    // fact and walk the table backwards to check the correctness of the S_i'th interval's upper
    // bound by using the lower bound size for the S_i+1 interval and subtracting the BSONObj size
    // containing one timestamp with the appropriate rowKey.
    std::pair<int, int> currentInterval;
    auto currentIntervalSize = 0;
    auto currentIntervalCount = 0;
    auto size = 0;
    for (size_t index = maxTableEntry; index > 0; --index) {
        currentInterval = BucketUnpacker::kTimestampObjSizeTable[index];
        currentIntervalSize = currentInterval.second;
        currentIntervalCount = currentInterval.first;
        auto rowKey = currentIntervalCount - 1;
        size = expectedTimestampObjSize(rowKey, 1);
        // We need to add back the kMinBSONLength since it's subtracted out.
        ASSERT_EQ(currentIntervalCount - 1,
                  BucketUnpacker::computeMeasurementCount(currentIntervalSize - size +
                                                          BSONObj::kMinBSONLength));
    }
}

TEST_F(InternalUnpackBucketExecTest, ComputeMeasurementCountAllPointsInSmallerIntervals) {
    // Test all values for some of the smaller intervals up to 100 measurements.
    for (auto bucketCount = 0; bucketCount < 25; ++bucketCount) {
        auto size = expectedTimestampObjSize(0, bucketCount);
        ASSERT_EQ(bucketCount, BucketUnpacker::computeMeasurementCount(size));
    }
}

TEST_F(InternalUnpackBucketExecTest, ComputeMeasurementCountInLargerIntervals) {
    ASSERT_EQ(2222, BucketUnpacker::computeMeasurementCount(30003));
    ASSERT_EQ(11111, BucketUnpacker::computeMeasurementCount(155560));
    ASSERT_EQ(449998, BucketUnpacker::computeMeasurementCount(7088863));
}
}  // namespace
}  // namespace mongo
