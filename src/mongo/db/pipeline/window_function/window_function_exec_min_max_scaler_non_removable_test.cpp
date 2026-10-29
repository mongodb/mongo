// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/window_function/window_function_exec_min_max_scaler_non_removable.h"

#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_mock.h"
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
class WindowFunctionExecMinMaxScalerNonRemovableTest : public AggregationContextFixture {
public:
    WindowFunctionExecMinMaxScalerNonRemovable createForFieldPath(
        std::deque<DocumentSource::GetNextResult> docs,
        const std::string& inputPath,
        WindowBounds::Bound<int> upper,
        std::pair<Value, Value> sMinAndMax = {Value(0), Value(1)}) {
        _docStage = exec::agg::MockStage::createForTest(std::move(docs), getExpCtx());
        _iter = std::make_unique<PartitionIterator>(
            getExpCtx().get(), _docStage.get(), &_tracker, boost::none, boost::none);
        auto input = ExpressionFieldPath::parse(
            getExpCtx().get(), inputPath, getExpCtx()->variablesParseState);
        return WindowFunctionExecMinMaxScalerNonRemovable(
            _iter.get(), std::move(input), upper, &_tracker["output"], sMinAndMax);
    }

    auto advanceIterator() {
        return _iter->advance();
    }

    void runTest(const std::deque<DocumentSource::GetNextResult>& docs,
                 WindowBounds::Bound<int> upper,
                 std::vector<double> expectedValues) {
        auto mgr = createForFieldPath(docs, "$a", upper);
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

TEST_F(WindowFunctionExecMinMaxScalerNonRemovableTest, AccumulateOnlyWithoutLookahead) {
    // If a single value is added into the window, $minMaxScaler should always return 0.
    // This is because the window has no range between the min and the max.
    // Therefore, the first value in the expected results vector should always be 0, for this tests
    // and all subsequent ones.
    const auto docs = std::deque<DocumentSource::GetNextResult>{
        Document{{"a", 1}}, Document{{"a", 2}}, Document{{"a", 3}}};
    runTest(docs, WindowBounds::Current{}, {0, 1, 1});
}

TEST_F(WindowFunctionExecMinMaxScalerNonRemovableTest, AccumulateOnlyWithLookahead) {
    const auto docs = std::deque<DocumentSource::GetNextResult>{
        Document{{"a", 1}}, Document{{"a", 2}}, Document{{"a", 3}}, Document{{"a", 4}}};
    runTest(docs, 1, {0, 0.5, 0.6666, 1});
}

TEST_F(WindowFunctionExecMinMaxScalerNonRemovableTest, AccumulateUnbounded) {
    const auto docs = std::deque<DocumentSource::GetNextResult>{
        Document{{"a", 1}}, Document{{"a", 2}}, Document{{"a", 3}}, Document{{"a", 4}}};
    runTest(docs, WindowBounds::Unbounded{}, {0, 0.3333, 0.6666, 1});
}

TEST_F(WindowFunctionExecMinMaxScalerNonRemovableTest, AccumulateMultiplePartitions) {
    const auto docs = std::deque<DocumentSource::GetNextResult>{Document{{"a", 1}, {"key", 1}},
                                                                Document{{"a", 2}, {"key", 1}},
                                                                Document{{"a", 0}, {"key", 2}},
                                                                Document{{"a", 4}, {"key", 2}},
                                                                Document{{"a", 3}, {"key", 3}}};
    auto mock = exec::agg::MockStage::createForTest(std::move(docs), getExpCtx());
    auto key = ExpressionFieldPath::createPathFromString(
        getExpCtx().get(), "key", getExpCtx()->variablesParseState);
    auto iter = PartitionIterator(getExpCtx().get(),
                                  mock.get(),
                                  &_tracker,
                                  boost::optional<boost::intrusive_ptr<Expression>>(key),
                                  boost::none);
    auto input =
        ExpressionFieldPath::parse(getExpCtx().get(), "$a", getExpCtx()->variablesParseState);
    auto mgr = WindowFunctionExecMinMaxScalerNonRemovable(&iter,
                                                          std::move(input),
                                                          WindowBounds::Unbounded{},
                                                          &_tracker["output"],
                                                          {Value(0), Value(1)});
    ASSERT_VALUE_EQ(Value(0), mgr.getNext(Document{{"a", 1}, {"key", 1}}));
    iter.advance();
    ASSERT_VALUE_EQ(Value(1), mgr.getNext(Document{{"a", 2}, {"key", 1}}));
    iter.advance();
    // Normally the stage would be responsible for detecting a new partition, for this test reset
    // the WindowFunctionExec directly.
    mgr.reset();
    ASSERT_VALUE_EQ(Value(0), mgr.getNext(Document{{"a", 0}, {"key", 2}}));
    iter.advance();
    ASSERT_VALUE_EQ(Value(1), mgr.getNext(Document{{"a", 4}, {"key", 2}}));
    iter.advance();
    mgr.reset();
    ASSERT_VALUE_EQ(Value(0), mgr.getNext(Document{{"a", 3}, {"key", 3}}));
}

using WindowFunctionExecMinMaxScalerNonRemovableTestDeathTest =
    WindowFunctionExecMinMaxScalerNonRemovableTest;
DEATH_TEST_F(WindowFunctionExecMinMaxScalerNonRemovableTestDeathTest,
             GetWindowValueThrowsWithNonNumericInput,
             "10487003") {
    const auto docs = std::deque<DocumentSource::GetNextResult>{
        Document{{"a", Value("invalid_string"sv)}, {"key", 1}}, Document{{"a", 2}, {"key", 1}}};
    auto mock = exec::agg::MockStage::createForTest(std::move(docs), getExpCtx());
    auto key = ExpressionFieldPath::createPathFromString(
        getExpCtx().get(), "key", getExpCtx()->variablesParseState);
    auto iter = PartitionIterator(getExpCtx().get(),
                                  mock.get(),
                                  &_tracker,
                                  boost::optional<boost::intrusive_ptr<Expression>>(key),
                                  boost::none);
    auto input =
        ExpressionFieldPath::parse(getExpCtx().get(), "$a", getExpCtx()->variablesParseState);
    auto mgr = WindowFunctionExecMinMaxScalerNonRemovable(&iter,
                                                          std::move(input),
                                                          WindowBounds::Unbounded{},
                                                          &_tracker["output"],
                                                          {Value(0), Value(1)});
    mgr.getWindowValue(Document{{"a", Value("invalid_string"sv)}, {"key", 1}});
}

TEST_F(WindowFunctionExecMinMaxScalerNonRemovableTest, UpdateWindowValueThrowsWithNonNumericInput) {
    const auto docs = std::deque<DocumentSource::GetNextResult>{
        Document{{"a", Value("invalid_string"sv)}, {"key", 1}}, Document{{"a", 2}, {"key", 1}}};
    auto mock = exec::agg::MockStage::createForTest(std::move(docs), getExpCtx());
    auto key = ExpressionFieldPath::createPathFromString(
        getExpCtx().get(), "key", getExpCtx()->variablesParseState);
    auto iter = PartitionIterator(getExpCtx().get(),
                                  mock.get(),
                                  &_tracker,
                                  boost::optional<boost::intrusive_ptr<Expression>>(key),
                                  boost::none);
    auto input =
        ExpressionFieldPath::parse(getExpCtx().get(), "$a", getExpCtx()->variablesParseState);
    auto mgr = WindowFunctionExecMinMaxScalerNonRemovable(&iter,
                                                          std::move(input),
                                                          WindowBounds::Unbounded{},
                                                          &_tracker["output"],
                                                          {Value(0), Value(1)});
    ASSERT_THROWS_CODE(mgr.getNext(Document{{"a", Value("invalid_string"sv)}, {"key", 1}}),
                       AssertionException,
                       10487001);
}

}  // namespace
}  // namespace mongo
