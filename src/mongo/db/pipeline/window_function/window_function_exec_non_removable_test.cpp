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

#include "mongo/db/pipeline/window_function/window_function_exec_non_removable.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/accumulator_for_window_functions.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/window_function/partition_iterator.h"
#include "mongo/db/pipeline/window_function/window_bounds.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/compiler/logical_model/sort_pattern/sort_pattern.h"
#include "mongo/unittest/unittest.h"

#include <deque>
#include <memory>
#include <string>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

class WindowFunctionExecNonRemovableTest : public AggregationContextFixture {
public:
    template <class AccumulatorType>
    WindowFunctionExecNonRemovable createForFieldPath(
        std::deque<DocumentSource::GetNextResult> docs,
        const std::string& inputPath,
        WindowBounds::Bound<int> upper,
        boost::optional<std::string> sortByPath = boost::none) {
        _docStage = exec::agg::MockStage::createForTest(std::move(docs), getExpCtx());
        _iter = std::make_unique<PartitionIterator>(
            getExpCtx().get(), _docStage.get(), &_tracker, boost::none, boost::none);
        auto input = ExpressionFieldPath::parse(
            getExpCtx().get(), inputPath, getExpCtx()->variablesParseState);
        if (sortByPath) {
            auto sortBy = ExpressionFieldPath::parse(
                getExpCtx().get(), *sortByPath, getExpCtx()->variablesParseState);
            return WindowFunctionExecNonRemovable(
                _iter.get(),
                ExpressionArray::create(
                    getExpCtx().get(),
                    std::vector<boost::intrusive_ptr<Expression>>{sortBy, input}),
                make_intrusive<AccumulatorType>(getExpCtx().get()),
                upper,
                &_tracker["output"]);
        } else {
            return WindowFunctionExecNonRemovable(
                _iter.get(),
                std::move(input),
                make_intrusive<AccumulatorType>(getExpCtx().get()),
                upper,
                &_tracker["output"]);
        }
    }

    auto advanceIterator() {
        return _iter->advance();
    }

    MemoryUsageTracker _tracker{false, 100 * 1024 * 1024 /* default memory limit */};

private:
    boost::intrusive_ptr<exec::agg::MockStage> _docStage;
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
    auto mgr = WindowFunctionExecNonRemovable(&iter,
                                              std::move(input),
                                              make_intrusive<AccumulatorSum>(getExpCtx().get()),
                                              1,
                                              &_tracker["output"]);
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

TEST_F(WindowFunctionExecNonRemovableTest, FullPartitionWindow) {
    const auto docs = std::deque<DocumentSource::GetNextResult>{
        Document{{"a", 1}}, Document{{"a", 2}}, Document{{"a", 3}}};
    auto mgr = createForFieldPath<AccumulatorSum>(std::move(docs), "$a", WindowBounds::Unbounded{});
    ASSERT_VALUE_EQ(Value(6), mgr.getNext());
    advanceIterator();
    ASSERT_VALUE_EQ(Value(6), mgr.getNext());
    advanceIterator();
    ASSERT_VALUE_EQ(Value(6), mgr.getNext());
}

TEST_F(WindowFunctionExecNonRemovableTest, InputExpressionAllowedToCreateVariables) {
    const auto docs = std::deque<DocumentSource::GetNextResult>{
        Document{{"a", 1}}, Document{{"a", 2}}, Document{{"a", 3}}};
    auto docStage = exec::agg::MockStage::createForTest(std::move(docs), getExpCtx());
    auto iter = std::make_unique<PartitionIterator>(
        getExpCtx().get(), docStage.get(), &_tracker, boost::none, boost::none);
    auto filterBSON =
        fromjson("{$filter: {input: [1, 2, 3], as: 'num', cond: {$gte: ['$$num', 2]}}}");
    auto input = ExpressionFilter::parse(
        getExpCtx().get(), filterBSON.firstElement(), getExpCtx()->variablesParseState);
    auto exec = WindowFunctionExecNonRemovable(iter.get(),
                                               std::move(input),
                                               make_intrusive<AccumulatorFirst>(getExpCtx().get()),
                                               1,
                                               &_tracker["output"]);
    // The input is a constant [2, 3] for each document.
    ASSERT_VALUE_EQ(Value(std::vector<Value>{Value(2), Value(3)}), exec.getNext());
    iter->advance();
    ASSERT_VALUE_EQ(Value(std::vector<Value>{Value(2), Value(3)}), exec.getNext());
    iter->advance();
    ASSERT_VALUE_EQ(Value(std::vector<Value>{Value(2), Value(3)}), exec.getNext());
}

TEST_F(WindowFunctionExecNonRemovableTest, CanReceiveSortByExpression) {
    const auto docs = std::deque<DocumentSource::GetNextResult>{
        Document{{"x", 1}, {"y", 0}}, Document{{"x", 3}, {"y", 2}}, Document{{"x", 5}, {"y", 4}}};
    auto mgr = createForFieldPath<AccumulatorIntegral>(
        docs, "$y" /* input */, 0, std::string("$x") /* sortBy */);
    double expectedIntegral = 0;
    ASSERT_VALUE_EQ(Value(expectedIntegral), mgr.getNext());
    advanceIterator();
    expectedIntegral += 2.0;  // (2 + 0) * (3 - 1) / 2.0 = 2.0
    ASSERT_VALUE_EQ(Value(expectedIntegral), mgr.getNext());
    advanceIterator();
    expectedIntegral += 6.0;  // (4 + 2) * (5 - 3) / 2.0 = 6.0
    ASSERT_VALUE_EQ(Value(expectedIntegral), mgr.getNext());
}

class RankTest : public WindowFunctionExecNonRemovableTest {
public:
    void setUp() override {
        _mock = mockStage();
    }

