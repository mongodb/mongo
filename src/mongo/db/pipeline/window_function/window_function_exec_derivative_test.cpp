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

#include "mongo/db/pipeline/window_function/window_function_exec_derivative.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/window_function/partition_iterator.h"
#include "mongo/db/pipeline/window_function/window_bounds.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/time_support.h"

#include <deque>
#include <memory>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

class WindowFunctionExecDerivativeTest : public AggregationContextFixture {
public:
    WindowFunctionExecDerivative createForFieldPath(
        std::deque<DocumentSource::GetNextResult> docs,
        const std::string& positionPath,
        const std::string& timePath,
        WindowBounds bounds,
        boost::optional<TimeUnit> timeUnit = boost::none) {
        _docStage = exec::agg::MockStage::createForTest(std::move(docs), getExpCtx());
        _iter = std::make_unique<PartitionIterator>(
            getExpCtx().get(), _docStage.get(), &_tracker, boost::none, boost::none);

        auto position = ExpressionFieldPath::parse(
            getExpCtx().get(), positionPath, getExpCtx()->variablesParseState);
        auto time = ExpressionFieldPath::parse(
            getExpCtx().get(), timePath, getExpCtx()->variablesParseState);

        return WindowFunctionExecDerivative(_iter.get(),
                                            std::move(position),
                                            std::move(time),
                                            std::move(bounds),
                                            std::move(timeUnit),
                                            &_tracker["output"]);
    }

    auto advanceIterator() {
        return _iter->advance();
    }

    Value eval(std::pair<Value, Value> start,
               std::pair<Value, Value> end,
               boost::optional<TimeUnit> unit = {}) {
        const std::deque<DocumentSource::GetNextResult> docs{
            Document{{"t", start.first}, {"y", start.second}},
            Document{{"t", end.first}, {"y", end.second}}};
        auto mgr = createForFieldPath(
            std::move(docs), "$y", "$t", {WindowBounds::DocumentBased{0, 1}}, std::move(unit));
        return mgr.getNext();
    }

private:
    boost::intrusive_ptr<exec::agg::MockStage> _docStage;
    MemoryUsageTracker _tracker{false, 100 * 1024 * 1024 /* default memory limit */};
    std::unique_ptr<PartitionIterator> _iter;
};

TEST_F(WindowFunctionExecDerivativeTest, LookBehind) {
    const auto docs = std::deque<DocumentSource::GetNextResult>{
        Document{{"t", 0}, {"y", 1}},
        Document{{"t", 1}, {"y", 2}},
        Document{{"t", 2}, {"y", 4}},
        Document{{"t", 3}, {"y", 8}},
    };

    // Look behind 1 document.
    auto mgr =
        createForFieldPath(std::move(docs), "$y", "$t", {WindowBounds::DocumentBased{-1, 0}});

    // Initially, the window only has one document, so we can't compute a derivative.
    ASSERT_VALUE_EQ(Value(BSONNULL), mgr.getNext());
    advanceIterator();
    // Now since t changes by 1 every time, answer should be just dy.
    ASSERT_VALUE_EQ(Value(1), mgr.getNext());
    advanceIterator();
    ASSERT_VALUE_EQ(Value(2), mgr.getNext());
    advanceIterator();
    ASSERT_VALUE_EQ(Value(4), mgr.getNext());
}

TEST_F(WindowFunctionExecDerivativeTest, LookAhead) {
    const auto docs = std::deque<DocumentSource::GetNextResult>{
        Document{{"t", 0}, {"y", 1}},
        Document{{"t", 1}, {"y", 2}},
        Document{{"t", 2}, {"y", 4}},
        Document{{"t", 3}, {"y", 8}},
    };

    // Look ahead 1 document.
    auto mgr =
        createForFieldPath(std::move(docs), "$y", "$t", {WindowBounds::DocumentBased{0, +1}});
    // Now the first document's window has two documents.
    ASSERT_VALUE_EQ(Value(1), mgr.getNext());
    advanceIterator();
    ASSERT_VALUE_EQ(Value(2), mgr.getNext());
    advanceIterator();
    ASSERT_VALUE_EQ(Value(4), mgr.getNext());
    advanceIterator();
    // At the end of the partition we only have one document.
    ASSERT_VALUE_EQ(Value(BSONNULL), mgr.getNext());
}

