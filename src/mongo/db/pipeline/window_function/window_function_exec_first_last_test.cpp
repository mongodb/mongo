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
#include "mongo/db/pipeline/window_function/window_function_exec_first_last.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class WindowFunctionExecFirstLastTest : public AggregationContextFixture {
public:
    std::pair<WindowFunctionExecFirst, WindowFunctionExecLast> createExecs(
        std::deque<DocumentSource::GetNextResult> docs,
        WindowBounds bounds,
        boost::optional<Value> defaultVal = boost::none,
        boost::optional<std::string> keyPath = boost::none) {
        // 'defaultValue' is an internal functionality of $first needed for $shift desugaring.
        using optExp = boost::optional<boost::intrusive_ptr<Expression>>;
        using optVal = boost::optional<Value>;
        _docSource = DocumentSourceMock::createForTest(std::move(docs), getExpCtx());
        auto expCtx = getExpCtx().get();
        auto vps = expCtx->variablesParseState;
        auto optKey =
            keyPath ? optExp(ExpressionFieldPath::parse(expCtx, *keyPath, vps)) : boost::none;
        _iter = std::make_unique<PartitionIterator>(
            expCtx, _docSource.get(), &_tracker, optKey, boost::none);
        auto inputField = ExpressionFieldPath::parse(expCtx, "$val", vps);

        return {WindowFunctionExecFirst(
                    _iter.get(), inputField, bounds, defaultVal, &_tracker["first"]),
                WindowFunctionExecLast(_iter.get(), inputField, bounds, &_tracker["last"])};
    }

    auto advanceIterator() {
        return _iter->advance();
    }

private:
    boost::intrusive_ptr<DocumentSourceMock> _docSource;
    MemoryUsageTracker _tracker{false, 100 * 1024 * 1024 /* default memory limit */};
    std::unique_ptr<PartitionIterator> _iter;
};

TEST_F(WindowFunctionExecFirstLastTest, LookBehind) {
    const auto docs = std::deque<DocumentSource::GetNextResult>{
        Document{{"val", 0}},
        Document{{"val", 1}},
        Document{{"val", 2}},
    };

    // Look behind 1 document.
    auto [fst, lst] = createExecs(std::move(docs), {WindowBounds::DocumentBased{-1, 0}});
    ASSERT_VALUE_EQ(Value(0), fst.getNext());
    ASSERT_VALUE_EQ(Value(0), lst.getNext());
    advanceIterator();
    ASSERT_VALUE_EQ(Value(0), fst.getNext());
    ASSERT_VALUE_EQ(Value(1), lst.getNext());
    advanceIterator();
    ASSERT_VALUE_EQ(Value(1), fst.getNext());
    ASSERT_VALUE_EQ(Value(2), lst.getNext());
}

TEST_F(WindowFunctionExecFirstLastTest, LookAhead) {
    const auto docs = std::deque<DocumentSource::GetNextResult>{
        Document{{"val", 0}},
        Document{{"val", 1}},
        Document{{"val", 2}},
    };

    // Look ahead 1 document.
    auto [fst, lst] = createExecs(std::move(docs), {WindowBounds::DocumentBased{0, +1}});
    ASSERT_VALUE_EQ(Value(0), fst.getNext());
    ASSERT_VALUE_EQ(Value(1), lst.getNext());
    advanceIterator();
    ASSERT_VALUE_EQ(Value(1), fst.getNext());
    ASSERT_VALUE_EQ(Value(2), lst.getNext());
    advanceIterator();
    ASSERT_VALUE_EQ(Value(2), fst.getNext());
    ASSERT_VALUE_EQ(Value(2), lst.getNext());
}

