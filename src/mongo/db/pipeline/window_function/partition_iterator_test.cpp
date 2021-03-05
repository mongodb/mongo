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

#define ASSERT_ADVANCE_RESULT(expected, received) ASSERT_EQ((int)expected, (int)received)

class PartitionIteratorTest : public AggregationContextFixture {
public:
    auto makeDefaultAccessor(
        boost::intrusive_ptr<DocumentSourceMock> mock,
        boost::optional<boost::intrusive_ptr<Expression>> partExpr = boost::none) {
        _iter = std::make_unique<PartitionIterator>(
            getExpCtx().get(), mock.get(), partExpr, boost::none);
        return PartitionAccessor(_iter.get(), PartitionAccessor::Policy::kDefaultSequential);
    }

    auto advance() {
        invariant(_iter);
        return _iter->advance();
    }

private:
    std::unique_ptr<PartitionIterator> _iter;
};

TEST_F(PartitionIteratorTest, IndexAccessPullsInRequiredDocument) {
    const auto docs = std::deque<DocumentSource::GetNextResult>{
        Document{{"a", 1}}, Document{{"b", 1}}, Document{{"c", 1}}};
    const auto mock = DocumentSourceMock::createForTest(docs, getExpCtx());
    auto partIter = makeDefaultAccessor(mock);
    ASSERT_DOCUMENT_EQ(docs[0].getDocument(), *partIter[0]);
    ASSERT_DOCUMENT_EQ(docs[1].getDocument(), *partIter[1]);
    ASSERT_DOCUMENT_EQ(docs[2].getDocument(), *partIter[2]);
}

TEST_F(PartitionIteratorTest, MultipleConsumer) {
    const auto docs = std::deque<DocumentSource::GetNextResult>{
        Document{{"a", 1}}, Document{{"b", 1}}, Document{{"c", 1}}};
    const auto mock = DocumentSourceMock::createForTest(docs, getExpCtx());
    auto partIter = makeDefaultAccessor(mock);
    ASSERT_DOCUMENT_EQ(docs[0].getDocument(), *partIter[0]);
    ASSERT_DOCUMENT_EQ(docs[1].getDocument(), *partIter[1]);
    ASSERT_DOCUMENT_EQ(docs[2].getDocument(), *partIter[2]);

    // Mock a second consumer that only needs document 0.
    ASSERT_DOCUMENT_EQ(docs[0].getDocument(), *partIter[0]);
}

TEST_F(PartitionIteratorTest, LookaheadOutOfRangeAccessEOF) {
    const auto docs = std::deque<DocumentSource::GetNextResult>{Document{{"key", 1}}};
    const auto mock = DocumentSourceMock::createForTest(docs, getExpCtx());
    auto partIter = makeDefaultAccessor(mock);
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
    auto partIter =
        makeDefaultAccessor(mock, boost::optional<boost::intrusive_ptr<Expression>>(key));
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
    auto partIter =
        makeDefaultAccessor(mock, boost::optional<boost::intrusive_ptr<Expression>>(key));
    ASSERT_DOCUMENT_EQ(docs[0].getDocument(), *partIter[0]);
    ASSERT_DOCUMENT_EQ(docs[1].getDocument(), *partIter[1]);
    ASSERT_FALSE(partIter[2]);
    ASSERT_ADVANCE_RESULT(PartitionIterator::AdvanceResult::kAdvanced, advance());
    ASSERT_DOCUMENT_EQ(docs[1].getDocument(), *partIter[0]);
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
    auto partIter =
        makeDefaultAccessor(mock, boost::optional<boost::intrusive_ptr<Expression>>(key));
    ASSERT_DOCUMENT_EQ(docs[0].getDocument(), *partIter[0]);
    // First advance to the final document in partition with key "1".
    ASSERT_ADVANCE_RESULT(PartitionIterator::AdvanceResult::kAdvanced, advance());
    // Next advance triggers a new partition.
    ASSERT_ADVANCE_RESULT(PartitionIterator::AdvanceResult::kNewPartition, advance());
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
    auto partIter =
        makeDefaultAccessor(mock, boost::optional<boost::intrusive_ptr<Expression>>(key));
    ASSERT_DOCUMENT_EQ(docs[0].getDocument(), *partIter[0]);
    ASSERT_ADVANCE_RESULT(PartitionIterator::AdvanceResult::kEOF, advance());

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
    auto partIter =
        makeDefaultAccessor(mock, boost::optional<boost::intrusive_ptr<Expression>>(key));
    ASSERT_DOCUMENT_EQ(docs[0].getDocument(), *partIter[0]);
    advance();
    ASSERT_DOCUMENT_EQ(docs[1].getDocument(), *partIter[0]);
    advance();
    ASSERT_DOCUMENT_EQ(docs[2].getDocument(), *partIter[0]);
}

