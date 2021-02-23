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
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/window_function/partition_iterator.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

// This provides access to getExpCtx(), but we'll use a different name for this test suite.
using PartitionIteratorTest = AggregationContextFixture;

#define ASSERT_ADVANCE_RESULT(expected, received) ASSERT_EQ((int)expected, (int)received)

TEST_F(PartitionIteratorTest, IndexAccessPullsInRequiredDocument) {
    const auto docs = std::deque<DocumentSource::GetNextResult>{
        Document{{"a", 1}}, Document{{"b", 1}}, Document{{"c", 1}}};
    const auto mock = DocumentSourceMock::createForTest(docs, getExpCtx());
    auto partIter = PartitionIterator(getExpCtx().get(), mock.get(), boost::none);
    ASSERT_DOCUMENT_EQ(docs[0].getDocument(), *partIter[0]);
    ASSERT_DOCUMENT_EQ(docs[1].getDocument(), *partIter[1]);
    ASSERT_DOCUMENT_EQ(docs[2].getDocument(), *partIter[2]);
}

TEST_F(PartitionIteratorTest, MultipleConsumer) {
    const auto docs = std::deque<DocumentSource::GetNextResult>{
        Document{{"a", 1}}, Document{{"b", 1}}, Document{{"c", 1}}};
    const auto mock = DocumentSourceMock::createForTest(docs, getExpCtx());
    auto partIter = PartitionIterator(getExpCtx().get(), mock.get(), boost::none);
    ASSERT_DOCUMENT_EQ(docs[0].getDocument(), *partIter[0]);
    ASSERT_DOCUMENT_EQ(docs[1].getDocument(), *partIter[1]);
    ASSERT_DOCUMENT_EQ(docs[2].getDocument(), *partIter[2]);

    // Mock a second consumer that only needs document 0.
    ASSERT_DOCUMENT_EQ(docs[0].getDocument(), *partIter[0]);
}

TEST_F(PartitionIteratorTest, LookaheadOutOfRangeAccessEOF) {
    const auto docs = std::deque<DocumentSource::GetNextResult>{Document{{"key", 1}}};
    const auto mock = DocumentSourceMock::createForTest(docs, getExpCtx());
    auto partIter = PartitionIterator(getExpCtx().get(), mock.get(), boost::none);
    ASSERT_DOCUMENT_EQ(docs[0].getDocument(), *partIter[0]);
    ASSERT_FALSE(partIter[1]);
    ASSERT_FALSE(partIter[-1]);
}

TEST_F(PartitionIteratorTest, LookaheadOutOfRangeAccessNewPartition) {
    const auto docs = std::deque<DocumentSource::GetNextResult>{Document{{"key", 1}, {"a", 1}},
                                                                Document{{"key", 1}, {"a", 2}},
                                                                Document{{"key", 2}, {"a", 3}}};
    const auto mock = DocumentSourceMock::createForTest(docs, getExpCtx());
    auto key = ExpressionFieldPath::createPathFromString(
        getExpCtx().get(), "key", getExpCtx()->variablesParseState);
    auto partIter = PartitionIterator(getExpCtx().get(), mock.get(), *key);
    ASSERT_DOCUMENT_EQ(docs[0].getDocument(), *partIter[0]);
    ASSERT_DOCUMENT_EQ(docs[1].getDocument(), *partIter[1]);
    ASSERT_FALSE(partIter[2]);
    ASSERT_FALSE(partIter[-1]);
}

TEST_F(PartitionIteratorTest, AdvanceMovesCurrent) {
    const auto docs = std::deque<DocumentSource::GetNextResult>{Document{{"key", 1}, {"a", 1}},
                                                                Document{{"key", 1}, {"a", 2}},
                                                                Document{{"key", 2}, {"a", 3}}};
    const auto mock = DocumentSourceMock::createForTest(docs, getExpCtx());
    auto key = ExpressionFieldPath::createPathFromString(
        getExpCtx().get(), "key", getExpCtx()->variablesParseState);
    auto partIter = PartitionIterator(getExpCtx().get(), mock.get(), *key);
    ASSERT_DOCUMENT_EQ(docs[0].getDocument(), *partIter[0]);
    ASSERT_DOCUMENT_EQ(docs[1].getDocument(), *partIter[1]);
    ASSERT_FALSE(partIter[2]);
    ASSERT_ADVANCE_RESULT(PartitionIterator::AdvanceResult::kAdvanced, partIter.advance());
    ASSERT_DOCUMENT_EQ(docs[1].getDocument(), *partIter[0]);
    ASSERT_DOCUMENT_EQ(docs[0].getDocument(), *partIter[-1]);
    ASSERT_FALSE(partIter[1]);
}