TEST_F(WindowFunctionExecDerivativeTest, LookAround) {
    const auto docs = std::deque<DocumentSource::GetNextResult>{
        Document{{"t", 0}, {"y", 1}},
        Document{{"t", 1}, {"y", 2}},
        Document{{"t", 2}, {"y", 4}},
        Document{{"t", 3}, {"y", 8}},
    };

    // Look around 1 document (look 1 behind and 1 ahead).
    // This case is interesting because at the partition boundaries, we can still define a
    // derivative, but the window is smaller.
    auto mgr =
        createForFieldPath(std::move(docs), "$y", "$t", {WindowBounds::DocumentBased{-1, +1}});
    // The first document sees itself and the 1 document following.
    // Time changes by 1 and y changes from 1 to 2.
    ASSERT_VALUE_EQ(Value(1), mgr.getNext());
    advanceIterator();
    // The second document sees the 1 previous and the 1 following.
    // Times changes by 2, and y changes from 1 to 4.
    ASSERT_VALUE_EQ(Value(3.0 / 2), mgr.getNext());
    advanceIterator();
    // Next, y goes from 2 to 8.
    ASSERT_VALUE_EQ(Value(6.0 / 2), mgr.getNext());
    advanceIterator();
    // Finally, the window shrinks back down to 2 documents.
    // y goes from 4 to 8.
    ASSERT_VALUE_EQ(Value(4), mgr.getNext());
}

TEST_F(WindowFunctionExecDerivativeTest, UnboundedBefore) {
    const auto docs = std::deque<DocumentSource::GetNextResult>{
        Document{{"t", 0}, {"y", 1}},
        Document{{"t", 1}, {"y", 10}},
        Document{{"t", 2}, {"y", 100}},
    };

    auto mgr = createForFieldPath(
        std::move(docs),
        "$y",
        "$t",
        {
            WindowBounds::DocumentBased{WindowBounds::Unbounded{}, WindowBounds::Current{}},
        });
    // t is 0 to 0.
    ASSERT_VALUE_EQ(Value(BSONNULL), mgr.getNext());
    advanceIterator();
    // t is 0 to 1.
    ASSERT_VALUE_EQ(Value(9.0 / 1), mgr.getNext());
    advanceIterator();
    // t is 0 to 2.
    ASSERT_VALUE_EQ(Value(99.0 / 2), mgr.getNext());
}

TEST_F(WindowFunctionExecDerivativeTest, UnboundedAfter) {
    const auto docs = std::deque<DocumentSource::GetNextResult>{
        Document{{"t", 0}, {"y", 1}},
        Document{{"t", 1}, {"y", 10}},
        Document{{"t", 2}, {"y", 100}},
    };

    auto mgr = createForFieldPath(
        std::move(docs),
        "$y",
        "$t",
        {
            WindowBounds::DocumentBased{WindowBounds::Current{}, WindowBounds::Unbounded{}},
        });
    // t is 0 to 2.
    ASSERT_VALUE_EQ(Value(99.0 / 2), mgr.getNext());
    advanceIterator();
    // t is 1 to 2.
    ASSERT_VALUE_EQ(Value(90.0 / 1), mgr.getNext());
    advanceIterator();
    // t is 2 to 2.
    ASSERT_VALUE_EQ(Value(BSONNULL), mgr.getNext());
}

TEST_F(WindowFunctionExecDerivativeTest, Unbounded) {
    const auto docs = std::deque<DocumentSource::GetNextResult>{
        Document{{"t", 0}, {"y", 1}},
        Document{{"t", 1}, {"y", 10}},
        Document{{"t", 2}, {"y", 100}},
    };

    auto mgr = createForFieldPath(
        std::move(docs),
        "$y",
        "$t",
        {
            WindowBounds::DocumentBased{WindowBounds::Unbounded{}, WindowBounds::Unbounded{}},
        });
    // t is 0 to 2, in all 3 cases.
    ASSERT_VALUE_EQ(Value(99.0 / 2), mgr.getNext());
    advanceIterator();
    ASSERT_VALUE_EQ(Value(99.0 / 2), mgr.getNext());
    advanceIterator();
    ASSERT_VALUE_EQ(Value(99.0 / 2), mgr.getNext());
}

