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

#include <deque>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/window_function/partition_iterator.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/framework.h"

namespace mongo {
namespace {

#define ASSERT_ADVANCE_RESULT(expected, received) ASSERT_EQ((int)expected, (int)received)

class PartitionIteratorTest : public AggregationContextFixture {
public:
    auto makeDefaultAccessor(
        boost::intrusive_ptr<DocumentSourceMock> mock,
        boost::optional<boost::intrusive_ptr<Expression>> partExpr = boost::none) {
        if (!_iter)
            _iter = std::make_unique<PartitionIterator>(
                getExpCtx().get(), mock.get(), &_tracker, partExpr, boost::none);
        return PartitionAccessor(_iter.get(), PartitionAccessor::Policy::kDefaultSequential);
    }

    auto makeEndpointAccessor(
        boost::intrusive_ptr<DocumentSourceMock> mock,
        boost::optional<boost::intrusive_ptr<Expression>> partExpr = boost::none) {
        if (!_iter)
            _iter = std::make_unique<PartitionIterator>(
                getExpCtx().get(), mock.get(), &_tracker, partExpr, boost::none);
        return PartitionAccessor(_iter.get(), PartitionAccessor::Policy::kEndpoints);
    }

    auto makeManualAccessor(
        boost::intrusive_ptr<DocumentSourceMock> mock,
        boost::optional<boost::intrusive_ptr<Expression>> partExpr = boost::none) {
        if (!_iter)
            _iter = std::make_unique<PartitionIterator>(
                getExpCtx().get(), mock.get(), &_tracker, partExpr, boost::none);
        return PartitionAccessor(_iter.get(), PartitionAccessor::Policy::kManual);
    }

    auto advance() {
        invariant(_iter);
        return _iter->advance();
    }

protected:
    MemoryUsageTracker _tracker{false, 100 * 1024 * 1024 /* default memory limit */};
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
    auto accessor = makeDefaultAccessor(mock, boost::none);