    void setSort(BSONObj sortSpec) {
        _pattern = std::make_unique<SortPattern>(sortSpec, getExpCtx());
        _sortKeyGen = std::make_unique<SortKeyGenerator>(*_pattern, nullptr);
    }

    auto docWithSortKey(const auto& doc) {
        ASSERT(_sortKeyGen) << "expected 'setSort()' to be called first";
        auto sortKeyVal = _sortKeyGen->computeSortKeyFromDocument(doc);
        MutableDocument docWithMeta(std::move(doc));
        docWithMeta.metadata().setSortKey(sortKeyVal, _pattern->isSingleElementKey());
        return docWithMeta.freeze();
    };

    void addMockedInput(auto doc) {
        _mock->push_back(docWithSortKey(doc));
    }

    auto makeRank() {
        auto partitionIter = std::make_unique<PartitionIterator>(
            getExpCtx().get(), _mock.get(), &_tracker, boost::none, *_pattern);

        auto input = make_intrusive<ExpressionInternalRawSortKey>(getExpCtx().get());
        auto exec =
            WindowFunctionExecNonRemovable(partitionIter.get(),
                                           std::move(input),
                                           make_intrusive<AccumulatorRank>(getExpCtx().get()),
                                           WindowBounds::Current{},
                                           &_tracker["output"]);
        return std::pair{std::move(partitionIter), std::move(exec)};
    }

protected:
    boost::intrusive_ptr<exec::agg::MockStage> _mock;
    std::unique_ptr<SortPattern> _pattern;
    std::unique_ptr<SortKeyGenerator> _sortKeyGen;
};

TEST_F(RankTest, BasicSortKey) {
    setSort(BSON("x" << 1));
    addMockedInput(Document{{"_id", 0}, {"x", 1}, {"y", 0}});
    addMockedInput(Document{{"_id", 1}, {"x", 1}, {"y", 1}});
    addMockedInput(Document{{"_id", 2}, {"x", 2}, {"y", 2}});
    auto&& [partitionIter, rank] = makeRank();
    ASSERT_VALUE_EQ(Value(1), rank.getNext());
    partitionIter->advance();
    ASSERT_VALUE_EQ(Value(1), rank.getNext());
    partitionIter->advance();
    ASSERT_VALUE_EQ(Value(3), rank.getNext());
}

TEST_F(RankTest, SortingArrays) {
    setSort(BSON("x" << 1));
    addMockedInput(Document{{"_id", 0}, {"x", std::vector<ImplicitValue>{1, 3, 2}}, {"y", 0}});
    addMockedInput(Document{{"_id", 1}, {"x", std::vector<ImplicitValue>{10, 1, 2}}, {"y", 1}});
    addMockedInput(Document{{"_id", 2}, {"x", std::vector<ImplicitValue>{2}}, {"y", 2}});
    auto&& [partitionIter, rank] = makeRank();
    ASSERT_VALUE_EQ(Value(1), rank.getNext());
    partitionIter->advance();
    ASSERT_VALUE_EQ(Value(1), rank.getNext());
    partitionIter->advance();
    ASSERT_VALUE_EQ(Value(3), rank.getNext());
}


TEST_F(RankTest, AllTies) {
    setSort(BSON("x" << -1));
    addMockedInput(Document{{"_id", 0}, {"x", std::vector<ImplicitValue>{1, 3, 2}}});
    addMockedInput(Document{{"_id", 1}, {"x", std::vector<ImplicitValue>{1, 1, 3}}});
    addMockedInput(Document{{"_id", 2}, {"x", std::vector<ImplicitValue>{3, 1}}});
    auto&& [partitionIter, rank] = makeRank();
    ASSERT_VALUE_EQ(Value(1), rank.getNext());
    partitionIter->advance();
    ASSERT_VALUE_EQ(Value(1), rank.getNext());
    partitionIter->advance();
    ASSERT_VALUE_EQ(Value(1), rank.getNext());
}

TEST_F(RankTest, OneIsMissingSortKey) {
    setSort(BSON("x" << -1));
    addMockedInput(Document{{"_id", 0}});
    addMockedInput(Document{{"_id", 1}, {"x", std::vector<ImplicitValue>{1, 1, 3}}});
    addMockedInput(Document{{"_id", 2}, {"x", std::vector<ImplicitValue>{3, 1}}});
    auto&& [partitionIter, rank] = makeRank();
    ASSERT_VALUE_EQ(Value(1), rank.getNext());
    partitionIter->advance();
    ASSERT_VALUE_EQ(Value(2), rank.getNext());
    partitionIter->advance();
    ASSERT_VALUE_EQ(Value(2), rank.getNext());
}

TEST_F(RankTest, AllMisingNestedSortKey) {
    setSort(BSON("x.y" << -1));
    addMockedInput(Document{{"_id", 0}});
    addMockedInput(Document{{"_id", 1}, {"x", std::vector<ImplicitValue>{1, 1, 3}}});
    addMockedInput(Document{{"_id", 2}, {"x", std::vector<ImplicitValue>{Document{{"x", 2}}, 1}}});
    auto&& [partitionIter, rank] = makeRank();
    ASSERT_VALUE_EQ(Value(1), rank.getNext());
    partitionIter->advance();
    ASSERT_VALUE_EQ(Value(1), rank.getNext());
    partitionIter->advance();
    ASSERT_VALUE_EQ(Value(1), rank.getNext());
}

TEST_F(RankTest, CorrectlyRespectsCollation) {
    getExpCtx()->setCollator(
        std::make_shared<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kReverseString));
    setSort(BSON("x" << 1));
    addMockedInput(Document{{"_id", 0}, {"x", "ba"_sd}});
    addMockedInput(Document{{"_id", 1}, {"x", "cba"_sd}});
    addMockedInput(Document{{"_id", 2}, {"x", "cba"_sd}});
    addMockedInput(Document{{"_id", 3}, {"x", "abcd"_sd}});
    auto&& [partitionIter, rank] = makeRank();
    ASSERT_VALUE_EQ(Value(1), rank.getNext());
    partitionIter->advance();
    ASSERT_VALUE_EQ(Value(2), rank.getNext());
    partitionIter->advance();
    ASSERT_VALUE_EQ(Value(2), rank.getNext());
    partitionIter->advance();
    ASSERT_VALUE_EQ(Value(4), rank.getNext());
}
}  // namespace
}  // namespace mongo