TEST_F(WindowFunctionExecDerivativeTest, NonNumbers) {
    auto t0 = Value{0};
    auto t1 = Value{1};
    auto y0 = Value{5};
    auto y1 = Value{6};
    auto bad = Value{"a string"_sd};

    // If the position or time is an invalid type, it's an error.
    ASSERT_THROWS_CODE(eval({t0, bad}, {t1, y1}), DBException, ErrorCodes::TypeMismatch);
    ASSERT_THROWS_CODE(eval({t0, y0}, {t1, bad}), DBException, ErrorCodes::TypeMismatch);
    ASSERT_THROWS_CODE(eval({bad, y0}, {t1, y1}), DBException, 5624902);
    ASSERT_THROWS_CODE(eval({t0, y0}, {bad, y1}), DBException, 5624902);

    bad = Value{BSONNULL};
    // If the position or time is null, it's an error.
    ASSERT_THROWS_CODE(eval({t0, bad}, {t1, y1}), DBException, 5624903);
    ASSERT_THROWS_CODE(eval({t0, y0}, {t1, bad}), DBException, 5624903);
    ASSERT_THROWS_CODE(eval({bad, y0}, {t1, y1}), DBException, 5624902);
    ASSERT_THROWS_CODE(eval({t0, y0}, {bad, y1}), DBException, 5624902);

    bad = Value{};
    // If the position or time is missing, it's an error.
    ASSERT_THROWS_CODE(eval({t0, bad}, {t1, y1}), DBException, 5624903);
    ASSERT_THROWS_CODE(eval({t0, y0}, {t1, bad}), DBException, 5624903);
    ASSERT_THROWS_CODE(eval({bad, y0}, {t1, y1}), DBException, 5624902);
    ASSERT_THROWS_CODE(eval({t0, y0}, {bad, y1}), DBException, 5624902);
}

TEST_F(WindowFunctionExecDerivativeTest, DatesAreNonNumbers) {
    // When no unit is specified, dates are considered an invalid type (an error).

    auto t0 = Value{Date_t::fromMillisSinceEpoch(0)};
    auto t1 = Value{Date_t::fromMillisSinceEpoch(8)};
    auto y0 = Value{5};
    auto y1 = Value{6};
    ASSERT_THROWS_CODE(eval({t0, y0}, {t1, y1}), DBException, 5624901);
}

TEST_F(WindowFunctionExecDerivativeTest, Unit) {
    // 'y' increases by 1, over 8ms.
    auto t0 = Value{Date_t::fromMillisSinceEpoch(0)};
    auto t1 = Value{Date_t::fromMillisSinceEpoch(8)};
    auto y0 = Value{5};
    auto y1 = Value{6};

    // Calculate the derivative, expressed in the given TimeUnit.
    auto calc = [&](TimeUnit unit) -> Value {
        return eval({t0, y0}, {t1, y1}, unit);
    };
    // Each ms, 'y' increased by 1/8.
    // (This should be exact, despite floating point, because 8 is a power of 2.)
    ASSERT_VALUE_EQ(calc(TimeUnit::millisecond), Value{1.0 / 8});

    // Each second, 'y' increases by 1/8 1000 times (once per ms).
    ASSERT_VALUE_EQ(Value{(1.0 / 8) * 1000}, calc(TimeUnit::second));
    // And so on, with larger units.
    ASSERT_VALUE_EQ(Value{(1.0 / 8) * 1000 * 60}, calc(TimeUnit::minute));
    ASSERT_VALUE_EQ(Value{(1.0 / 8) * 1000 * 60 * 60}, calc(TimeUnit::hour));
    ASSERT_VALUE_EQ(Value{(1.0 / 8) * 1000 * 60 * 60 * 24}, calc(TimeUnit::day));
    ASSERT_VALUE_EQ(Value{(1.0 / 8) * 1000 * 60 * 60 * 24 * 7}, calc(TimeUnit::week));
}

TEST_F(WindowFunctionExecDerivativeTest, UnitNonDate) {
    // unit requires the time input to be a datetime: non-datetimes throw an error.

    auto t0 = Value{Date_t::fromMillisSinceEpoch(0)};
    auto t1 = Value{Date_t::fromMillisSinceEpoch(1000)};
    auto y0 = Value{0};
    auto y1 = Value{0};
    auto bad = Value{500};

    ASSERT_THROWS_CODE(eval({bad, y0}, {t1, y1}, TimeUnit::millisecond), DBException, 5624900);
    ASSERT_THROWS_CODE(eval({t0, y0}, {bad, y1}, TimeUnit::millisecond), DBException, 5624900);
}


}  // namespace
}  // namespace mongo