TEST_F(WindowFunctionExecFirstLastTest, LookAround) {
    const auto docs = std::deque<DocumentSource::GetNextResult>{
        Document{{"val", 0}},
        Document{{"val", 1}},
        Document{{"val", 2}},
    };

    // Look around 1 document (look 1 behind and 1 ahead).
    auto [fst, lst] = createExecs(std::move(docs), {WindowBounds::DocumentBased{-1, +1}});
    ASSERT_VALUE_EQ(Value(0), fst.getNext());
    ASSERT_VALUE_EQ(Value(1), lst.getNext());
    advanceIterator();
    ASSERT_VALUE_EQ(Value(0), fst.getNext());
    ASSERT_VALUE_EQ(Value(2), lst.getNext());
    advanceIterator();
    ASSERT_VALUE_EQ(Value(1), fst.getNext());
    ASSERT_VALUE_EQ(Value(2), lst.getNext());
}

TEST_F(WindowFunctionExecFirstLastTest, UnboundedBefore) {
    const auto docs = std::deque<DocumentSource::GetNextResult>{
        Document{{"val", 0}},
        Document{{"val", 1}},
        Document{{"val", 2}},
    };

    auto [fst, lst] = createExecs(
        std::move(docs),
        {WindowBounds::DocumentBased{WindowBounds::Unbounded{}, WindowBounds::Current{}}});
    ASSERT_VALUE_EQ(Value(0), fst.getNext());
    ASSERT_VALUE_EQ(Value(0), lst.getNext());
    advanceIterator();
    ASSERT_VALUE_EQ(Value(0), fst.getNext());
    ASSERT_VALUE_EQ(Value(1), lst.getNext());
    advanceIterator();
    ASSERT_VALUE_EQ(Value(0), fst.getNext());
    ASSERT_VALUE_EQ(Value(2), lst.getNext());
}

TEST_F(WindowFunctionExecFirstLastTest, UnboundedAfter) {
    const auto docs = std::deque<DocumentSource::GetNextResult>{
        Document{{"val", 0}},
        Document{{"val", 1}},
        Document{{"val", 2}},
    };

    auto [fst, lst] = createExecs(
        std::move(docs),
        {WindowBounds::DocumentBased{WindowBounds::Current{}, WindowBounds::Unbounded{}}});
    ASSERT_VALUE_EQ(Value(0), fst.getNext());
    ASSERT_VALUE_EQ(Value(2), lst.getNext());
    advanceIterator();
    ASSERT_VALUE_EQ(Value(1), fst.getNext());
    ASSERT_VALUE_EQ(Value(2), lst.getNext());
    advanceIterator();
    ASSERT_VALUE_EQ(Value(2), fst.getNext());
    ASSERT_VALUE_EQ(Value(2), lst.getNext());
}

TEST_F(WindowFunctionExecFirstLastTest, Unbounded) {
    const auto docs = std::deque<DocumentSource::GetNextResult>{
        Document{{"val", 0}},
        Document{{"val", 1}},
        Document{{"val", 2}},
    };

    auto [fst, lst] = createExecs(
        std::move(docs),
        {WindowBounds::DocumentBased{WindowBounds::Unbounded{}, WindowBounds::Unbounded{}}});
    ASSERT_VALUE_EQ(Value(0), fst.getNext());
    ASSERT_VALUE_EQ(Value(2), lst.getNext());
    advanceIterator();
    ASSERT_VALUE_EQ(Value(0), fst.getNext());
    ASSERT_VALUE_EQ(Value(2), lst.getNext());
}

TEST_F(WindowFunctionExecFirstLastTest, SingletonWindow) {
    const auto docs = std::deque<DocumentSource::GetNextResult>{
        Document{{"val", 0}},
        Document{{"val", 1}},
        Document{{"val", 2}},
    };

    auto [fst, lst] = createExecs(std::move(docs), {WindowBounds::DocumentBased{0, 0}});
    ASSERT_VALUE_EQ(Value(0), fst.getNext());
    ASSERT_VALUE_EQ(Value(0), lst.getNext());
    advanceIterator();
    ASSERT_VALUE_EQ(Value(1), fst.getNext());
    ASSERT_VALUE_EQ(Value(1), lst.getNext());
    advanceIterator();
    ASSERT_VALUE_EQ(Value(2), fst.getNext());
    ASSERT_VALUE_EQ(Value(2), lst.getNext());
}