TEST_F(PartitionIteratorTest, AdvanceOverPartitionBoundary) {
    const auto docs = std::deque<DocumentSource::GetNextResult>{Document{{"key", 1}, {"a", 1}},
                                                                Document{{"key", 1}, {"a", 2}},
                                                                Document{{"key", 2}, {"a", 3}},
                                                                Document{{"key", 2}, {"a", 4}}};
    const auto mock = DocumentSourceMock::createForTest(docs, getExpCtx());
    auto key = ExpressionFieldPath::createPathFromString(
        getExpCtx().get(), "key", getExpCtx()->variablesParseState);
    auto partIter = PartitionIterator(getExpCtx().get(), mock.get(), *key);
    ASSERT_DOCUMENT_EQ(docs[0].getDocument(), *partIter[0]);
    // First advance to the final document in partition with key "1".
    ASSERT_ADVANCE_RESULT(PartitionIterator::AdvanceResult::kAdvanced, partIter.advance());
    // Next advance triggers a new partition.
    ASSERT_ADVANCE_RESULT(PartitionIterator::AdvanceResult::kNewPartition, partIter.advance());
    ASSERT_DOCUMENT_EQ(docs[2].getDocument(), *partIter[0]);
    ASSERT_DOCUMENT_EQ(docs[3].getDocument(), *partIter[1]);
    ASSERT_FALSE(partIter[2]);
    ASSERT_FALSE(partIter[-1]);
}

TEST_F(PartitionIteratorTest, AdvanceResultsInEof) {
    const auto docs = std::deque<DocumentSource::GetNextResult>{Document{{"key", 1}}};
    const auto mock = DocumentSourceMock::createForTest(docs, getExpCtx());
    auto key = ExpressionFieldPath::createPathFromString(
        getExpCtx().get(), "key", getExpCtx()->variablesParseState);
    auto partIter = PartitionIterator(getExpCtx().get(), mock.get(), *key);
    ASSERT_DOCUMENT_EQ(docs[0].getDocument(), *partIter[0]);
    ASSERT_ADVANCE_RESULT(PartitionIterator::AdvanceResult::kEOF, partIter.advance());

    // Any access is disallowed.
    ASSERT_FALSE(partIter[0]);
    ASSERT_FALSE(partIter[1]);
    ASSERT_FALSE(partIter[-1]);
}

TEST_F(PartitionIteratorTest, CurrentReturnsCorrectDocumentAsIteratorAdvances) {
    const auto docs = std::deque<DocumentSource::GetNextResult>{
        Document{{"key", 1}}, Document{{"key", 2}}, Document{{"key", 3}}};
    const auto mock = DocumentSourceMock::createForTest(docs, getExpCtx());
    auto key = ExpressionFieldPath::createPathFromString(
        getExpCtx().get(), "key", getExpCtx()->variablesParseState);
    auto partIter = PartitionIterator(getExpCtx().get(), mock.get(), *key);
    ASSERT_DOCUMENT_EQ(docs[0].getDocument(), *partIter[0]);
    partIter.advance();
    ASSERT_DOCUMENT_EQ(docs[1].getDocument(), *partIter[0]);
    partIter.advance();
    ASSERT_DOCUMENT_EQ(docs[2].getDocument(), *partIter[0]);
}

TEST_F(PartitionIteratorTest, EmptyCollectionReturnsEOF) {
    const auto docs = std::deque<DocumentSource::GetNextResult>{};
    const auto mock = DocumentSourceMock::createForTest(docs, getExpCtx());
    auto key = ExpressionFieldPath::createPathFromString(
        getExpCtx().get(), "key", getExpCtx()->variablesParseState);
    auto partIter = PartitionIterator(getExpCtx().get(), mock.get(), *key);
    ASSERT_FALSE(partIter[0]);
    ASSERT_ADVANCE_RESULT(PartitionIterator::AdvanceResult::kEOF, partIter.advance());
}

TEST_F(PartitionIteratorTest, PartitionByArrayErrs) {
    const auto docs = std::deque<DocumentSource::GetNextResult>{Document{{"key", 1}},
                                                                Document{fromjson("{key: [1]}")}};
    const auto mock = DocumentSourceMock::createForTest(docs, getExpCtx());
    auto key = ExpressionFieldPath::createPathFromString(
        getExpCtx().get(), "key", getExpCtx()->variablesParseState);
    auto partIter = PartitionIterator(getExpCtx().get(), mock.get(), *key);
    ASSERT_DOCUMENT_EQ(docs[0].getDocument(), *partIter[0]);
    ASSERT_THROWS_CODE(*partIter[1], AssertionException, ErrorCodes::TypeMismatch);
}

TEST_F(PartitionIteratorTest, CurrentOffsetIsCorrectAfterDocumentsAreAccessed) {
    const auto docs = std::deque<DocumentSource::GetNextResult>{
        Document{{"key", 1}}, Document{{"key", 2}}, Document{{"key", 3}}, Document{{"key", 4}}};
    const auto mock = DocumentSourceMock::createForTest(docs, getExpCtx());
    auto key = ExpressionFieldPath::createPathFromString(
        getExpCtx().get(), "a", getExpCtx()->variablesParseState);
    auto partIter = PartitionIterator(getExpCtx().get(), mock.get(), *key);
    ASSERT_EQ(0, partIter.getCurrentOffset());
    auto doc = partIter[0];
    partIter.advance();
    ASSERT_EQ(1, partIter.getCurrentOffset());
    doc = partIter[0];
    partIter.advance();
    ASSERT_EQ(2, partIter.getCurrentOffset());
    doc = partIter[0];
    partIter.advance();
    ASSERT_EQ(3, partIter.getCurrentOffset());
    doc = partIter[0];
    ASSERT_EQ(3, partIter.getCurrentOffset());
}

}  // namespace
}  // namespace mongo
