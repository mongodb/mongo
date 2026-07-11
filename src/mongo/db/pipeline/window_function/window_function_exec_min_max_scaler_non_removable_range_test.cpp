// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/window_function/window_function_exec_min_max_scaler_non_removable_range.h"

#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/window_function/partition_iterator.h"
#include "mongo/db/pipeline/window_function/window_bounds.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;
class WindowFunctionExecMinMaxScalerNonRemovableRangeTest : public AggregationContextFixture {
public:
    WindowFunctionExecMinMaxScalerNonRemovableRange createForFieldPath(
        std::deque<DocumentSource::GetNextResult> docs,
        const std::string& inputPath,
        const std::string& sortByPath,
        WindowBounds::Bound<Value> upperBound,
        std::pair<Value, Value> sMinAndMax = {Value(0), Value(1)}) {
        _docStage = exec::agg::MockStage::createForTest(std::move(docs), getExpCtx());
        SortPattern sortPattern{BSON(sortByPath << 1), getExpCtx()};
        _iter = std::make_unique<PartitionIterator>(
            getExpCtx().get(), _docStage.get(), &_tracker, boost::none, sortPattern);
        auto input = ExpressionFieldPath::parse(
            getExpCtx().get(), inputPath, getExpCtx()->variablesParseState);
        auto sortBy = ExpressionFieldPath::parse(
            getExpCtx().get(), "$" + sortByPath, getExpCtx()->variablesParseState);
        return WindowFunctionExecMinMaxScalerNonRemovableRange(
            _iter.get(),
            std::move(input),
            sortBy,
            createRangeBounds(WindowBounds::Unbounded{}, upperBound),
            &_tracker["output"],
            sMinAndMax);
    }

    auto advanceIterator() {
        return _iter->advance();
    }

    WindowBounds createRangeBounds(WindowBounds::Bound<Value> min, WindowBounds::Bound<Value> max) {
        return WindowBounds{WindowBounds::RangeBased{min, max}};
    }

    void runTest(const std::deque<DocumentSource::GetNextResult>& docs,
                 WindowBounds::Bound<Value> upperBound,
                 std::vector<double> expectedValues,
                 const std::string& sortByPath) {
        auto mgr = createForFieldPath(docs, "$a", sortByPath, upperBound);
        for (int i = 0; i < (int)expectedValues.size(); i++) {
            auto currentDoc = docs[i].getDocument();
            ASSERT_APPROX_EQUAL(
                expectedValues[i], mgr.getNext(currentDoc).coerceToDouble(), 0.0001);
            advanceIterator();
        }
    }

    MemoryUsageTracker _tracker{false,
                                MemoryUsageLimit{100 * 1024 * 1024} /* default memory limit */};

private:
    boost::intrusive_ptr<exec::agg::MockStage> _docStage;
    std::unique_ptr<PartitionIterator> _iter;
};

TEST_F(WindowFunctionExecMinMaxScalerNonRemovableRangeTest, AccumulateNoLookahead) {
    // If a single value is added into the window, $minMaxScaler should always return 0.
    // This is because the window has no range between the min and the max.
    // Therefore, the first value in the expected results vector should always be 0, for this tests
    // and all subsequent ones.
    const auto docs = std::deque<DocumentSource::GetNextResult>{
        Document{{"a", 1}}, Document{{"a", 2}}, Document{{"a", 3}}};
    runTest(docs, Value(0), {0, 1, 1}, "a");
}

TEST_F(WindowFunctionExecMinMaxScalerNonRemovableRangeTest, AccumulateLookahead) {
    const auto docs = std::deque<DocumentSource::GetNextResult>{
        Document{{"a", 1}}, Document{{"a", 2}}, Document{{"a", 3}}};
    runTest(docs, Value(1), {0, 0.5, 1}, "a");
}

TEST_F(WindowFunctionExecMinMaxScalerNonRemovableRangeTest, AccumulateLookaheadOutOfRange) {
    const auto docs = std::deque<DocumentSource::GetNextResult>{
        Document{{"a", 1}}, Document{{"a", 2}}, Document{{"a", 5}}};
    runTest(docs, Value(2), {0, 1, 1}, "a");
}

TEST_F(WindowFunctionExecMinMaxScalerNonRemovableRangeTest, AccumulateUnbounded) {
    const auto docs = std::deque<DocumentSource::GetNextResult>{
        Document{{"a", 1}}, Document{{"a", 2}}, Document{{"a", 5}}};
    runTest(docs, WindowBounds::Unbounded{}, {0, 0.25, 1}, "a");
}