TEST_F(WindowFunctionExecFirstLastTest, DefaultValueForEmptyWindow1) {
    const auto docs = std::deque<DocumentSource::GetNextResult>{
        Document{{"val", 0}},
        Document{{"val", 1}},
        Document{{"val", 2}},
        Document{{"val", 3}},
        Document{{"val", 4}},
    };

    // WindowFunctionExecLast does not accept a default value because it is an internal
    // functionality meant for $shift desugaring.
    auto [fst, _] = createExecs(std::move(docs), {WindowBounds::DocumentBased{-3, -2}}, Value(-99));
    ASSERT_VALUE_EQ(Value(-99), fst.getNext());
    advanceIterator();
    ASSERT_VALUE_EQ(Value(-99), fst.getNext());
    advanceIterator();
    ASSERT_VALUE_EQ(Value(0), fst.getNext());
    advanceIterator();
    ASSERT_VALUE_EQ(Value(0), fst.getNext());
    advanceIterator();
    ASSERT_VALUE_EQ(Value(1), fst.getNext());
}

TEST_F(WindowFunctionExecFirstLastTest, DefaultValueForEmptyWindow2) {
    const auto docs = std::deque<DocumentSource::GetNextResult>{
        Document{{"val", 0}},
        Document{{"val", 1}},
    };

    auto [fst, _] = createExecs(std::move(docs), {WindowBounds::DocumentBased{3, 4}}, Value(-99));
    ASSERT_VALUE_EQ(Value(-99), fst.getNext());
    advanceIterator();
    ASSERT_VALUE_EQ(Value(-99), fst.getNext());
}

TEST_F(WindowFunctionExecFirstLastTest, NotAdvancingIterator) {
    const auto docs = std::deque<DocumentSource::GetNextResult>{
        Document{{"val", 0}},
        Document{{"val", 1}},
    };

    // getNext() returns the same value if the iterator does not advance.
    auto [fst, lst] = createExecs(std::move(docs), {WindowBounds::DocumentBased{0, 1}});
    ASSERT_VALUE_EQ(Value(0), fst.getNext());
    ASSERT_VALUE_EQ(Value(0), fst.getNext());
    ASSERT_VALUE_EQ(Value(1), lst.getNext());
    ASSERT_VALUE_EQ(Value(1), lst.getNext());
}

TEST_F(WindowFunctionExecFirstLastTest, AcrossPartitions) {
    const auto docs = std::deque<DocumentSource::GetNextResult>{
        Document{{"key", 0}, {"val", 0}},
        Document{{"key", 0}, {"val", 1}},
        Document{{"key", 1}, {"val", 2}},
        Document{{"key", 1}, {"val", 3}},
    };

    auto [fst, lst] = createExecs(std::move(docs),
                                  {WindowBounds::DocumentBased{0, 1}},
                                  boost::none,
                                  boost::optional<std::string>("$key"));
    // Partition 0, window full
    ASSERT_VALUE_EQ(Value(0), fst.getNext());
    ASSERT_VALUE_EQ(Value(1), lst.getNext());
    advanceIterator();  // Partition 0, window half-full
    ASSERT_VALUE_EQ(Value(1), fst.getNext());
    ASSERT_VALUE_EQ(Value(1), lst.getNext());
    advanceIterator();  // Partition 1, window full
    ASSERT_VALUE_EQ(Value(2), fst.getNext());
    ASSERT_VALUE_EQ(Value(3), lst.getNext());
    advanceIterator();  // Partition 1, window half-full
    ASSERT_VALUE_EQ(Value(3), fst.getNext());
    ASSERT_VALUE_EQ(Value(3), lst.getNext());
}

}  // namespace
}  // namespace mongo