    // Test that an accessor that attempts to read off the end of the partition returns boost::none
    // instead of tassert'ing.
    ASSERT_FALSE(accessor[-1]);
    ASSERT_FALSE(accessor[2]);
}

DEATH_TEST_F(PartitionIteratorTest, SingleConsumerDefaultPolicy, "Requested expired document") {
    const auto docs = std::deque<DocumentSource::GetNextResult>{
        Document{{"a", 1}}, Document{{"a", 2}}, Document{{"a", 3}}, Document{{"a", 4}}};
    const auto mock = DocumentSourceMock::createForTest(docs, getExpCtx());
    auto accessor = makeDefaultAccessor(mock, boost::none);
    // Access the first document, which marks it as expired.
    ASSERT_DOCUMENT_EQ(docs[0].getDocument(), *accessor[0]);
    // Advance the iterator which frees the first expired document.
    advance();
    // Attempting to access the first doc results in a tripwire assertion.
    ASSERT_THROWS_CODE(accessor[-1], AssertionException, 5643005);
}

DEATH_TEST_F(PartitionIteratorTest, MultipleConsumerDefaultPolicy, "Requested expired document") {
    const auto docs = std::deque<DocumentSource::GetNextResult>{
        Document{{"a", 1}}, Document{{"a", 2}}, Document{{"a", 3}}};
    const auto mock = DocumentSourceMock::createForTest(docs, getExpCtx());
    auto laggingAccessor = makeDefaultAccessor(mock, boost::none);
    auto leadingAccessor = makeDefaultAccessor(mock, boost::none);

    // The lagging accessor is referencing 1 doc behind current, and leading is 1 doc ahead.
    ASSERT_FALSE(laggingAccessor[-1]);
    ASSERT_DOCUMENT_EQ(docs[1].getDocument(), *leadingAccessor[1]);
    advance();

    // At this point, no documents are expired.
    ASSERT_DOCUMENT_EQ(docs[0].getDocument(), *laggingAccessor[-1]);
    ASSERT_DOCUMENT_EQ(docs[2].getDocument(), *leadingAccessor[1]);
    advance();

    ASSERT_DOCUMENT_EQ(docs[1].getDocument(), *laggingAccessor[-1]);
    // The leading accessor has fallen off the right side of the partition.
    ASSERT_FALSE(leadingAccessor[1]);

    // The first document should now be expired.
    ASSERT_THROWS_CODE(laggingAccessor[-2], AssertionException, 5643005);
    ASSERT_THROWS_CODE(leadingAccessor[-2], AssertionException, 5643005);
}

DEATH_TEST_F(PartitionIteratorTest, SingleConsumerEndpointPolicy, "Requested expired document") {
    const auto docs = std::deque<DocumentSource::GetNextResult>{
        Document{{"a", 1}}, Document{{"a", 2}}, Document{{"a", 3}}, Document{{"a", 4}}};
    const auto mock = DocumentSourceMock::createForTest(docs, getExpCtx());
    auto accessor = makeEndpointAccessor(mock, boost::none);
    // Mock a window with documents [1, 2].
    auto windowObj = BSON("window" << BSON("documents" << BSON_ARRAY(1 << 2)));
    auto bounds = WindowBounds::parse(
        windowObj.firstElement(), SortPattern(BSON("a" << 1), getExpCtx()), getExpCtx().get());
    // Retrieving the endpoints triggers the expiration, with the assumption that all documents
    // below the lower bound are not needed.
    auto endpoints = accessor.getEndpoints(bounds);
    // Advance the iterator which frees the first expired document.
    advance();

    // Advancing again does not trigger any expiration since there has not been a subsequent call to
    // getEndpoints().
    advance();
    ASSERT_DOCUMENT_EQ(docs[1].getDocument(), *accessor[-1]);

    endpoints = accessor.getEndpoints(bounds);
    // Now the second document, currently at index 0 in the cache, will be released.
    advance();
    ASSERT_THROWS_CODE(accessor[-1], AssertionException, 5371202);
    ASSERT_THROWS_CODE(accessor[-2], AssertionException, 5371202);
}

DEATH_TEST_F(PartitionIteratorTest, MultipleConsumerEndpointPolicy, "Requested expired document") {
    const auto docs = std::deque<DocumentSource::GetNextResult>{
        Document{{"a", 1}}, Document{{"a", 2}}, Document{{"a", 3}}, Document{{"a", 4}}};
    const auto mock = DocumentSourceMock::createForTest(docs, getExpCtx());

    // Create two endpoint accessors, one at [-1, 0] and another at [0, 1]. Since the first one may
    // access the document at (current - 1), the only expiration that can happen on advance() would
    // be (newCurrent - 2).
    auto lookBehindAccessor = makeEndpointAccessor(mock, boost::none);
    auto lookAheadAccessor = makeEndpointAccessor(mock, boost::none);
    auto windowObj = BSON("window" << BSON("documents" << BSON_ARRAY(-1 << 0)));
    auto negBounds = WindowBounds::parse(
        windowObj.firstElement(), SortPattern(BSON("a" << 1), getExpCtx()), getExpCtx().get());
    windowObj = BSON("window" << BSON("documents" << BSON_ARRAY(0 << 1)));
    auto posBounds = WindowBounds::parse(
        windowObj.firstElement(), SortPattern(BSON("a" << 1), getExpCtx()), getExpCtx().get());

    auto endpoints = lookBehindAccessor.getEndpoints(negBounds);
    ASSERT_DOCUMENT_EQ(docs[0].getDocument(), *lookBehindAccessor[endpoints->first]);
    ASSERT_DOCUMENT_EQ(docs[0].getDocument(), *lookBehindAccessor[endpoints->second]);
    endpoints = lookAheadAccessor.getEndpoints(posBounds);
    ASSERT_DOCUMENT_EQ(docs[0].getDocument(), *lookAheadAccessor[endpoints->first]);
    ASSERT_DOCUMENT_EQ(docs[1].getDocument(), *lookAheadAccessor[endpoints->second]);
    // Advance the iterator which does not free any documents.
    advance();

    endpoints = lookBehindAccessor.getEndpoints(negBounds);
    ASSERT_DOCUMENT_EQ(docs[0].getDocument(), *lookBehindAccessor[endpoints->first]);
    ASSERT_DOCUMENT_EQ(docs[1].getDocument(), *lookBehindAccessor[endpoints->second]);
    endpoints = lookAheadAccessor.getEndpoints(posBounds);
    ASSERT_DOCUMENT_EQ(docs[1].getDocument(), *lookAheadAccessor[endpoints->first]);
    ASSERT_DOCUMENT_EQ(docs[2].getDocument(), *lookAheadAccessor[endpoints->second]);

    // Advance again, the current document is now {a: 3}.
    advance();
    endpoints = lookBehindAccessor.getEndpoints(negBounds);
    ASSERT_DOCUMENT_EQ(docs[1].getDocument(), *lookBehindAccessor[endpoints->first]);
    ASSERT_DOCUMENT_EQ(docs[2].getDocument(), *lookBehindAccessor[endpoints->second]);
    endpoints = lookAheadAccessor.getEndpoints(posBounds);
    ASSERT_DOCUMENT_EQ(docs[2].getDocument(), *lookAheadAccessor[endpoints->first]);
    ASSERT_DOCUMENT_EQ(docs[3].getDocument(), *lookAheadAccessor[endpoints->second]);

    // Since both accessors are done with document 0, the next advance will free it but keep around
    // the other docs.
    advance();
    ASSERT_DOCUMENT_EQ(docs[1].getDocument(), *lookBehindAccessor[-2]);
    ASSERT_THROWS_CODE(lookBehindAccessor[-3], AssertionException, 5643005);
}

DEATH_TEST_F(PartitionIteratorTest,
             SingleConsumerRightEndpointPolicy,
             "Requested expired document") {
    const auto docs = std::deque<DocumentSource::GetNextResult>{
        Document{{"a", 1}}, Document{{"a", 2}}, Document{{"a", 3}}, Document{{"a", 4}}};
    const auto mock = DocumentSourceMock::createForTest(docs, getExpCtx());
    auto partIter =
        PartitionIterator(getExpCtx().get(), mock.get(), &_tracker, boost::none, boost::none);
    auto accessor = PartitionAccessor(&partIter, PartitionAccessor::Policy::kRightEndpoint);
    // Use a window of 'documents: [-2, -1]'.
    auto windowObj = BSON("window" << BSON("documents" << BSON_ARRAY(-2 << -1)));
    auto bounds = WindowBounds::parse(
        windowObj.firstElement(), SortPattern(BSON("a" << 1), getExpCtx()), getExpCtx().get());

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
    ASSERT_THROWS_CODE(accessor[-3], AssertionException, 5643005);
}

DEATH_TEST_F(PartitionIteratorTest, MixedPolicy, "Requested expired document") {
    const auto docs = std::deque<DocumentSource::GetNextResult>{
        Document{{"a", 1}}, Document{{"a", 2}}, Document{{"a", 3}}, Document{{"a", 4}}};
    const auto mock = DocumentSourceMock::createForTest(docs, getExpCtx());
    auto endpointAccessor = makeEndpointAccessor(mock, boost::none);
    auto defaultAccessor = makeDefaultAccessor(mock, boost::none);
    // Mock a window with documents [1, 2].
    auto windowObj = BSON("window" << BSON("documents" << BSON_ARRAY(1 << 2)));
    auto bounds = WindowBounds::parse(
        windowObj.firstElement(), SortPattern(BSON("a" << 1), getExpCtx()), getExpCtx().get());

    // Before advancing, ensure that both accessors expire the first document.
    auto endpoints = endpointAccessor.getEndpoints(bounds);
    ASSERT_DOCUMENT_EQ(docs[1].getDocument(), *endpointAccessor[endpoints->first]);
    ASSERT_DOCUMENT_EQ(docs[2].getDocument(), *endpointAccessor[endpoints->second]);
    ASSERT_DOCUMENT_EQ(docs[1].getDocument(), *defaultAccessor[1]);

    // Advance the iterator which frees the first expired document.
    advance();

    // Advance again to get the iterator to document {a: 3}.
    advance();
    // Adjust the default accessor to refer back to the document {a: 2}.
    ASSERT_DOCUMENT_EQ(docs[1].getDocument(), *defaultAccessor[-1]);
    // Keep the same endpoint accessor, which will only include the last document.
    endpoints = endpointAccessor.getEndpoints(bounds);
    ASSERT_DOCUMENT_EQ(docs[3].getDocument(), *endpointAccessor[endpoints->first]);
    ASSERT_DOCUMENT_EQ(docs[3].getDocument(), *endpointAccessor[endpoints->second]);

    // Since the default accessor has not read {a: 3} yet, it won't be released after another
    // advance.
    advance();
    ASSERT_DOCUMENT_EQ(docs[2].getDocument(), *defaultAccessor[-1]);

    // The iterator is currently at {a: 4}, with {a: 1} and {a: 2} both being released.
    ASSERT_THROWS_CODE(defaultAccessor[-2], AssertionException, 5643005);
}

TEST_F(PartitionIteratorTest, MemoryUsageAccountsForDocumentIteratorCache) {
    std::string largeStr(1024, 'x');
    auto bsonDoc = BSON("a" << largeStr);
    const auto docs =
        std::deque<DocumentSource::GetNextResult>{Document(bsonDoc), Document(bsonDoc)};
    const auto mock = DocumentSourceMock::createForTest(docs, getExpCtx());

    [[maybe_unused]] auto accessor = makeDefaultAccessor(mock, boost::none);
    size_t initialDocSize = docs[0].getDocument().getCurrentApproximateSize();

    // Pull in the first document, and verify the reported size of the iterator is roughly double
    // the size of the document. The size of the iterator is double the size of the document because
    // we greedily fill the cache, so each internal document in memory stores two copies of
    // largeStr.
    ASSERT_DOCUMENT_EQ(*_iter->current(), docs[0].getDocument());
    ASSERT_GT(_iter->getApproximateSize(), initialDocSize * 2);
    ASSERT_LT(_iter->getApproximateSize(), initialDocSize * 2 + 500);

    // Pull in the second document. Both docs remain in the cache so the reported memory should
    // include both.
    advance();
    ASSERT_DOCUMENT_EQ(*_iter->current(), docs[1].getDocument());
    ASSERT_GT(_iter->getApproximateSize(), initialDocSize * 2 * 2);
    ASSERT_LT(_iter->getApproximateSize(), initialDocSize * 2 * 2 + 500);
}

TEST_F(PartitionIteratorTest, MemoryUsageAccountsForArraysInDocumentIteratorCache) {
    std::string largeStr(1024, 'x');
    auto bsonDoc = BSON("arr" << BSON_ARRAY(BSON("subObj" << largeStr)));
    const auto docs =
        std::deque<DocumentSource::GetNextResult>{Document(bsonDoc), Document(bsonDoc)};
    const auto mock = DocumentSourceMock::createForTest(docs, getExpCtx());

    [[maybe_unused]] auto accessor = makeDefaultAccessor(mock, boost::none);
    size_t initialDocSize = docs[0].getDocument().getCurrentApproximateSize();

    // Pull in the first document, and verify the reported size of the iterator is roughly
    // triple the size of the document. The reason for this is that 'largeStr' is cached twice; once
    // for the 'arr' element and once for the nested 'subObj' element.
    ASSERT_DOCUMENT_EQ(*_iter->current(), docs[0].getDocument());
    ASSERT_GT(_iter->getApproximateSize(), initialDocSize * 3);
    ASSERT_LT(_iter->getApproximateSize(), initialDocSize * 3 + 1024);

    // Pull in the second document. Both docs remain in the cache so the reported memory should
    // include both.
    advance();
    ASSERT_DOCUMENT_EQ(*_iter->current(), docs[1].getDocument());
    ASSERT_GT(_iter->getApproximateSize(), (initialDocSize * 3) * 2);
    ASSERT_LT(_iter->getApproximateSize(), (initialDocSize * 3) * 2 + 1024);
}

TEST_F(PartitionIteratorTest, MemoryUsageAccountsForNestedArraysInDocumentIteratorCache) {
    std::string largeStr(1024, 'x');
    auto bsonDoc = BSON("arr" << BSON_ARRAY(BSON_ARRAY(BSON("subObj" << largeStr))));
    const auto docs =
        std::deque<DocumentSource::GetNextResult>{Document(bsonDoc), Document(bsonDoc)};
    const auto mock = DocumentSourceMock::createForTest(docs, getExpCtx());

    [[maybe_unused]] auto accessor = makeDefaultAccessor(mock, boost::none);
    size_t initialDocSize = docs[0].getDocument().getCurrentApproximateSize();

    // Pull in the first document, and verify the reported size of the iterator is roughly
    // triple the size of the document. The reason for this is that 'largeStr' is cached twice; once
    // for the 'arr' element and once for the nested 'subObj' element.
    ASSERT_DOCUMENT_EQ(*_iter->current(), docs[0].getDocument());
    ASSERT_GT(_iter->getApproximateSize(), initialDocSize * 3);
    ASSERT_LT(_iter->getApproximateSize(), initialDocSize * 3 + 1024);

    // Pull in the second document. Both docs remain in the cache so the reported memory should
    // include both.
    advance();
    ASSERT_DOCUMENT_EQ(*_iter->current(), docs[1].getDocument());
    ASSERT_GT(_iter->getApproximateSize(), (initialDocSize * 3) * 2);
    ASSERT_LT(_iter->getApproximateSize(), (initialDocSize * 3) * 2 + 1024);
}

TEST_F(PartitionIteratorTest, MemoryUsageAccountsForNestedObjInDocumentIteratorCache) {
    std::string largeStr(1024, 'x');
    auto bsonDoc = BSON(
        "obj" << BSON("subObj" << BSON("subObjSubObj" << largeStr) << "uncachedSub" << largeStr));
    const auto docs = std::deque<DocumentSource::GetNextResult>{Document(bsonDoc)};
    const auto mock = DocumentSourceMock::createForTest(docs, getExpCtx());

    [[maybe_unused]] auto accessor = makeDefaultAccessor(mock, boost::none);
    size_t initialDocSize = docs[0].getDocument().getCurrentApproximateSize();

    // Pull in the first document, and verify the reported size. TODO SERVER-57011: The approximate
    // size should not double count the nested strings.
    ASSERT_DOCUMENT_EQ(*_iter->current(), docs[0].getDocument());
    ASSERT_GT(_iter->getApproximateSize(), initialDocSize * 3);
    ASSERT_LT(_iter->getApproximateSize(), initialDocSize * 4);
}

TEST_F(PartitionIteratorTest, MemoryUsageAccountsForReleasedDocuments) {
    std::string largeStr(1000, 'x');
    auto bsonDoc = BSON("a" << largeStr);
    const auto docs =
        std::deque<DocumentSource::GetNextResult>{Document(bsonDoc), Document(bsonDoc)};
    const auto mock = DocumentSourceMock::createForTest(docs, getExpCtx());

    auto accessor = makeDefaultAccessor(mock, boost::none);
    size_t initialDocSize = docs[0].getDocument().getCurrentApproximateSize();

    // Pull in the first document, and verify the reported size of the iterator is roughly double
    // the size of the document.
    ASSERT_DOCUMENT_EQ(*accessor[0], docs[0].getDocument());
    ASSERT_GT(_iter->getApproximateSize(), initialDocSize * 2);
    ASSERT_LT(_iter->getApproximateSize(), initialDocSize * 2 + 1024);

    // The accessor will have marked the first document as expired, and thus freed on the next call
    // to advance().
    advance();
    ASSERT_DOCUMENT_EQ(*_iter->current(), docs[1].getDocument());
    ASSERT_GT(_iter->getApproximateSize(), initialDocSize * 2);
    ASSERT_LT(_iter->getApproximateSize(), initialDocSize * 2 + 1024);
}

TEST_F(PartitionIteratorTest, ManualPolicy) {
    const auto docs =
        std::deque<DocumentSource::GetNextResult>{Document{{"key", 1}, {"a", 1}},
                                                  Document{{"key", 2}, {"a", BSONNULL}},
                                                  Document{{"key", 3}, {"a", 3}},
                                                  Document{{"key", 4}, {"a", 8}},
                                                  Document{{"key", 6}, {"a", 3}}};
    const auto mock = DocumentSourceMock::createForTest(docs, getExpCtx());
    auto accessor = makeManualAccessor(mock, boost::none);
    size_t initialDocSize = docs[0].getDocument().getApproximateSize();

    ASSERT_DOCUMENT_EQ(*accessor[0], docs[0].getDocument());
    // The documents in this test are so small we will not see the effects of greedy caching that we
    // can see in the above tests. We can therefore expect our advances to increase the size of our
    // iterator by one doc uniformly each time.
    ASSERT_EQ(_iter->getApproximateSize(), initialDocSize * 1);
    advance();
    // Confirm nothing has been released after advancing.
    ASSERT_EQ(_iter->getApproximateSize(), initialDocSize * 2);
    ASSERT_DOCUMENT_EQ(*_iter->current(), docs[1].getDocument());
    advance();
    // Confirm nothing has been released after advancing.
    ASSERT_EQ(_iter->getApproximateSize(), initialDocSize * 3);
    ASSERT_DOCUMENT_EQ(*_iter->current(), docs[2].getDocument());

    // Expire the third document and everything behind it.
    accessor.manualExpireUpTo(0);
    // Advance the iterator which frees the manually expired documents.
    advance();
    ASSERT_EQ(_iter->getApproximateSize(), initialDocSize);
    ASSERT_DOCUMENT_EQ(*_iter->current(), docs[3].getDocument());
}

}  // namespace
}  // namespace mongo
