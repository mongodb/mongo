// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/window_function/window_function_exec_first_last.h"

#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/window_function/partition_iterator.h"
#include "mongo/db/pipeline/window_function/window_bounds.h"
#include "mongo/unittest/unittest.h"

#include <deque>
#include <memory>
#include <string>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

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
        _docStage = exec::agg::MockStage::createForTest(std::move(docs), getExpCtx());
        auto expCtx = getExpCtx().get();
        auto vps = expCtx->variablesParseState;
        auto optKey =
            keyPath ? optExp(ExpressionFieldPath::parse(expCtx, *keyPath, vps)) : boost::none;
        _iter = std::make_unique<PartitionIterator>(
            expCtx, _docStage.get(), &_tracker, optKey, boost::none);
        auto inputField = ExpressionFieldPath::parse(expCtx, "$val", vps);

        return {WindowFunctionExecFirst(
                    _iter.get(), inputField, bounds, defaultVal, &_tracker["first"]),
                WindowFunctionExecLast(_iter.get(), inputField, bounds, &_tracker["last"])};
    }

    auto advanceIterator() {
        return _iter->advance();
    }

private:
    boost::intrusive_ptr<exec::agg::MockStage> _docStage;
    MemoryUsageTracker _tracker{false,
                                MemoryUsageLimit{100 * 1024 * 1024} /* default memory limit */};
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