TEST_F(PartitionIteratorTest, PartitionWithNullsAndMissingFields) {
    const auto docs = std::deque<DocumentSource::GetNextResult>{
        Document{{"key", BSONNULL}}, Document{}, Document{{"key", BSONNULL}}, Document{}};
    const auto mock = DocumentSourceMock::createForTest(docs, getExpCtx());
    auto key = ExpressionFieldPath::createPathFromString(
        getExpCtx().get(), "key", getExpCtx()->variablesParseState);
    auto partIter =
        makeDefaultAccessor(mock, boost::optional<boost::intrusive_ptr<Expression>>(key));

    // All documents are in the same partition.
    ASSERT_DOCUMENT_EQ(docs[0].getDocument(), *partIter[0]);
    ASSERT_DOCUMENT_EQ(docs[1].getDocument(), *partIter[1]);
    ASSERT_DOCUMENT_EQ(docs[2].getDocument(), *partIter[2]);
    ASSERT_DOCUMENT_EQ(docs[3].getDocument(), *partIter[3]);
}

TEST_F(PartitionIteratorTest, PartitionWithNullsAndMissingFieldsCompound) {
    const auto docs = std::deque<DocumentSource::GetNextResult>{
        Document{{"key", Document{{"a", BSONNULL}}}},
        Document{{"key", Document{{"a", BSONNULL}}}},
        Document{{"key", Document{{"a", BSONNULL}, {"b", BSONNULL}}}},
        Document{{"key", Document{{"a", BSONNULL}, {"b", BSONNULL}}}},
        Document{{"key", Document{{"b", BSONNULL}}}}};
    const auto mock = DocumentSourceMock::createForTest(docs, getExpCtx());
    auto key = ExpressionFieldPath::createPathFromString(
        getExpCtx().get(), "key", getExpCtx()->variablesParseState);
    auto partIter =
        makeDefaultAccessor(mock, boost::optional<boost::intrusive_ptr<Expression>>(key));

    // First partition of {a: null}.
    ASSERT_DOCUMENT_EQ(docs[0].getDocument(), *partIter[0]);
    ASSERT_DOCUMENT_EQ(docs[1].getDocument(), *partIter[1]);
    ASSERT_FALSE(partIter[2]);
    ASSERT_FALSE(partIter[-1]);
    ASSERT_ADVANCE_RESULT(PartitionIterator::AdvanceResult::kAdvanced, advance());
    // Second partition of {a: null, b: null}.
    ASSERT_ADVANCE_RESULT(PartitionIterator::AdvanceResult::kNewPartition, advance());
    ASSERT_DOCUMENT_EQ(docs[2].getDocument(), *partIter[0]);
    ASSERT_DOCUMENT_EQ(docs[3].getDocument(), *partIter[1]);
    ASSERT_FALSE(partIter[2]);
    ASSERT_FALSE(partIter[-1]);
    ASSERT_ADVANCE_RESULT(PartitionIterator::AdvanceResult::kAdvanced, advance());
    // Last partition of {b: null}.
    ASSERT_ADVANCE_RESULT(PartitionIterator::AdvanceResult::kNewPartition, advance());
    ASSERT_DOCUMENT_EQ(docs[4].getDocument(), *partIter[0]);
    ASSERT_FALSE(partIter[1]);
}