TEST_F(WindowFunctionExecMinMaxScalerNonRemovableRangeTest, AccumulateMultiplePartitions) {
    const auto docs =
        std::deque<DocumentSource::GetNextResult>{Document{{"a", 1}, {"y", 1}, {"key", 1}},
                                                  Document{{"a", 2}, {"y", 2}, {"key", 1}},
                                                  Document{{"a", 3}, {"y", 3}, {"key", 1}},
                                                  Document{{"a", 4}, {"y", 5}, {"key", 1}},
                                                  Document{{"a", 0}, {"y", 1}, {"key", 2}},
                                                  Document{{"a", 4}, {"y", 1}, {"key", 2}},
                                                  Document{{"a", 10}, {"y", 1}, {"key", 3}}};
    auto mock = exec::agg::MockStage::createForTest(std::move(docs), getExpCtx());
    auto key = ExpressionFieldPath::createPathFromString(
        getExpCtx().get(), "key", getExpCtx()->variablesParseState);
    SortPattern sortPattern{BSON("y" << 1), getExpCtx()};
    auto iter = PartitionIterator(getExpCtx().get(),
                                  mock.get(),
                                  &_tracker,
                                  boost::optional<boost::intrusive_ptr<Expression>>(key),
                                  sortPattern);

    auto input =
        ExpressionFieldPath::parse(getExpCtx().get(), "$a", getExpCtx()->variablesParseState);
    auto sortBy =
        ExpressionFieldPath::parse(getExpCtx().get(), "$y", getExpCtx()->variablesParseState);

    auto mgr = WindowFunctionExecMinMaxScalerNonRemovableRange(
        &iter,
        std::move(input),
        sortBy,
        createRangeBounds(WindowBounds::Unbounded{}, Value(2)),
        &_tracker["output"],
        {Value(0), Value(1)});

    ASSERT_VALUE_EQ(Value(0), mgr.getNext(Document{{"a", 1}, {"y", 1}, {"key", 1}}));
    iter.advance();
    ASSERT_VALUE_EQ(Value(0.5), mgr.getNext(Document{{"a", 2}, {"y", 2}, {"key", 1}}));
    iter.advance();
    ASSERT_APPROX_EQUAL(
        0.6666, mgr.getNext(Document{{"a", 3}, {"y", 3}, {"key", 1}}).coerceToDouble(), 0.0001);
    iter.advance();
    ASSERT_VALUE_EQ(Value(1), mgr.getNext(Document{{"a", 4}, {"y", 5}, {"key", 1}}));
    iter.advance();
    // Normally the stage would be responsible for detecting a new partition, for this test reset
    // the WindowFunctionExec directly.
    mgr.reset();
    ASSERT_VALUE_EQ(Value(0), mgr.getNext(Document{{"a", 0}, {"y", 1}, {"key", 2}}));
    iter.advance();
    ASSERT_VALUE_EQ(Value(1), mgr.getNext(Document{{"a", 4}, {"y", 1}, {"key", 2}}));
    iter.advance();
    mgr.reset();
    ASSERT_VALUE_EQ(Value(0), mgr.getNext(Document{{"a", 10}, {"y", 1}, {"key", 3}}));
}

using WindowFunctionExecMinMaxScalerNonRemovableRangeTestDeathTest =
    WindowFunctionExecMinMaxScalerNonRemovableRangeTest;
DEATH_TEST_F(WindowFunctionExecMinMaxScalerNonRemovableRangeTestDeathTest,
             GetWindowValueThrowsWithNonNumericInput,
             "10487006") {
    const auto docs = std::deque<DocumentSource::GetNextResult>{
        Document{{"a", Value("invalid_string"sv)}, {"y", 1}, {"key", 1}},
        Document{{"a", 4}, {"y", 5}, {"key", 1}}};
    auto mock = exec::agg::MockStage::createForTest(std::move(docs), getExpCtx());
    auto key = ExpressionFieldPath::createPathFromString(
        getExpCtx().get(), "key", getExpCtx()->variablesParseState);
    SortPattern sortPattern{BSON("y" << 1), getExpCtx()};
    auto iter = PartitionIterator(getExpCtx().get(),
                                  mock.get(),
                                  &_tracker,
                                  boost::optional<boost::intrusive_ptr<Expression>>(key),
                                  sortPattern);

    auto input =
        ExpressionFieldPath::parse(getExpCtx().get(), "$a", getExpCtx()->variablesParseState);
    auto sortBy =
        ExpressionFieldPath::parse(getExpCtx().get(), "$y", getExpCtx()->variablesParseState);

    auto mgr = WindowFunctionExecMinMaxScalerNonRemovableRange(
        &iter,
        std::move(input),
        sortBy,
        createRangeBounds(WindowBounds::Unbounded{}, Value(2)),
        &_tracker["output"],
        {Value(0), Value(1)});
    mgr.getWindowValue(Document{{"a", Value("invalid_string"sv)}, {"y", 1}, {"key", 1}});
}

TEST_F(WindowFunctionExecMinMaxScalerNonRemovableRangeTest,
       UpdateWindowValueThrowsWithNonNumericInput) {
    const auto docs = std::deque<DocumentSource::GetNextResult>{
        Document{{"a", Value("invalid_string"sv)}, {"y", 1}, {"key", 1}},
        Document{{"a", 4}, {"y", 5}, {"key", 1}}};
    auto mock = exec::agg::MockStage::createForTest(std::move(docs), getExpCtx());
    auto key = ExpressionFieldPath::createPathFromString(
        getExpCtx().get(), "key", getExpCtx()->variablesParseState);
    SortPattern sortPattern{BSON("y" << 1), getExpCtx()};
    auto iter = PartitionIterator(getExpCtx().get(),
                                  mock.get(),
                                  &_tracker,
                                  boost::optional<boost::intrusive_ptr<Expression>>(key),
                                  sortPattern);

    auto input =
        ExpressionFieldPath::parse(getExpCtx().get(), "$a", getExpCtx()->variablesParseState);
    auto sortBy =
        ExpressionFieldPath::parse(getExpCtx().get(), "$y", getExpCtx()->variablesParseState);

    auto mgr = WindowFunctionExecMinMaxScalerNonRemovableRange(
        &iter,
        std::move(input),
        sortBy,
        createRangeBounds(WindowBounds::Unbounded{}, Value(2)),
        &_tracker["output"],
        {Value(0), Value(1)});
    ASSERT_THROWS_CODE(
        mgr.getNext(Document{{"a", Value("invalid_string"sv)}, {"y", 1}, {"key", 1}}),
        AssertionException,
        10487004);
}

}  // namespace
}  // namespace mongo
