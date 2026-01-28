/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/window_function/partition_iterator.h"
#include "mongo/db/pipeline/window_function/window_function_exec_non_removable.h"
#include "mongo/unittest/unittest.h"

#include <deque>
#include <memory>
#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

class WindowFunctionMergeObjectsTest : public AggregationContextFixture {
public:
    WindowFunctionMergeObjectsTest() {}

    template <class AccumulatorType>
    WindowFunctionExecNonRemovable createForFieldPath(
        std::deque<DocumentSource::GetNextResult> docs,
        const std::string& inputPath,
        WindowBounds::Bound<int> upperBound,
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
                upperBound,
                &_tracker["output"]);
        } else {
            return WindowFunctionExecNonRemovable(
                _iter.get(),
                std::move(input),
                make_intrusive<AccumulatorType>(getExpCtx().get()),
                upperBound,
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

/**
 * Test the behavior of the $mergeObjects window function.
 * $mergeObjects can only be a non-removable window function because the result of $mergeObjects is
 * dependent on the order of the documents added to the accumulator (since later documents overwrite
 * the values of conflicting field names).
 */

TEST_F(WindowFunctionMergeObjectsTest, AccumulateOnlyWithoutLookahead) {
    const auto docs =
        std::deque<DocumentSource::GetNextResult>{Document{{"obj", Document({{"a", 1}})}},
                                                  Document{{"obj", Document({{"b", 2}})}},
                                                  Document{{"obj", Document({{"c", 3}})}}};
    auto mgr = createForFieldPath<AccumulatorMergeObjects>(std::move(docs), "$obj", 0);
    ASSERT_VALUE_EQ(Value(Document({{"a", 1}})), mgr.getNext());
    advanceIterator();
    ASSERT_VALUE_EQ(Value(Document({{"a", 1}, {"b", 2}})), mgr.getNext());
    advanceIterator();
    ASSERT_VALUE_EQ(Value(Document({{"a", 1}, {"b", 2}, {"c", 3}})), mgr.getNext());
}

TEST_F(WindowFunctionMergeObjectsTest, AccumulateOnlyWithExplicitCurrentUpperBound) {
    const auto docs =
        std::deque<DocumentSource::GetNextResult>{Document{{"obj", Document({{"a", 1}})}},
                                                  Document{{"obj", Document({{"b", 2}})}},
                                                  Document{{"obj", Document({{"c", 3}})}}};
    auto mgr = createForFieldPath<AccumulatorMergeObjects>(
        std::move(docs), "$obj", WindowBounds::Current{});
    ASSERT_VALUE_EQ(Value(Document({{"a", 1}})), mgr.getNext());
    advanceIterator();
    ASSERT_VALUE_EQ(Value(Document({{"a", 1}, {"b", 2}})), mgr.getNext());
    advanceIterator();
    ASSERT_VALUE_EQ(Value(Document({{"a", 1}, {"b", 2}, {"c", 3}})), mgr.getNext());
}

TEST_F(WindowFunctionMergeObjectsTest, AccumulateOnlyWithLookahead) {
    const auto docs =
        std::deque<DocumentSource::GetNextResult>{Document{{"obj", Document({{"a", 1}})}},
                                                  Document{{"obj", Document({{"b", 2}})}},
                                                  Document{{"obj", Document({{"c", 3}})}}};
    auto mgr = createForFieldPath<AccumulatorMergeObjects>(std::move(docs), "$obj", 1);
    ASSERT_VALUE_EQ(Value(Document({{"a", 1}, {"b", 2}})), mgr.getNext());
    advanceIterator();
    ASSERT_VALUE_EQ(Value(Document({{"a", 1}, {"b", 2}, {"c", 3}})), mgr.getNext());
    advanceIterator();
    ASSERT_VALUE_EQ(Value(Document({{"a", 1}, {"b", 2}, {"c", 3}})), mgr.getNext());
}

TEST_F(WindowFunctionMergeObjectsTest, AccumulateOnlyWithLookBehind) {
    const auto docs =
        std::deque<DocumentSource::GetNextResult>{Document{{"obj", Document({{"a", 1}})}},
                                                  Document{{"obj", Document({{"b", 2}})}},
                                                  Document{{"obj", Document({{"c", 3}})}}};
    auto mgr = createForFieldPath<AccumulatorMergeObjects>(std::move(docs), "$obj", -1);
    ASSERT_VALUE_EQ(Value(Document()), mgr.getNext());
    advanceIterator();
    ASSERT_VALUE_EQ(Value(Document({{"a", 1}})), mgr.getNext());
    advanceIterator();
    ASSERT_VALUE_EQ(Value(Document({{"a", 1}, {"b", 2}})), mgr.getNext());
}

TEST_F(WindowFunctionMergeObjectsTest, OverlappingFieldsOverrideInOrder) {
    const auto docs = std::deque<DocumentSource::GetNextResult>{
        Document{{"obj", Document({{"a", 1}, {"b", 10}})}},
        Document{{"obj", Document({{"a", 2}, {"c", 20}})}},
        Document{{"obj", Document({{"a", 3}, {"d", 30}})}}};
    auto mgr = createForFieldPath<AccumulatorMergeObjects>(std::move(docs), "$obj", 0);
    ASSERT_VALUE_EQ(Value(Document({{"a", 1}, {"b", 10}})), mgr.getNext());
    advanceIterator();
    ASSERT_VALUE_EQ(Value(Document({{"a", 2}, {"b", 10}, {"c", 20}})), mgr.getNext());
    advanceIterator();
    ASSERT_VALUE_EQ(Value(Document({{"a", 3}, {"b", 10}, {"c", 20}, {"d", 30}})), mgr.getNext());
}

TEST_F(WindowFunctionMergeObjectsTest, EmptyDocumentsInWindow) {
    const auto docs =
        std::deque<DocumentSource::GetNextResult>{Document{{"obj", Document({{"a", 1}})}},
                                                  Document{{"obj", Document({})}},
                                                  Document{{"obj", Document({{"b", 2}})}}};
    auto mgr = createForFieldPath<AccumulatorMergeObjects>(std::move(docs), "$obj", 0);
    ASSERT_VALUE_EQ(Value(Document({{"a", 1}})), mgr.getNext());
    advanceIterator();
    ASSERT_VALUE_EQ(Value(Document({{"a", 1}})), mgr.getNext());
    advanceIterator();
    ASSERT_VALUE_EQ(Value(Document({{"a", 1}, {"b", 2}})), mgr.getNext());
}

TEST_F(WindowFunctionMergeObjectsTest, MultiplePartitions) {
    const auto docs = std::deque<DocumentSource::GetNextResult>{
        Document{{"obj", Document({{"a", 1}})}, {"key", 1}},
        Document{{"obj", Document({{"b", 2}})}, {"key", 2}},
        Document{{"obj", Document({{"c", 3}})}, {"key", 3}}};
    auto mock = exec::agg::MockStage::createForTest(std::move(docs), getExpCtx());
    auto key = ExpressionFieldPath::createPathFromString(
        getExpCtx().get(), "key", getExpCtx()->variablesParseState);
    auto iter = PartitionIterator(getExpCtx().get(),
                                  mock.get(),
                                  &_tracker,
                                  boost::optional<boost::intrusive_ptr<Expression>>(key),
                                  boost::none);
    auto input =
        ExpressionFieldPath::parse(getExpCtx().get(), "$obj", getExpCtx()->variablesParseState);
    auto mgr =
        WindowFunctionExecNonRemovable(&iter,
                                       std::move(input),
                                       make_intrusive<AccumulatorMergeObjects>(getExpCtx().get()),
                                       0,
                                       &_tracker["output"]);
    ASSERT_VALUE_EQ(Value(Document({{"a", 1}})), mgr.getNext());
    iter.advance();

    // Reset for new partition
    mgr.reset();
    ASSERT_VALUE_EQ(Value(Document({{"b", 2}})), mgr.getNext());
    iter.advance();
    mgr.reset();
    ASSERT_VALUE_EQ(Value(Document({{"c", 3}})), mgr.getNext());
}

TEST_F(WindowFunctionMergeObjectsTest, NestedDocuments) {
    const auto docs = std::deque<DocumentSource::GetNextResult>{
        Document{{"obj", Document({{"a", Document({{"x", 1}})}})}},
        Document{{"obj", Document({{"a", Document({{"y", 2}})}})}},
        Document{{"obj", Document({{"b", 3}})}}};
    auto mgr = createForFieldPath<AccumulatorMergeObjects>(std::move(docs), "$obj", 0);
    ASSERT_VALUE_EQ(Value(Document({{"a", Document({{"x", 1}})}})), mgr.getNext());
    advanceIterator();
    // Nested documents are replaced entirely, not merged recursively
    ASSERT_VALUE_EQ(Value(Document({{"a", Document({{"y", 2}})}})), mgr.getNext());
    advanceIterator();
    ASSERT_VALUE_EQ(Value(Document({{"a", Document({{"y", 2}})}, {"b", 3}})), mgr.getNext());
}

}  // namespace
}  // namespace mongo