TEST_F(PartitionIteratorTest, EmptyCollectionReturnsEOF) {
    const auto docs = std::deque<DocumentSource::GetNextResult>{};
    const auto mock = DocumentSourceMock::createForTest(docs, getExpCtx());
    auto key = ExpressionFieldPath::createPathFromString(
        getExpCtx().get(), "key", getExpCtx()->variablesParseState);
    auto partIter =
        makeDefaultAccessor(mock, boost::optional<boost::intrusive_ptr<Expression>>(key));
    ASSERT_FALSE(partIter[0]);
    ASSERT_ADVANCE_RESULT(PartitionIterator::AdvanceResult::kEOF, advance());
}

TEST_F(PartitionIteratorTest, PartitionByArrayErrs) {
    const auto docs = std::deque<DocumentSource::GetNextResult>{Document{{"key", 1}},
                                                                Document{fromjson("{key: [1]}")}};
    const auto mock = DocumentSourceMock::createForTest(docs, getExpCtx());
    auto key = ExpressionFieldPath::createPathFromString(
        getExpCtx().get(), "key", getExpCtx()->variablesParseState);
    auto partIter =
        makeDefaultAccessor(mock, boost::optional<boost::intrusive_ptr<Expression>>(key));
    ASSERT_DOCUMENT_EQ(docs[0].getDocument(), *partIter[0]);
    ASSERT_THROWS_CODE(*partIter[1], AssertionException, ErrorCodes::TypeMismatch);
}

TEST_F(PartitionIteratorTest, CurrentOffsetIsCorrectAfterDocumentsAreAccessed) {
    const auto docs = std::deque<DocumentSource::GetNextResult>{
        Document{{"key", 1}}, Document{{"key", 2}}, Document{{"key", 3}}, Document{{"key", 4}}};
    const auto mock = DocumentSourceMock::createForTest(docs, getExpCtx());
    auto key = ExpressionFieldPath::createPathFromString(
        getExpCtx().get(), "a", getExpCtx()->variablesParseState);
    auto partIter =
        makeDefaultAccessor(mock, boost::optional<boost::intrusive_ptr<Expression>>(key));
    auto doc = partIter[0];
    advance();
    ASSERT_EQ(1, partIter.getCurrentPartitionIndex());
    doc = partIter[0];
    advance();
    ASSERT_EQ(2, partIter.getCurrentPartitionIndex());
    doc = partIter[0];
    advance();
    ASSERT_EQ(3, partIter.getCurrentPartitionIndex());
    doc = partIter[0];
    ASSERT_EQ(3, partIter.getCurrentPartitionIndex());
}

TEST_F(PartitionIteratorTest, OutsideOfPartitionAccessShouldNotTassert) {
    const auto docs =
        std::deque<DocumentSource::GetNextResult>{Document{{"a", 1}}, Document{{"a", 2}}};
    const auto mock = DocumentSourceMock::createForTest(docs, getExpCtx());
    auto partIter = PartitionIterator(getExpCtx().get(), mock.get(), boost::none, boost::none);
    auto accessor = PartitionAccessor(&partIter, PartitionAccessor::Policy::kDefaultSequential);

    // Test that an accessor that attempts to read off the end of the partition returns boost::none
    // instead of tassert'ing.
    ASSERT_FALSE(accessor[-1]);
    ASSERT_FALSE(accessor[2]);
}

DEATH_TEST_F(PartitionIteratorTest,
             SingleConsumerDefaultPolicy,
             "Invalid access of expired document") {
    const auto docs = std::deque<DocumentSource::GetNextResult>{
        Document{{"a", 1}}, Document{{"a", 2}}, Document{{"a", 3}}, Document{{"a", 4}}};
    const auto mock = DocumentSourceMock::createForTest(docs, getExpCtx());
    auto partIter = PartitionIterator(getExpCtx().get(), mock.get(), boost::none, boost::none);
    auto accessor = PartitionAccessor(&partIter, PartitionAccessor::Policy::kDefaultSequential);
    // Access the first document, which marks it as expired.
    ASSERT_DOCUMENT_EQ(docs[0].getDocument(), *accessor[0]);
    // Advance the iterator which frees the first expired document.
    partIter.advance();
    // Attempting to access the first doc results in a tripwire assertion.
    ASSERT_THROWS_CODE(accessor[-1], AssertionException, 5371202);
}

