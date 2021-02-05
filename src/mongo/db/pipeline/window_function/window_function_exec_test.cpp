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
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/window_function/partition_iterator.h"
#include "mongo/db/pipeline/window_function/window_bounds.h"
#include "mongo/db/pipeline/window_function/window_function_exec_non_removable.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class WindowFunctionExecNonRemovableTest : public AggregationContextFixture {
public:
    template <class AccumulatorType>
    WindowFunctionExecNonRemovable<AccumulatorState> createForFieldPath(
        std::deque<DocumentSource::GetNextResult> docs,
        const std::string& inputPath,
        WindowBounds::Bound<int> upper) {
        _docSource = DocumentSourceMock::createForTest(std::move(docs), getExpCtx());
        _iter =
            std::make_unique<PartitionIterator>(getExpCtx().get(), _docSource.get(), boost::none);
        auto input = ExpressionFieldPath::parse(
            getExpCtx().get(), inputPath, getExpCtx()->variablesParseState);
        return WindowFunctionExecNonRemovable<AccumulatorState>(
            _iter.get(), std::move(input), AccumulatorType::create(getExpCtx().get()), upper);
    }

    auto advanceIterator() {
        return _iter->advance();
    }

private:
    boost::intrusive_ptr<DocumentSourceMock> _docSource;
    std::unique_ptr<PartitionIterator> _iter;
};

TEST_F(WindowFunctionExecNonRemovableTest, AccumulateOnlyWithoutLookahead) {
    const auto docs = std::deque<DocumentSource::GetNextResult>{
        Document{{"a", 1}}, Document{{"a", 2}}, Document{{"a", 3}}};
    auto mgr = createForFieldPath<AccumulatorSum>(std::move(docs), "$a", 0);
    ASSERT_VALUE_EQ(Value(1), mgr.getNext());
    advanceIterator();
    ASSERT_VALUE_EQ(Value(3), mgr.getNext());
    advanceIterator();
    ASSERT_VALUE_EQ(Value(6), mgr.getNext());
}

TEST_F(WindowFunctionExecNonRemovableTest, AccumulateOnlyWithExplicitCurrentUpperBound) {
    const auto docs = std::deque<DocumentSource::GetNextResult>{
        Document{{"a", 1}}, Document{{"a", 2}}, Document{{"a", 3}}};
    auto mgr = createForFieldPath<AccumulatorSum>(std::move(docs), "$a", WindowBounds::Current{});
    ASSERT_VALUE_EQ(Value(1), mgr.getNext());
    advanceIterator();
    ASSERT_VALUE_EQ(Value(3), mgr.getNext());
    advanceIterator();
    ASSERT_VALUE_EQ(Value(6), mgr.getNext());
}

TEST_F(WindowFunctionExecNonRemovableTest, AccumulateOnlyWithLookahead) {
    const auto docs = std::deque<DocumentSource::GetNextResult>{
        Document{{"a", 1}}, Document{{"a", 2}}, Document{{"a", 3}}};
    auto mgr = createForFieldPath<AccumulatorSum>(std::move(docs), "$a", 1);
    ASSERT_VALUE_EQ(Value(3), mgr.getNext());
    advanceIterator();
    ASSERT_VALUE_EQ(Value(6), mgr.getNext());
    advanceIterator();
    ASSERT_VALUE_EQ(Value(6), mgr.getNext());
}

TEST_F(WindowFunctionExecNonRemovableTest, AccumulateOnlyWithLookBehind) {
    const auto docs = std::deque<DocumentSource::GetNextResult>{
        Document{{"a", 1}}, Document{{"a", 2}}, Document{{"a", 3}}};
    auto mgr = createForFieldPath<AccumulatorSum>(std::move(docs), "$a", -1);
    ASSERT_VALUE_EQ(Value(0), mgr.getNext());
    advanceIterator();
    ASSERT_VALUE_EQ(Value(1), mgr.getNext());
    advanceIterator();
    ASSERT_VALUE_EQ(Value(3), mgr.getNext());
}

TEST_F(WindowFunctionExecNonRemovableTest, AccumulateOnlyWithMultiplePartitions) {
    const auto docs = std::deque<DocumentSource::GetNextResult>{Document{{"a", 1}, {"key", 1}},
                                                                Document{{"a", 2}, {"key", 2}},
                                                                Document{{"a", 3}, {"key", 3}}};
    auto mock = DocumentSourceMock::createForTest(std::move(docs), getExpCtx());
    auto key = ExpressionFieldPath::createPathFromString(
        getExpCtx().get(), "key", getExpCtx()->variablesParseState);
    auto iter = PartitionIterator{getExpCtx().get(), mock.get(), *key};
    auto input =
        ExpressionFieldPath::parse(getExpCtx().get(), "$a", getExpCtx()->variablesParseState);
    auto mgr = WindowFunctionExecNonRemovable<AccumulatorState>(
        &iter, std::move(input), AccumulatorSum::create(getExpCtx().get()), 1);
    ASSERT_VALUE_EQ(Value(1), mgr.getNext());
    iter.advance();
    // Normally the stage would be responsible for detecting a new partition, for this test reset
    // the WindowFunctionExec directly.
    mgr.reset();
    ASSERT_VALUE_EQ(Value(2), mgr.getNext());
    iter.advance();
    mgr.reset();
    ASSERT_VALUE_EQ(Value(3), mgr.getNext());
}

TEST_F(WindowFunctionExecNonRemovableTest, UnboundedNotYetSupported) {
    auto docSource = DocumentSourceMock::createForTest({}, getExpCtx());
    auto iter =
        std::make_unique<PartitionIterator>(getExpCtx().get(), docSource.get(), boost::none);
    auto input =
        ExpressionFieldPath::parse(getExpCtx().get(), "$a", getExpCtx()->variablesParseState);
    ASSERT_THROWS_CODE(
        [&]() {
            auto unused = WindowFunctionExecNonRemovable<AccumulatorState>(
                iter.get(),
                std::move(input),
                AccumulatorSum::create(getExpCtx().get()),
                WindowBounds::Unbounded{});
        }(),
        AssertionException,
        5374100);
}

}  // namespace
}  // namespace mongo