DEATH_TEST_F(PartitionIteratorTest,
             MultipleConsumerDefaultPolicy,
             "Invalid access of expired document") {
    const auto docs = std::deque<DocumentSource::GetNextResult>{
        Document{{"a", 1}}, Document{{"a", 2}}, Document{{"a", 3}}};
    const auto mock = DocumentSourceMock::createForTest(docs, getExpCtx());
    auto partIter = PartitionIterator(getExpCtx().get(), mock.get(), boost::none, boost::none);
    auto laggingAccessor =
        PartitionAccessor(&partIter, PartitionAccessor::Policy::kDefaultSequential);
    auto leadingAccessor =
        PartitionAccessor(&partIter, PartitionAccessor::Policy::kDefaultSequential);

    // The lagging accessor is referencing 1 doc behind current, and leading is 1 doc ahead.
    ASSERT_FALSE(laggingAccessor[-1]);
    ASSERT_DOCUMENT_EQ(docs[1].getDocument(), *leadingAccessor[1]);
    partIter.advance();

    // At this point, no documents are expired.
    ASSERT_DOCUMENT_EQ(docs[0].getDocument(), *laggingAccessor[-1]);
    ASSERT_DOCUMENT_EQ(docs[2].getDocument(), *leadingAccessor[1]);
    partIter.advance();

    ASSERT_DOCUMENT_EQ(docs[1].getDocument(), *laggingAccessor[-1]);
    // The leading accessor has fallen off the right side of the partition.
    ASSERT_FALSE(leadingAccessor[1]);

    // The first document should now be expired.
    ASSERT_THROWS_CODE(laggingAccessor[-2], AssertionException, 5371202);
    ASSERT_THROWS_CODE(leadingAccessor[-2], AssertionException, 5371202);
}

DEATH_TEST_F(PartitionIteratorTest,
             SingleConsumerEndpointPolicy,
             "Invalid access of expired document") {
    const auto docs = std::deque<DocumentSource::GetNextResult>{
        Document{{"a", 1}}, Document{{"a", 2}}, Document{{"a", 3}}, Document{{"a", 4}}};
    const auto mock = DocumentSourceMock::createForTest(docs, getExpCtx());
    auto partIter = PartitionIterator(getExpCtx().get(), mock.get(), boost::none, boost::none);
    auto accessor = PartitionAccessor(&partIter, PartitionAccessor::Policy::kEndpoints);
    // Mock a window with documents [1, 2].
    auto bounds = WindowBounds::parse(BSON("documents" << BSON_ARRAY(1 << 2)),
                                      SortPattern(BSON("a" << 1), getExpCtx()),
                                      getExpCtx().get());
    // Retrieving the endpoints triggers the expiration, with the assumption that all documents
    // below the lower bound are not needed.
    auto endpoints = accessor.getEndpoints(bounds);
    // Advance the iterator which frees the first expired document.
    partIter.advance();

    // Advancing again does not trigger any expiration since there has not been a subsequent call to
    // getEndpoints().
    partIter.advance();
    ASSERT_DOCUMENT_EQ(docs[1].getDocument(), *accessor[-1]);

    endpoints = accessor.getEndpoints(bounds);
    // Now the second document, currently at index 0 in the cache, will be released.
    partIter.advance();
    ASSERT_THROWS_CODE(accessor[-1], AssertionException, 5371202);
    ASSERT_THROWS_CODE(accessor[-2], AssertionException, 5371202);
}

DEATH_TEST_F(PartitionIteratorTest,
             MultipleConsumerEndpointPolicy,
             "Invalid access of expired document") {
    const auto docs = std::deque<DocumentSource::GetNextResult>{
        Document{{"a", 1}}, Document{{"a", 2}}, Document{{"a", 3}}, Document{{"a", 4}}};
    const auto mock = DocumentSourceMock::createForTest(docs, getExpCtx());
    auto partIter = PartitionIterator(getExpCtx().get(), mock.get(), boost::none, boost::none);

    // Create two endpoint accessors, one at [-1, 0] and another at [0, 1]. Since the first one may
    // access the document at (current - 1), the only expiration that can happen on advance() would
    // be (newCurrent - 2).
    auto lookBehindAccessor = PartitionAccessor(&partIter, PartitionAccessor::Policy::kEndpoints);
    auto lookAheadAccessor = PartitionAccessor(&partIter, PartitionAccessor::Policy::kEndpoints);
    auto negBounds = WindowBounds::parse(BSON("documents" << BSON_ARRAY(-1 << 0)),
                                         SortPattern(BSON("a" << 1), getExpCtx()),
                                         getExpCtx().get());
    auto posBounds = WindowBounds::parse(BSON("documents" << BSON_ARRAY(0 << 1)),
                                         SortPattern(BSON("a" << 1), getExpCtx()),
                                         getExpCtx().get());

    auto endpoints = lookBehindAccessor.getEndpoints(negBounds);
    ASSERT_DOCUMENT_EQ(docs[0].getDocument(), *lookBehindAccessor[endpoints->first]);
    ASSERT_DOCUMENT_EQ(docs[0].getDocument(), *lookBehindAccessor[endpoints->second]);
    endpoints = lookAheadAccessor.getEndpoints(posBounds);
    ASSERT_DOCUMENT_EQ(docs[0].getDocument(), *lookAheadAccessor[endpoints->first]);
    ASSERT_DOCUMENT_EQ(docs[1].getDocument(), *lookAheadAccessor[endpoints->second]);
    // Advance the iterator which does not free any documents.
    partIter.advance();

    endpoints = lookBehindAccessor.getEndpoints(negBounds);
    ASSERT_DOCUMENT_EQ(docs[0].getDocument(), *lookBehindAccessor[endpoints->first]);
    ASSERT_DOCUMENT_EQ(docs[1].getDocument(), *lookBehindAccessor[endpoints->second]);
    endpoints = lookAheadAccessor.getEndpoints(posBounds);
    ASSERT_DOCUMENT_EQ(docs[1].getDocument(), *lookAheadAccessor[endpoints->first]);
    ASSERT_DOCUMENT_EQ(docs[2].getDocument(), *lookAheadAccessor[endpoints->second]);

    // Advance again, the current document is now {a: 3}.
    partIter.advance();
    endpoints = lookBehindAccessor.getEndpoints(negBounds);
    ASSERT_DOCUMENT_EQ(docs[1].getDocument(), *lookBehindAccessor[endpoints->first]);
    ASSERT_DOCUMENT_EQ(docs[2].getDocument(), *lookBehindAccessor[endpoints->second]);
    endpoints = lookAheadAccessor.getEndpoints(posBounds);
    ASSERT_DOCUMENT_EQ(docs[2].getDocument(), *lookAheadAccessor[endpoints->first]);
    ASSERT_DOCUMENT_EQ(docs[3].getDocument(), *lookAheadAccessor[endpoints->second]);

    // Since both accessors are done with document 0, the next advance will free it but keep around
    // the other docs.
    partIter.advance();
    ASSERT_DOCUMENT_EQ(docs[1].getDocument(), *lookBehindAccessor[-2]);
    ASSERT_THROWS_CODE(lookBehindAccessor[-3], AssertionException, 5371202);
}

DEATH_TEST_F(PartitionIteratorTest,
             SingleConsumerRightEndpointPolicy,
             "Invalid access of expired document") {
    const auto docs = std::deque<DocumentSource::GetNextResult>{
        Document{{"a", 1}}, Document{{"a", 2}}, Document{{"a", 3}}, Document{{"a", 4}}};
    const auto mock = DocumentSourceMock::createForTest(docs, getExpCtx());
    auto partIter = PartitionIterator(getExpCtx().get(), mock.get(), boost::none, boost::none);
    auto accessor = PartitionAccessor(&partIter, PartitionAccessor::Policy::kRightEndpoint);
    // Use a window of 'documents: [-2, -1]'.
    auto bounds = WindowBounds::parse(BSON("documents" << BSON_ARRAY(-2 << -1)),
                                      SortPattern(BSON("a" << 1), getExpCtx()),
                                      getExpCtx().get());

    // Advance until {a: 3} is the current document.
    partIter.advance();
    partIter.advance();
    ASSERT_DOCUMENT_EQ(docs[2].getDocument(), *accessor[0]);

    // Retrieving the endpoints triggers the expiration: everything below the right endpoint
    // is marked as no longer needed.
    auto endpoints = accessor.getEndpoints(bounds);
    // The endpoints are {a: 1} and {a: 2}. So we will expect {a: 1} to be released.
    ASSERT(endpoints != boost::none);
    ASSERT_DOCUMENT_EQ(docs[0].getDocument(), *accessor[endpoints->first]);
    ASSERT_DOCUMENT_EQ(docs[1].getDocument(), *accessor[endpoints->second]);

    // The no-longer-needed documents are released on the next advance().
    partIter.advance();

    // The current document is now {a: 4}, and {a: 1} has been released.
    ASSERT_DOCUMENT_EQ(docs[3].getDocument(), *accessor[0]);
    ASSERT_DOCUMENT_EQ(docs[2].getDocument(), *accessor[-1]);
    ASSERT_DOCUMENT_EQ(docs[1].getDocument(), *accessor[-2]);
    ASSERT_THROWS_CODE(accessor[-3], AssertionException, 5371202);
}

DEATH_TEST_F(PartitionIteratorTest, MixedPolicy, "Invalid access of expired document") {
    const auto docs = std::deque<DocumentSource::GetNextResult>{
        Document{{"a", 1}}, Document{{"a", 2}}, Document{{"a", 3}}, Document{{"a", 4}}};
    const auto mock = DocumentSourceMock::createForTest(docs, getExpCtx());
    auto partIter = PartitionIterator(getExpCtx().get(), mock.get(), boost::none, boost::none);
    auto endpointAccessor = PartitionAccessor(&partIter, PartitionAccessor::Policy::kEndpoints);
    auto defaultAccessor =
        PartitionAccessor(&partIter, PartitionAccessor::Policy::kDefaultSequential);
    // Mock a window with documents [1, 2].
    auto bounds = WindowBounds::parse(BSON("documents" << BSON_ARRAY(1 << 2)),
                                      SortPattern(BSON("a" << 1), getExpCtx()),
                                      getExpCtx().get());

    // Before advancing, ensure that both accessors expire the first document.
    auto endpoints = endpointAccessor.getEndpoints(bounds);
    ASSERT_DOCUMENT_EQ(docs[1].getDocument(), *endpointAccessor[endpoints->first]);
    ASSERT_DOCUMENT_EQ(docs[2].getDocument(), *endpointAccessor[endpoints->second]);
    ASSERT_DOCUMENT_EQ(docs[1].getDocument(), *defaultAccessor[1]);

    // Advance the iterator which frees the first expired document.
    partIter.advance();

    // Advance again to get the iterator to document {a: 3}.
    partIter.advance();
    // Adjust the default accessor to refer back to the document {a: 2}.
    ASSERT_DOCUMENT_EQ(docs[1].getDocument(), *defaultAccessor[-1]);
    // Keep the same endpoint accessor, which will only include the last document.
    endpoints = endpointAccessor.getEndpoints(bounds);
    ASSERT_DOCUMENT_EQ(docs[3].getDocument(), *endpointAccessor[endpoints->first]);
    ASSERT_DOCUMENT_EQ(docs[3].getDocument(), *endpointAccessor[endpoints->second]);

    // Since the default accessor has not read {a: 3} yet, it won't be released after another
    // advance.
    partIter.advance();
    ASSERT_DOCUMENT_EQ(docs[2].getDocument(), *defaultAccessor[-1]);

    // The iterator is currently at {a: 4}, with {a: 1} and {a: 2} both being released.
    ASSERT_THROWS_CODE(defaultAccessor[-2], AssertionException, 5371202);
}

}  // namespace
}  // namespace mongo
