/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <cmath>
#include <memory>

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/accumulator_for_window_functions.h"
#include "mongo/db/pipeline/accumulator_multi.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace AccumulatorTests {

using boost::intrusive_ptr;
using std::numeric_limits;
using std::string;

/**
 * Takes a list of pairs of arguments and expected results, and creates an AccumulatorState using
 * the provided lambda. It then asserts that for the given AccumulatorState the input returns
 * the expected results.
 */
using OperationsType = std::vector<std::pair<std::vector<Value>, Value>>;
static void assertExpectedResults(
    ExpressionContext* const expCtx,
    OperationsType operations,
    std::function<intrusive_ptr<AccumulatorState>(ExpressionContext* const)> initializeAccumulator,
    bool skipMerging = false) {
    for (auto&& op : operations) {
        try {
            // Asserts that result equals expected result when not sharded.
            {
                auto accum = initializeAccumulator(expCtx);
                for (auto&& val : op.first) {
                    accum->process(val, false);
                }
                Value result = accum->getValue(false);
                ASSERT_VALUE_EQ(op.second, result);
                ASSERT_EQUALS(op.second.getType(), result.getType());
            }

            // Asserts that result equals expected result when all input is on one shard.
            if (!skipMerging) {
                auto accum = initializeAccumulator(expCtx);
                auto shard = initializeAccumulator(expCtx);
                for (auto&& val : op.first) {
                    shard->process(val, false);
                }
                auto val = shard->getValue(true);
                accum->process(val, true);
                Value result = accum->getValue(false);
                ASSERT_VALUE_EQ(op.second, result);
                ASSERT_EQUALS(op.second.getType(), result.getType());
            }

            // Asserts that result equals expected result when each input is on a separate shard.
            if (!skipMerging) {
                auto accum = initializeAccumulator(expCtx);
                for (auto&& val : op.first) {
                    auto shard = initializeAccumulator(expCtx);
                    shard->process(val, false);
                    accum->process(shard->getValue(true), true);
                }
                Value result = accum->getValue(false);
                ASSERT_VALUE_EQ(op.second, result);
                ASSERT_EQUALS(op.second.getType(), result.getType());
            }
        } catch (...) {
            LOGV2(24180, "failed", "argument"_attr = Value(op.first));
            throw;
        }
    }
}

/**
 * Takes the name of an AccumulatorState as its template argument and a list of pairs of arguments
 * and expected results as its second argument, and asserts that for the given AccumulatorState the
 * arguments evaluate to the expected results.
 */
template <typename AccName>
static void assertExpectedResults(
    ExpressionContext* const expCtx,
    std::initializer_list<std::pair<std::vector<Value>, Value>> operations,
    bool skipMerging = false,
    boost::optional<Value> newGroupValue = boost::none) {
    auto initializeAccumulator =
        [&](ExpressionContext* const expCtx) -> intrusive_ptr<AccumulatorState> {
        auto accum = AccName::create(expCtx);
        if (newGroupValue) {
            accum->startNewGroup(*newGroupValue);
        }
        return accum;
    };
    assertExpectedResults(expCtx, OperationsType(operations), initializeAccumulator, skipMerging);
}

TEST(Accumulators, Avg) {
    auto expCtx = ExpressionContextForTest{};
    assertExpectedResults<AccumulatorAvg>(
        &expCtx,
        {
            // No documents evaluated.
            {{}, Value(BSONNULL)},

            // One int value is converted to double.
            {{Value(3)}, Value(3.0)},
            // One long value is converted to double.
            {{Value(-4LL)}, Value(-4.0)},
            // One double value.
            {{Value(22.6)}, Value(22.6)},

            // Averaging two ints.
            {{Value(10), Value(11)}, Value(10.5)},
            // Averaging two longs.
            {{Value(10LL), Value(11LL)}, Value(10.5)},
            // Averaging two doubles.
            {{Value(10.0), Value(11.0)}, Value(10.5)},

            // The average of an int and a double is a double.
            {{Value(10), Value(11.0)}, Value(10.5)},
            // The average of a long and a double is a double.
            {{Value(5LL), Value(1.0)}, Value(3.0)},
            // The average of an int and a long is a double.
            {{Value(5), Value(3LL)}, Value(4.0)},
            // Averaging an int, long, and double.
            {{Value(1), Value(2LL), Value(6.0)}, Value(3.0)},

            // Unlike $sum, two ints do not overflow in the 'total' portion of the average.
            {{Value(numeric_limits<int>::max()), Value(numeric_limits<int>::max())},
             Value(static_cast<double>(numeric_limits<int>::max()))},
            // Two longs do overflow in the 'total' portion of the average.
            {{Value(numeric_limits<long long>::max()), Value(numeric_limits<long long>::max())},
             Value(static_cast<double>(numeric_limits<long long>::max()))},

            // Averaging two decimals.
            {{Value(Decimal128("-1234567890.1234567889")),
              Value(Decimal128("-1234567890.1234567891"))},
             Value(Decimal128("-1234567890.1234567890"))},

            // Averaging two longs and a decimal results in an accurate decimal result.
            {{Value(1234567890123456788LL),
              Value(1234567890123456789LL),
              Value(Decimal128("1234567890123456790.037037036703702"))},
             Value(Decimal128("1234567890123456789.012345678901234"))},

            // Averaging a double and a decimal
            {{Value(1.0E22), Value(Decimal128("9999999999999999999999.9999999999"))},
             Value(Decimal128("9999999999999999999999.99999999995"))},
        });
}

TEST(Accumulators, First) {
    auto expCtx = ExpressionContextForTest{};
    assertExpectedResults<AccumulatorFirst>(
        &expCtx,
        {// No documents evaluated.
         {{}, Value()},

         // The accumulator evaluates one document and retains its value.
         {{Value(5)}, Value(5)},
         // The accumulator evaluates one document with the field missing, returns missing value.
         {{Value()}, Value()},

         // The accumulator evaluates two documents and retains the value in the first.
         {{Value(5), Value(7)}, Value(5)},
         // The accumulator evaluates two documents and retains the missing value in the first.
         {{Value(), Value(7)}, Value()}});
}

TEST(Accumulators, FirstN) {
    auto expCtx = ExpressionContextForTest{};
    auto n = Value(2);

    assertExpectedResults<AccumulatorFirstN>(
        &expCtx,
        {
            // Basic test involving no values.
            {{}, Value(std::vector<Value>{})},
            // Basic test: testing 1 value.
            {{Value(3)}, Value(std::vector<Value>{Value(3)})},
            // Basic test involving 2 values.
            {{Value(3), Value(4)}, Value(std::vector<Value>{Value(3), Value(4)})},

            // Test that processes more than 'n' total values.
            {{Value(4), Value(5), Value(6), Value(3), Value(2), Value(1)},
             Value(std::vector<Value>{Value(4), Value(5)})},

            // Null and missing values should NOT be ignored (missing gets upconverted to null).
            {{Value(), Value(BSONNULL), Value(4), Value(), Value(BSONNULL), Value(5), Value(6)},
             Value(std::vector<Value>{Value(BSONNULL), Value(BSONNULL)})},

            // Testing mixed types.
            {{Value(4), Value("str"_sd), Value(3.2), Value(4.0)},
             Value(std::vector<Value>{Value(4), Value("str"_sd)})},

            // Testing duplicate values.
            {{Value("std"_sd), Value("std"_sd), Value("test"_sd)},
             Value(std::vector<Value>{Value("std"_sd), Value("std"_sd)})},

            {{Value(9.1), Value(4.22), Value(4.22)},
             Value(std::vector<Value>{Value(9.1), Value(4.22)})},
        },
        false, /* skipMerging */
        n);

    // Additional test partition where N = 1.
    n = Value(1);
    assertExpectedResults<AccumulatorFirstN>(
        &expCtx,
        {
            // Basic test involving no values.
            {{}, Value(std::vector<Value>{})},
            // Basic test: testing 1 value.
            {{Value(3)}, Value(std::vector<Value>{Value(3)})},
            // Basic test involving 2 values.
            {{Value(3), Value(4)}, Value(std::vector<Value>{Value(3)})},

            // Test that processes more than 'n' total values.
            {{Value(4), Value(5), Value(6), Value(3), Value(2), Value(1)},
             Value(std::vector<Value>{Value(4)})},
        },
        false, /* skipMerging */
        n);
}

TEST(Accumulators, Last) {
    auto expCtx = ExpressionContextForTest{};
    assertExpectedResults<AccumulatorLast>(
        &expCtx,
        {// No documents evaluated.
         {{}, Value()},

         // The accumulator evaluates one document and retains its value.
         {{Value(5)}, Value(5)},
         // The accumulator evaluates one document with the field missing, returns missing value.
         {{Value()}, Value()},

         // The accumulator evaluates two documents and retains the value in the last.
         {{Value(5), Value(7)}, Value(7)},
         // The accumulator evaluates two documents and retains the missing value in the last.
         {{Value(7), Value()}, Value()}});
}

TEST(Accumulators, LastN) {
    auto expCtx = ExpressionContextForTest{};
    auto n = Value(2);
    assertExpectedResults<AccumulatorLastN>(
        &expCtx,
        {
            // Basic test involving no values.
            {{}, Value(std::vector<Value>{})},
            // Basic test: testing 1 value.
            {{Value(3)}, Value(std::vector<Value>{Value(3)})},
            // Basic test involving 2 values.
            {{Value(3), Value(4)}, Value(std::vector<Value>{Value(3), Value(4)})},

            // Test that processes more than 'n' total values.
            {{Value(4), Value(5), Value(6), Value(3), Value(2), Value(1)},
             Value(std::vector<Value>{Value(2), Value(1)})},

            // Null and missing values should NOT be ignored (missing gets upconverted to null).
            {{Value(), Value(BSONNULL), Value(4), Value(), Value(BSONNULL), Value(5), Value(6)},
             Value(std::vector<Value>{Value(5), Value(6)})},

            {{Value(),
              Value(BSONNULL),
              Value(),
              Value(),
              Value(),
              Value(BSONNULL),
              Value(BSONNULL)},
             Value(std::vector<Value>{Value(BSONNULL), Value(BSONNULL)})},

            {{Value(),
              Value(BSONNULL),
              Value(BSONNULL),
              Value(BSONNULL),
              Value(),
              Value(),
              Value()},
             Value(std::vector<Value>{Value(BSONNULL), Value(BSONNULL)})},

            // Testing mixed types.
            {{Value(4), Value("str"_sd), Value(3.2), Value(4.0)},
             Value(std::vector<Value>{Value(3.2), Value(4.0)})},

            // Testing duplicate values.
            {{Value("std"_sd), Value("std"_sd), Value("test"_sd)},
             Value(std::vector<Value>{Value("std"_sd), Value("test"_sd)})},

            {{Value(9.1), Value(4.22), Value(4.22)},
             Value(std::vector<Value>{Value(4.22), Value(4.22)})},
        },
        false, /* skipMerging */
        n);

    // Additional test partition where N = 1.
    n = Value(1);
    assertExpectedResults<AccumulatorLastN>(
        &expCtx,
        {
            // Basic test involving no values.
            {{}, Value(std::vector<Value>{})},
            // Basic test: testing 1 value.
            {{Value(3)}, Value(std::vector<Value>{Value(3)})},
            // Basic test involving 2 values.
            {{Value(3), Value(4)}, Value(std::vector<Value>{Value(4)})},

            // Test that processes more than 'n' total values.
            {{Value(4), Value(5), Value(6), Value(3), Value(2), Value(1)},
             Value(std::vector<Value>{Value(1)})},
        },
        false, /* skipMerging */
        n);
}

TEST(Accumulators, Min) {
    auto expCtx = ExpressionContextForTest{};
    assertExpectedResults<AccumulatorMin>(
        &expCtx,
        {// No documents evaluated.
         {{}, Value(BSONNULL)},

         // The accumulator evaluates one document and retains its value.
         {{Value(5)}, Value(5)},
         // The accumulator evaluates one document with the field missing and returns null.
         {{Value()}, Value(BSONNULL)},

         // The accumulator evaluates two documents and retains the minimum value.
         {{Value(5), Value(7)}, Value(5)},
         // The accumulator evaluates two documents and ignores the missing value.
         {{Value(7), Value()}, Value(7)}});
}

TEST(Accumulators, MinRespectsCollation) {
    auto expCtx = ExpressionContextForTest{};
    auto collator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kReverseString);
    expCtx.setCollator(std::move(collator));
    assertExpectedResults<AccumulatorMin>(&expCtx,
                                          {{{Value("abc"_sd), Value("cba"_sd)}, Value("cba"_sd)}});
}

TEST(Accumulators, MinN) {
    auto expCtx = ExpressionContextForTest{};
    const auto n = Value(3);
    assertExpectedResults<AccumulatorMinN>(
        &expCtx,
        {
            // Basic tests.
            {{Value(3), Value(4), Value(5), Value(100)},
             {Value(std::vector<Value>{Value(3), Value(4), Value(5)})}},
            {{Value(10), Value(8), Value(9), Value(7), Value(1)},
             {Value(std::vector<Value>{Value(1), Value(7), Value(8)})}},
            {{Value(11.32), Value(91.0), Value(2), Value(701), Value(101)},
             {Value(std::vector<Value>{Value(2), Value(11.32), Value(91.0)})}},

            // 3 or fewer values results in those values being returned.
            {{Value(10), Value(8), Value(9)},
             {Value(std::vector<Value>{Value(8), Value(9), Value(10)})}},
            {{Value(10)}, {Value(std::vector<Value>{Value(10)})}},

            // Ties are broken arbitrarily.
            {{Value(10), Value(10), Value(1), Value(10), Value(1), Value(10)},
             {Value(std::vector<Value>{Value(1), Value(1), Value(10)})}},

            // Null/missing cases (missing and null both get ignored).
            {{Value(100), Value(BSONNULL), Value(), Value(4), Value(3)},
             {Value(std::vector<Value>{Value(3), Value(4), Value(100)})}},
            {{Value(100), Value(), Value(BSONNULL), Value(), Value(3)},
             {Value(std::vector<Value>{Value(3), Value(100)})}},
        },
        false /*skipMerging*/,
        n);
}

TEST(Accumulators, MinNRespectsCollation) {
    auto expCtx = ExpressionContextForTest{};
    auto collator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kReverseString);
    expCtx.setCollator(std::move(collator));
    const auto n = Value(2);
    assertExpectedResults<AccumulatorMinN>(
        &expCtx,
        {{{Value("abc"_sd), Value("cba"_sd), Value("cca"_sd)},
          Value(std::vector<Value>{Value("cba"_sd), Value("cca"_sd)})}},
        false /* skipMerging */,
        n);
}

TEST(Accumulators, MaxN) {
    auto expCtx = ExpressionContextForTest{};
    const auto n = Value(3);
    assertExpectedResults<AccumulatorMaxN>(
        &expCtx,
        {
            // Basic tests.
            {{Value(3), Value(4), Value(5), Value(100)},
             {Value(std::vector<Value>{Value(100), Value(5), Value(4)})}},
            {{Value(10), Value(8), Value(9), Value(7), Value(1)},
             {Value(std::vector<Value>{Value(10), Value(9), Value(8)})}},
            {{Value(11.32), Value(91.0), Value(2), Value(701), Value(101)},
             {Value(std::vector<Value>{Value(701), Value(101), Value(91.0)})}},

            // 3 or fewer values results in those values being returned.
            {{Value(10), Value(8), Value(9)},
             {Value(std::vector<Value>{Value(10), Value(9), Value(8)})}},
            {{Value(10)}, {Value(std::vector<Value>{Value(10)})}},

            // Ties are broken arbitrarily.
            {{Value(1), Value(1), Value(1), Value(10), Value(1), Value(10)},
             {Value(std::vector<Value>{Value(10), Value(10), Value(1)})}},

            // Null/missing cases (missing and null both get ignored).
            {{Value(100), Value(BSONNULL), Value(), Value(4), Value(3)},
             {Value(std::vector<Value>{Value(100), Value(4), Value(3)})}},
            {{Value(100), Value(), Value(BSONNULL), Value(), Value(3)},
             {Value(std::vector<Value>{Value(100), Value(3)})}},
        },
        false /*skipMerging*/,
        n);
}

TEST(Accumulators, MaxNRespectsCollation) {
    auto expCtx = ExpressionContextForTest{};
    auto collator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kReverseString);
    expCtx.setCollator(std::move(collator));
    const auto n = Value(2);
    assertExpectedResults<AccumulatorMaxN>(
        &expCtx,
        {{{Value("abc"_sd), Value("cba"_sd), Value("cca"_sd)},
          Value(std::vector<Value>{Value("abc"_sd), Value("cca"_sd)})}},
        false /* skipMerging */,
        n);
}

TEST(Accumulators, Max) {
    auto expCtx = ExpressionContextForTest{};
    assertExpectedResults<AccumulatorMax>(
        &expCtx,
        {// No documents evaluated.
         {{}, Value(BSONNULL)},

         // The accumulator evaluates one document and retains its value.
         {{Value(5)}, Value(5)},
         // The accumulator evaluates one document with the field missing and returns null.
         {{Value()}, Value(BSONNULL)},

         // The accumulator evaluates two documents and retains the maximum value.
         {{Value(5), Value(7)}, Value(7)},
         // The accumulator evaluates two documents and ignores the missing value.
         {{Value(7), Value()}, Value(7)}});
}

TEST(Accumulators, MaxRespectsCollation) {
    auto expCtx = ExpressionContextForTest{};
    auto collator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kReverseString);
    expCtx.setCollator(std::move(collator));
    assertExpectedResults<AccumulatorMax>(&expCtx,
                                          {{{Value("abc"_sd), Value("cba"_sd)}, Value("abc"_sd)}});
}

TEST(Accumulators, Sum) {
    auto expCtx = ExpressionContextForTest{};
    assertExpectedResults<AccumulatorSum>(
        &expCtx,
        {// No documents evaluated.
         {{}, Value(0)},

         // An int.
         {{Value(10)}, Value(10)},
         // A long.
         {{Value(10LL)}, Value(10LL)},
         // A double.
         {{Value(10.0)}, Value(10.0)},

         // A long that cannot be expressed as an int.
         {{Value(60000000000LL)}, Value(60000000000LL)},
         // A non integer valued double.
         {{Value(7.5)}, Value(7.5)},
         // A nan double.
         {{Value(numeric_limits<double>::quiet_NaN())}, Value(numeric_limits<double>::quiet_NaN())},

         // Two ints are summed.
         {{Value(4), Value(5)}, Value(9)},
         // An int and a long.
         {{Value(4), Value(5LL)}, Value(9LL)},
         // Two longs.
         {{Value(4LL), Value(5LL)}, Value(9LL)},
         // An int and a double.
         {{Value(4), Value(5.5)}, Value(9.5)},
         // A long and a double.
         {{Value(4LL), Value(5.5)}, Value(9.5)},
         // Two doubles.
         {{Value(2.5), Value(5.5)}, Value(8.0)},
         // An int, a long, and a double.
         {{Value(5), Value(99LL), Value(0.2)}, Value(104.2)},
         // Two decimals.
         {{Value(Decimal128("-10.100")), Value(Decimal128("20.200"))}, Value(Decimal128("10.100"))},
         // Two longs and a decimal.
         {{Value(10LL), Value(10LL), Value(Decimal128("10.000"))}, Value(Decimal128("30.000"))},
         // A double and a decimal.
         {{Value(2.5), Value(Decimal128("2.5"))}, Value(Decimal128("5.0"))},
         // An int, long, double and decimal.
         {{Value(10), Value(10LL), Value(10.5), Value(Decimal128("9.6"))},
          Value(Decimal128("40.1"))},

         // A negative value is summed.
         {{Value(5), Value(-8.5)}, Value(-3.5)},
         // A long and a negative int are summed.
         {{Value(5LL), Value(-6)}, Value(-1LL)},

         // Two ints do not overflow.
         {{Value(numeric_limits<int>::max()), Value(10)}, Value(numeric_limits<int>::max() + 10LL)},
         // Two negative ints do not overflow.
         {{Value(-numeric_limits<int>::max()), Value(-10)},
          Value(-numeric_limits<int>::max() - 10LL)},
         // An int and a long do not trigger an int overflow.
         {{Value(numeric_limits<int>::max()), Value(1LL)},
          Value(static_cast<long long>(numeric_limits<int>::max()) + 1)},
         // An int and a double do not trigger an int overflow.
         {{Value(numeric_limits<int>::max()), Value(1.0)},
          Value(static_cast<long long>(numeric_limits<int>::max()) + 1.0)},
         // An int and a long overflow into a double.
         {{Value(1), Value(numeric_limits<long long>::max())},
          Value(-static_cast<double>(numeric_limits<long long>::min()))},
         // Two longs overflow into a double.
         {{Value(numeric_limits<long long>::max()), Value(numeric_limits<long long>::max())},
          Value(static_cast<double>(numeric_limits<long long>::max()) * 2)},
         // A long and a double do not trigger a long overflow.
         {{Value(numeric_limits<long long>::max()), Value(1.0)},
          Value(numeric_limits<long long>::max() + 1.0)},
         // Two doubles overflow to infinity.
         {{Value(numeric_limits<double>::max()), Value(numeric_limits<double>::max())},
          Value(numeric_limits<double>::infinity())},
         // Two large integers do not overflow if a double is added later.
         {{Value(numeric_limits<long long>::max()),
           Value(numeric_limits<long long>::max()),
           Value(1.0)},
          Value(static_cast<double>(numeric_limits<long long>::max()) +
                static_cast<double>(numeric_limits<long long>::max()))},

         // An int and a NaN double.
         {{Value(4), Value(numeric_limits<double>::quiet_NaN())},
          Value(numeric_limits<double>::quiet_NaN())},
         // Null values are ignored.
         {{Value(5), Value(BSONNULL)}, Value(5)},
         // Missing values are ignored.
         {{Value(9), Value()}, Value(9)}});
}

TEST(Accumulators, TopBottomNRespectsCollation) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    auto collator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kReverseString);
    expCtx->setCollator(std::move(collator));
    const auto n = Value(2);
    auto mkdoc = [](Value a) {
        return Value(BSON(AccumulatorN::kFieldNameOutput << a << AccumulatorN::kFieldNameSortFields
                                                         << BSON_ARRAY(a)));
    };

    OperationsType bottomCasesAscending{
        {{mkdoc(Value("abc"_sd)), mkdoc(Value("cba"_sd)), mkdoc(Value("cca"_sd))},
         Value(std::vector<Value>{Value("cca"_sd), Value("abc"_sd)})}};

    assertExpectedResults(expCtx.get(),
                          bottomCasesAscending,
                          [&](ExpressionContext* const expCtx) -> intrusive_ptr<AccumulatorState> {
                              auto acc =
                                  AccumulatorTopBottomN<TopBottomSense::kBottom, false>::create(
                                      expCtx, BSON("a" << 1));
                              acc->startNewGroup(n);
                              return acc;
                          });

    OperationsType bottomCasesDescending{
        {{mkdoc(Value("abc"_sd)), mkdoc(Value("cba"_sd)), mkdoc(Value("cca"_sd))},
         Value(std::vector<Value>{Value("cca"_sd), Value("cba"_sd)})}};
    assertExpectedResults(expCtx.get(),
                          bottomCasesDescending,
                          [&](ExpressionContext* const expCtx) -> intrusive_ptr<AccumulatorState> {
                              auto acc =
                                  AccumulatorTopBottomN<TopBottomSense::kBottom, false>::create(
                                      expCtx, BSON("a" << -1));
                              acc->startNewGroup(n);
                              return acc;
                          });

    OperationsType topCasesAscending{
        {{mkdoc(Value("abc"_sd)), mkdoc(Value("cba"_sd)), mkdoc(Value("cca"_sd))},
         Value(std::vector<Value>{Value("cba"_sd), Value("cca"_sd)})}};
    assertExpectedResults(expCtx.get(),
                          topCasesAscending,
                          [&](ExpressionContext* const expCtx) -> intrusive_ptr<AccumulatorState> {
                              auto acc = AccumulatorTopBottomN<TopBottomSense::kTop, false>::create(
                                  expCtx, BSON("a" << 1));
                              acc->startNewGroup(n);
                              return acc;
                          });

    OperationsType topCasesDescending{
        {{mkdoc(Value("abc"_sd)), mkdoc(Value("cba"_sd)), mkdoc(Value("cca"_sd))},
         Value(std::vector<Value>{Value("abc"_sd), Value("cca"_sd)})}};
    assertExpectedResults(expCtx.get(),
                          topCasesDescending,
                          [&](ExpressionContext* const expCtx) -> intrusive_ptr<AccumulatorState> {
                              auto acc = AccumulatorTopBottomN<TopBottomSense::kTop, false>::create(
                                  expCtx, BSON("a" << -1));
                              acc->startNewGroup(n);
                              return acc;
                          });
}

TEST(Accumulators, TopNDescendingBottomNAscending) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    const auto n3 = Value(3);
    const auto n1 = Value(1);
    auto mkdoc = [](Value a) {
        return Value(BSON(AccumulatorN::kFieldNameOutput << a << AccumulatorN::kFieldNameSortFields
                                                         << BSON_ARRAY(a)));
    };
    auto mkdoc2 = [](int a, Value b) {
        return Value(BSON(AccumulatorN::kFieldNameOutput << b << AccumulatorN::kFieldNameSortFields
                                                         << BSON_ARRAY(a)));
    };
    OperationsType cases{
        // Basic tests.
        {{mkdoc(Value(3)), mkdoc(Value(4)), mkdoc(Value(5)), mkdoc(Value(100))},
         {Value(std::vector<Value>{Value(4), Value(5), Value(100)})}},
        {{mkdoc(Value(10)), mkdoc(Value(8)), mkdoc(Value(9)), mkdoc(Value(7)), mkdoc(Value(1))},
         {Value(std::vector<Value>{Value(8), Value(9), Value(10)})}},
        {{mkdoc(Value(11.32)),
          mkdoc(Value(91.0)),
          mkdoc(Value(2)),
          mkdoc(Value(701)),
          mkdoc(Value(101))},
         {Value(std::vector<Value>{Value(91.0), Value(101), Value(701)})}},

        // 3 or fewer values results in those values being returned.
        {{mkdoc(Value(10)), mkdoc(Value(8)), mkdoc(Value(9))},
         {Value(std::vector<Value>{Value(8), Value(9), Value(10)})}},
        {{mkdoc(Value(10))}, {Value(std::vector<Value>{Value(10)})}},

        // Ties are broken arbitrarily.
        {{mkdoc(Value(10)),
          mkdoc(Value(1)),
          mkdoc(Value(1)),
          mkdoc(Value(1)),
          mkdoc(Value(1)),
          mkdoc(Value(10))},
         {Value(std::vector<Value>{Value(1), Value(10), Value(10)})}},

        // Null/missing cases (missing and null are NOT ignored, but missing is converted to null).
        {{mkdoc(Value(BSONNULL)), mkdoc(Value()), mkdoc(Value(BSONNULL)), mkdoc(Value(3))},
         {Value(std::vector<Value>{Value(BSONNULL), Value(BSONNULL), Value(3)})}},

        {{mkdoc(Value()), mkdoc(Value(BSONNULL)), mkdoc(Value()), mkdoc(Value(3))},
         {Value(std::vector<Value>{Value(BSONNULL), Value(BSONNULL), Value(3)})}},

        // Output values different than sortBy.
        {{mkdoc2(5, Value(7)), mkdoc2(4, Value(2)), mkdoc2(3, Value(3)), mkdoc2(1, Value(3))},
         {Value(std::vector<Value>{Value(3), Value(2), Value(7)})}},
        {{mkdoc2(5, Value(BSONNULL)), mkdoc2(4, Value()), mkdoc2(3, Value(3))},
         {Value(std::vector<Value>{Value(3), Value(BSONNULL), Value(BSONNULL)})}}};

    OperationsType bottomSpecificCases = {
        // All 10s encountered once map is full.
        {{mkdoc2(1, Value(1)),
          mkdoc2(1, Value(2)),
          mkdoc2(10, Value(3)),
          mkdoc2(10, Value(4)),
          mkdoc2(10, Value(5))},
         {Value(std::vector<Value>{Value(3), Value(4), Value(5)})}},

        // All 10s encountered before map is full.
        {{mkdoc2(10, Value(1)),
          mkdoc2(10, Value(2)),
          mkdoc2(1, Value(3)),
          mkdoc2(1, Value(4)),
          mkdoc2(1, Value(5))},
         {Value(std::vector<Value>{Value(3), Value(1), Value(2)})}},

        // All 10s encountered when the map is full.
        {{mkdoc2(10, Value(3)),
          mkdoc2(10, Value(4)),
          mkdoc2(10, Value(5)),
          mkdoc2(1, Value(1)),
          mkdoc2(1, Value(2))},
         {Value(std::vector<Value>{Value(3), Value(4), Value(5)})}}};

    try {
        auto accumInit = [&](ExpressionContext* const expCtx) -> intrusive_ptr<AccumulatorState> {
            auto acc = AccumulatorTopBottomN<TopBottomSense::kBottom, false>::create(
                expCtx, BSON("a" << 1));
            acc->startNewGroup(n3);
            return acc;
        };
        assertExpectedResults(expCtx.get(), cases, accumInit);
        assertExpectedResults(expCtx.get(), bottomSpecificCases, accumInit);

    } catch (...) {
        LOGV2(5788006, "bottom3 a: 1");
        throw;
    }

    // topN descending will return same results, but in reverse order.
    for (auto& [input, expected] : cases) {
        tassert(6078100, "expected should be an array", expected.isArray());
        auto arr = expected.getArray();
        std::reverse(std::begin(arr), std::end(arr));
        expected = Value(arr);
    }

    OperationsType topSpecificCases = {// All 10s encountered once map is full.
                                       {{mkdoc2(1, Value(1)),
                                         mkdoc2(1, Value(2)),
                                         mkdoc2(10, Value(3)),
                                         mkdoc2(10, Value(4)),
                                         mkdoc2(10, Value(5))},
                                        {Value(std::vector<Value>{Value(3), Value(4), Value(5)})}},

                                       // All 10s encountered before map is full.
                                       {{mkdoc2(10, Value(1)),
                                         mkdoc2(10, Value(2)),
                                         mkdoc2(1, Value(3)),
                                         mkdoc2(1, Value(4)),
                                         mkdoc2(1, Value(5))},
                                        {Value(std::vector<Value>{Value(1), Value(2), Value(3)})}},

                                       // All 10s encountered when the map is full.
                                       {{mkdoc2(10, Value(3)),
                                         mkdoc2(10, Value(4)),
                                         mkdoc2(10, Value(5)),
                                         mkdoc2(1, Value(1)),
                                         mkdoc2(1, Value(2))},
                                        {Value(std::vector<Value>{Value(3), Value(4), Value(5)})}}};
    try {
        auto accInit = [&](ExpressionContext* const expCtx) -> intrusive_ptr<AccumulatorState> {
            auto acc =
                AccumulatorTopBottomN<TopBottomSense::kTop, false>::create(expCtx, BSON("a" << -1));
            acc->startNewGroup(n3);
            return acc;
        };
        assertExpectedResults(expCtx.get(), cases, accInit);
        assertExpectedResults(expCtx.get(), topSpecificCases, accInit);
    } catch (...) {
        LOGV2(5788007, "top3 a: -1");
        throw;
    }
}

TEST(Accumulators, TopNAscendingBottomNDescending) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    const auto n3 = Value(3);
    const auto n1 = Value(1);
    auto mkdoc = [](Value a) {
        return Value(BSON(AccumulatorN::kFieldNameOutput << a << AccumulatorN::kFieldNameSortFields
                                                         << BSON_ARRAY(a)));
    };
    auto mkdoc2 = [](int a, Value b) {
        return Value(BSON(AccumulatorN::kFieldNameOutput << b << AccumulatorN::kFieldNameSortFields
                                                         << BSON_ARRAY(a)));
    };
    OperationsType cases{
        // Basic tests.
        {{mkdoc(Value(3)), mkdoc(Value(4)), mkdoc(Value(5)), mkdoc(Value(100))},
         {Value(std::vector<Value>{Value(5), Value(4), Value(3)})}},
        {{mkdoc(Value(10)), mkdoc(Value(8)), mkdoc(Value(9)), mkdoc(Value(7)), mkdoc(Value(1))},
         {Value(std::vector<Value>{Value(8), Value(7), Value(1)})}},
        {{mkdoc(Value(11.32)),
          mkdoc(Value(91.0)),
          mkdoc(Value(2)),
          mkdoc(Value(701)),
          mkdoc(Value(101))},
         {Value(std::vector<Value>{Value(91.0), Value(11.32), Value(2)})}},

        // 3 or fewer values results in those values being returned.
        {{mkdoc(Value(10)), mkdoc(Value(8)), mkdoc(Value(9))},
         {Value(std::vector<Value>{Value(10), Value(9), Value(8)})}},
        {{mkdoc(Value(10))}, {Value(std::vector<Value>{Value(10)})}},

        // Ties are broken arbitrarily.
        {{mkdoc(Value(10)),
          mkdoc(Value(10)),
          mkdoc(Value(1)),
          mkdoc(Value(10)),
          mkdoc(Value(1)),
          mkdoc(Value(10))},
         {Value(std::vector<Value>{Value(10), Value(1), Value(1)})}},

        // Null/missing cases (missing and null are NOT ignored, but missing is upconverted to
        // null).
        {{mkdoc(Value(100)),
          mkdoc(Value(BSONNULL)),
          mkdoc(Value()),
          mkdoc(Value(BSONNULL)),
          mkdoc(Value())},
         {Value(std::vector<Value>{Value(BSONNULL), Value(BSONNULL), Value(BSONNULL)})}},
        {{mkdoc(Value(100)),
          mkdoc(Value()),
          mkdoc(Value(BSONNULL)),
          mkdoc(Value()),
          mkdoc(Value())},
         {Value(std::vector<Value>{Value(BSONNULL), Value(BSONNULL), Value(BSONNULL)})}},

        // Output values different than sortBy.
        {{mkdoc2(5, Value(7)), mkdoc2(6, Value(5)), mkdoc2(4, Value(2)), mkdoc2(3, Value(3))},
         {Value(std::vector<Value>{Value(7), Value(2), Value(3)})}},
        {{mkdoc2(5, Value(BSONNULL)), mkdoc2(4, Value()), mkdoc2(3, Value(3))},
         {Value(std::vector<Value>{Value(BSONNULL), Value(BSONNULL), Value(3)})}}};

    OperationsType bottomSpecificCases = {
        // One 1 encountered once map is full.
        {{mkdoc2(1, Value(1)),
          mkdoc2(10, Value(3)),
          mkdoc2(10, Value(4)),
          mkdoc2(1, Value(2)),
          mkdoc2(10, Value(5))},
         {Value(std::vector<Value>{Value(4), Value(1), Value(2)})}},

        // All 1s encountered before map is full.
        {{mkdoc2(1, Value(1)),
          mkdoc2(1, Value(2)),
          mkdoc2(10, Value(3)),
          mkdoc2(10, Value(4)),
          mkdoc2(10, Value(5))},
         {Value(std::vector<Value>{Value(3), Value(1), Value(2)})}},

        // All 1s encountered when the map is full.
        {{mkdoc2(10, Value(3)),
          mkdoc2(10, Value(4)),
          mkdoc2(10, Value(5)),
          mkdoc2(1, Value(1)),
          mkdoc2(1, Value(2))},
         {Value(std::vector<Value>{Value(5), Value(1), Value(2)})}}};

    try {
        auto accInit = [&](ExpressionContext* const expCtx) -> intrusive_ptr<AccumulatorState> {
            auto acc = AccumulatorTopBottomN<TopBottomSense::kBottom, false>::create(
                expCtx, BSON("a" << -1));
            acc->startNewGroup(n3);
            return acc;
        };
        assertExpectedResults(expCtx.get(), cases, accInit);
        assertExpectedResults(expCtx.get(), bottomSpecificCases, accInit);
    } catch (...) {
        LOGV2(5788008, "bottom3 a: -1");
        throw;
    }

    // topN ascending will return same results, but in reverse order.
    for (auto& [input, expected] : cases) {
        tassert(6078101, "expected should be an array", expected.isArray());
        auto arr = expected.getArray();
        std::reverse(std::begin(arr), std::end(arr));
        expected = Value(arr);
    }

    OperationsType topSpecifcCases{// One 10 encountered once map is full.
                                   {{mkdoc2(1, Value(1)),
                                     mkdoc2(10, Value(3)),
                                     mkdoc2(10, Value(4)),
                                     mkdoc2(1, Value(2)),
                                     mkdoc2(10, Value(5))},
                                    {Value(std::vector<Value>{Value(1), Value(2), Value(3)})}},

                                   // All 10s encountered before map is full.
                                   {{mkdoc2(1, Value(1)),
                                     mkdoc2(1, Value(2)),
                                     mkdoc2(10, Value(3)),
                                     mkdoc2(10, Value(4)),
                                     mkdoc2(10, Value(5))},
                                    {Value(std::vector<Value>{Value(1), Value(2), Value(3)})}},

                                   // All 10s encountered when the map is full.
                                   {{mkdoc2(1, Value(3)),
                                     mkdoc2(1, Value(4)),
                                     mkdoc2(1, Value(5)),
                                     mkdoc2(10, Value(1)),
                                     mkdoc2(10, Value(2))},
                                    {Value(std::vector<Value>{Value(3), Value(4), Value(5)})}}};

    try {
        auto accInit = [&](ExpressionContext* const expCtx) -> intrusive_ptr<AccumulatorState> {
            auto acc =
                AccumulatorTopBottomN<TopBottomSense::kTop, false>::create(expCtx, BSON("a" << 1));
            acc->startNewGroup(n3);
            return acc;
        };
        assertExpectedResults(expCtx.get(), cases, accInit);
        assertExpectedResults(expCtx.get(), topSpecifcCases, accInit);
    } catch (...) {
        LOGV2(5788009, "top3 a: 1");
        throw;
    }
}

// Utility to test the single counterparts of the topN/bottomN accumulators.
template <TopBottomSense s>
void testSingle(OperationsType cases, ExpressionContext* const expCtx, const BSONObj& sortPattern) {
    // Unpack for single versions.
    for (auto& [input, expected] : cases) {
        expected = Value(expected.getArray()[0]);
    }
    try {
        // n = 1 single = true should return 1 non array value.
        assertExpectedResults(
            expCtx, cases, [&](ExpressionContext* const expCtx) -> intrusive_ptr<AccumulatorState> {
                auto acc = AccumulatorTopBottomN<s, true>::create(expCtx, sortPattern);
                acc->startNewGroup(Value(1));
                return acc;
            });
    } catch (...) {
        if constexpr (s == kTop) {
            LOGV2(5788013, "top single", "sortPattern"_attr = sortPattern);
        } else {
            LOGV2(5788016, "bottom single", "sortPattern"_attr = sortPattern);
        }
        throw;
    }
}

TEST(Accumulators, TopBottomSingle) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    const auto n = Value(1);
    auto mkdoc = [](Value a) {
        return Value(BSON(AccumulatorN::kFieldNameOutput << a << AccumulatorN::kFieldNameSortFields
                                                         << BSON_ARRAY(a)));
    };

    const BSONObj ascSort = BSON("a" << 1);
    const BSONObj descSort = BSON("a" << -1);

    // When n = 1, bottomN over ascending sort is the same as topN over descending sort.
    OperationsType bottomAscTopDescCases{
        {{mkdoc(Value(3)), mkdoc(Value(4))}, {Value(std::vector<Value>{Value(4)})}},
        {{mkdoc(Value(4)), mkdoc(Value(3))}, {Value(std::vector<Value>{Value(4)})}},
        {{mkdoc(Value(BSONNULL)), mkdoc(Value(4))}, {Value(std::vector<Value>{Value(4)})}},

        {{mkdoc(Value()), mkdoc(Value(4))}, {Value(std::vector<Value>{Value(4)})}},
        {{mkdoc(Value(BSONUndefined)), mkdoc(Value(4))}, {Value(std::vector<Value>{Value(4)})}}};

    // n = 1 single = false should return a 1 elem array.
    try {
        assertExpectedResults(
            expCtx.get(),
            bottomAscTopDescCases,
            [&](ExpressionContext* const expCtx) -> intrusive_ptr<AccumulatorState> {
                auto acc =
                    AccumulatorTopBottomN<TopBottomSense::kBottom, false>::create(expCtx, ascSort);
                acc->startNewGroup(n);
                return acc;
            });
    } catch (...) {
        LOGV2(5788010, "bottom1 a: 1");
        throw;
    }
    testSingle<TopBottomSense::kBottom>(bottomAscTopDescCases, expCtx.get(), ascSort);

    try {
        assertExpectedResults(
            expCtx.get(),
            bottomAscTopDescCases,
            [&](ExpressionContext* const expCtx) -> intrusive_ptr<AccumulatorState> {
                auto acc = AccumulatorTopBottomN<TopBottomSense::kTop, false>::create(
                    expCtx, BSON("a" << -1));
                acc->startNewGroup(n);
                return acc;
            });
    } catch (...) {
        LOGV2(5788011, "top1 a: -1");
        throw;
    }
    testSingle<TopBottomSense::kTop>(bottomAscTopDescCases, expCtx.get(), descSort);

    // When n = 1, bottomN over descending sort is the same as topN over ascending sort.
    OperationsType bottomDescTopAscCases{
        {{mkdoc(Value(3)), mkdoc(Value(4))}, {Value(std::vector<Value>{Value(3)})}},
        {{mkdoc(Value(4)), mkdoc(Value(3))}, {Value(std::vector<Value>{Value(3)})}},
        {{mkdoc(Value(BSONNULL)), mkdoc(Value(4))}, {Value(std::vector<Value>{Value(BSONNULL)})}},

        {{mkdoc(Value()), mkdoc(Value(4))}, {Value(std::vector<Value>{Value(BSONNULL)})}},
        {{mkdoc(Value(BSONUndefined)), mkdoc(Value(4))},
         {Value(std::vector<Value>{Value(BSONUndefined)})}}};

    // n = 1 single = false should return a 1 elem array.
    try {
        assertExpectedResults(
            expCtx.get(),
            bottomDescTopAscCases,
            [&](ExpressionContext* const expCtx) -> intrusive_ptr<AccumulatorState> {
                auto acc = AccumulatorTopBottomN<TopBottomSense::kBottom, false>::create(
                    expCtx, BSON("a" << -1));
                acc->startNewGroup(n);
                return acc;
            });
    } catch (...) {
        LOGV2(5788012, "bottom1 a: -1");
        throw;
    }
    testSingle<TopBottomSense::kBottom>(bottomDescTopAscCases, expCtx.get(), descSort);

    // n = 1 single = false should return a 1 elem array.
    try {
        assertExpectedResults(
            expCtx.get(),
            bottomDescTopAscCases,
            [&](ExpressionContext* const expCtx) -> intrusive_ptr<AccumulatorState> {
                auto acc = AccumulatorTopBottomN<TopBottomSense::kTop, false>::create(
                    expCtx, BSON("a" << 1));
                acc->startNewGroup(n);
                return acc;
            });
    } catch (...) {
        LOGV2(6078102, "top a: 1");
        throw;
    }
    testSingle<TopBottomSense::kTop>(bottomDescTopAscCases, expCtx.get(), ascSort);
}

template <typename T>
struct TopBottomNRemoveTest : public AggregationContextFixture {
    void init(boost::optional<int> n, BSONObj sortBy) {
        auto expCtx = getExpCtxRaw();
        expCtx->setCollator(std::make_unique<CollatorInterfaceMock>(
            CollatorInterfaceMock::MockType::kToLowerString));
        auto accState = T::create(expCtx, sortBy, /* isRemovable */ true);
        _acc = boost::dynamic_pointer_cast<T>(accState);
        _acc->startNewGroup(Value(n.value_or(1)));
    }

    template <typename SortKeyType>
    void add(SortKeyType sortKey, int output) {
        auto v =
            Value(BSON(AccumulatorN::kFieldNameOutput
                       << output << AccumulatorN::kFieldNameSortFields << BSON_ARRAY(sortKey)));
        _acc->process(v, false);
        _q.push(v);
    }

    void remove() {
        _acc->remove(_q.front());
        _q.pop();
    }

    void assertExpected(std::vector<Value> vec) {
        ASSERT_VALUE_EQ(Value(vec), _acc->getValue(false));
    }

    void assertExpectedSingle(int expected) {
        ASSERT_VALUE_EQ(Value(expected), _acc->getValue(false));
    }

    void assertNull() {
        ASSERT_VALUE_EQ(Value(BSONNULL), _acc->getValue(false));
    }

    // Assumes init() already called.
    void testNoRemoveUnderflow() {
        auto usageBefore = _acc->getMemUsage();
        auto sortKey =
            BSON("a" << BSONNULL << "b" << BSONNULL << "c" << BSONNULL << "d" << BSONNULL);
        add(sortKey, 0);
        add(sortKey, 0);
        add(sortKey, 0);
        add(sortKey, 0);
        remove();
        remove();
        remove();
        remove();
        auto usageAfter = _acc->getMemUsage();
        ASSERT_EQ(usageBefore, usageAfter);
    }

    intrusive_ptr<T> _acc = nullptr;
    std::queue<Value> _q;
};

using TopNRemoveTest = TopBottomNRemoveTest<AccumulatorTopBottomN<TopBottomSense::kTop, false>>;
TEST_F(TopNRemoveTest, TopNRemove) {
    // Test accumulator {$topN: {n: 3, output: "$output", sortBy: {sortKey: 1}}}
    init(3, BSON("sortKey" << 1));

    // Basic remove test.
    add(1, 1);
    add(2, 2);
    assertExpected({Value(1), Value(2)});

    remove();  // 1, 1
    assertExpected({Value(2)});

    // Add some elements with equal keys.
    add(2, 3);
    add(2, 4);
    add(3, 5);
    add(1, 1);
    assertExpected({Value(1), Value(2), Value(3)});  // 4, 5 not shown.

    remove();                                        // 2, 2
    assertExpected({Value(1), Value(3), Value(4)});  // 5 not shown.

    // Add value back but its at the end of the queue so it's not included in results.
    add(2, 2);
    assertExpected({Value(1), Value(3), Value(4)});  // 2, 5 not shown.

    remove();                                        // 2, 3
    assertExpected({Value(1), Value(4), Value(2)});  // 5 not shown.

    remove();  // 2, 4
    assertExpected({Value(1), Value(2), Value(5)});

    // Add value after and then pop front value.
    add(3, 6);
    assertExpected({Value(1), Value(2), Value(5)});  // 6 not shown.

    remove();  // 3, 5
    assertExpected({Value(1), Value(2), Value(6)});
    remove();  // 1, 1
    assertExpected({Value(2), Value(6)});
    remove();  // 2, 2
    assertExpected({Value(6)});
    remove();  // Cleared.
    assertExpected({});
}

TEST_F(TopNRemoveTest, TopNRemoveRoundRobin) {
    // Test accumulator {$topN: {n: 3, output: "$output", sortBy: {sortKey: 1}}}
    init(3, BSON("sortKey" << 1));

    // Mostly round robin insert and remove.
    add(1, 1);
    add(1, 2);
    add(2, 4);
    add(3, 7);
    add(1, 3);
    add(2, 5);
    add(3, 8);
    add(2, 6);
    add(3, 9);

    // 1: [1, 2, 3], 2: [4, 5, 6], 3: [7, 8, 9]
    assertExpected({Value(1), Value(2), Value(3)});
    remove();
    // 1: [2, 3], 2: [4, 5, 6], 3: [7, 8, 9]
    assertExpected({Value(2), Value(3), Value(4)});
    remove();
    // 1: [3], 2: [4, 5, 6], 3: [7, 8, 9]
    assertExpected({Value(3), Value(4), Value(5)});
    remove();
    // 1: [3], 2: [5, 6], 3: [7, 8, 9]
    assertExpected({Value(3), Value(5), Value(6)});
    remove();
    // 1: [3], 2: [5, 6], 3: [8, 9]
    assertExpected({Value(3), Value(5), Value(6)});
    remove();
    // 2: [5, 6], 3: [8, 9]
    assertExpected({Value(5), Value(6), Value(8)});
    remove();
    // 2: [6], 3: [8, 9]
    assertExpected({Value(6), Value(8), Value(9)});
    remove();
    // 2: [6], 3: [9]
    assertExpected({Value(6), Value(9)});
}

TEST_F(TopNRemoveTest, TopNRemoveCollator) {
    // Test accumulator {$topN: {n: 3, output: "$output", sortBy: {sortKey: 1}}}
    init(3, BSON("sortKey" << 1));

    add("FOO", 1);
    add("foo", 2);
    add("fOo", 3);
    assertExpected({Value(1), Value(2), Value(3)});
    remove();
    assertExpected({Value(2), Value(3)});
    add("foO", 4);
    assertExpected({Value(2), Value(3), Value(4)});
    remove();
    assertExpected({Value(3), Value(4)});
    remove();
    assertExpected({Value(4)});
}

TEST_F(TopNRemoveTest, TopNRemoveNoUnderflow) {
    // Test accumulator {$topN: {n: 3, output: "$output", sortBy: {sortKey: 1}}}
    init(3, BSON("sortKey" << 1));
    testNoRemoveUnderflow();
}

using BottomNRemoveTest =
    TopBottomNRemoveTest<AccumulatorTopBottomN<TopBottomSense::kBottom, false>>;
TEST_F(BottomNRemoveTest, BottomNRemove) {
    // Test accumulator {$bottomN: {n: 3, output: "$output", sortBy: {sortKey: 1}}}
    init(3, BSON("sortKey" << 1));

    // Mostly round robin insert and remove.
    add(3, 1);
    add(3, 2);
    add(2, 4);
    add(1, 7);
    add(3, 3);
    add(2, 5);
    add(1, 8);
    add(2, 6);
    add(1, 9);

    // 1: [7, 8, 9], 2: [4, 5, 6], 3: [1, 2, 3]
    assertExpected({Value(1), Value(2), Value(3)});
    remove();
    // 1: [7, 8, 9], 2: [4, 5, 6], 3: [2, 3]
    assertExpected({Value(6), Value(2), Value(3)});
    remove();
    // 1: [7, 8, 9], 2: [4, 5, 6], 3: [3]
    assertExpected({Value(5), Value(6), Value(3)});
    remove();
    // 1: [7, 8, 9], 2: [5, 6], 3: [3]
    assertExpected({Value(5), Value(6), Value(3)});
    remove();
    // 1: [8, 9], 2: [5, 6], 3: [3]
    assertExpected({Value(5), Value(6), Value(3)});
    remove();
    // 1: [8, 9], 2: [5, 6]
    assertExpected({Value(9), Value(5), Value(6)});
    remove();
    // 1: [8, 9], 2: [6]
    assertExpected({Value(8), Value(9), Value(6)});
    remove();
    // 2: [9], 3: [6]
    assertExpected({Value(9), Value(6)});
    remove();
    // 2: [9]
    assertExpected({Value(9)});
    remove();  // Empty.
    assertExpected({});
}

TEST_F(BottomNRemoveTest, BottomNRemoveDesc) {
    // Test accumulator {$bottomN: {n: 3, output: "$output", sortBy: {sortKey: 1}}}
    init(3, BSON("sortKey" << -1));

    // Mostly round robin insert and remove.
    add(1, 1);
    add(1, 2);
    add(2, 4);
    add(3, 7);
    add(1, 3);
    add(2, 5);
    add(3, 8);
    add(2, 6);
    add(3, 9);

    // 3: [7, 8, 9], 2: [4, 5, 6], 1: [1, 2, 3]
    assertExpected({Value(1), Value(2), Value(3)});
    remove();
    // 3: [7, 8, 9], 2: [4, 5, 6], 1: [2, 3]
    assertExpected({Value(6), Value(2), Value(3)});
    remove();
    // 3: [7, 8, 9], 2: [4, 5, 6], 1: [3]
    assertExpected({Value(5), Value(6), Value(3)});
    remove();
    // 3: [7, 8, 9], 2: [5, 6], 1: [3]
    assertExpected({Value(5), Value(6), Value(3)});
    remove();
    // 3: [8, 9], 2: [5, 6], 1: [3]
    assertExpected({Value(5), Value(6), Value(3)});
    remove();
    // 3: [8, 9], 2: [5, 6]
    assertExpected({Value(9), Value(5), Value(6)});
    remove();
    // 3: [8, 9], 2: [6]
    assertExpected({Value(8), Value(9), Value(6)});
    remove();
    // 2: [9], 3: [6]
    assertExpected({Value(9), Value(6)});
    remove();
    // 2: [9]
    assertExpected({Value(9)});
    remove();  // Empty.
    assertExpected({});
}

TEST_F(BottomNRemoveTest, BottomNAddRemove) {
    // Test accumulator {$bottomN: {n: 3, output: "$output", sortBy: {sortKey: 1}}}
    init(3, BSON("sortKey" << 1));

    assertExpected({});
    add(1, 1);
    assertExpected({Value(1)});
    add(1, 2);
    add(1, 3);
    remove();
    assertExpected({Value(2), Value(3)});
}

TEST_F(BottomNRemoveTest, BottomNRemoveCollator) {
    // Test accumulator {$bottomN: {n: 3, output: "$output", sortBy: {sortKey: 1}}}
    init(3, BSON("sortKey" << 1));

    add("FOO", 1);
    add("foo", 2);
    add("fOo", 3);
    assertExpected({Value(1), Value(2), Value(3)});
    remove();
    assertExpected({Value(2), Value(3)});
    add("foO", 4);
    assertExpected({Value(2), Value(3), Value(4)});
    remove();
    assertExpected({Value(3), Value(4)});
    remove();
    assertExpected({Value(4)});
}

TEST_F(BottomNRemoveTest, BottomNRemoveNoUnderflow) {
    // Test accumulator {$bottomN: {n: 3, output: "$output", sortBy: {sortKey: 1}}}
    init(3, BSON("sortKey" << -1));
    testNoRemoveUnderflow();
}

using TopRemoveTest = TopBottomNRemoveTest<AccumulatorTopBottomN<TopBottomSense::kTop, true>>;
TEST_F(TopRemoveTest, TopRemove) {
    // Test accumulator {$top: {output: "$output", sortBy: {sortKey: 1}}}
    init(boost::none, BSON("sortKey" << 1));

    // Mostly round robin insert and remove.
    add(1, 1);
    add(1, 2);
    add(2, 4);
    add(3, 7);
    add(1, 3);
    add(2, 5);
    add(3, 8);
    add(2, 6);
    add(3, 9);

    // 1: [1, 2, 3], 2: [4, 5, 6], 3: [7, 8, 9]
    assertExpectedSingle(1);
    remove();
    // 1: [2, 3], 2: [4, 5, 6], 3: [7, 8, 9]
    assertExpectedSingle(2);
    remove();
    // 1: [3], 2: [4, 5, 6], 3: [7, 8, 9]
    assertExpectedSingle(3);
    remove();
    // 1: [3], 2: [5, 6], 3: [7, 8, 9]
    assertExpectedSingle(3);
    remove();
    // 1: [3], 2: [5, 6], 3: [8, 9]
    assertExpectedSingle(3);
    remove();
    // 2: [5, 6], 3: [8, 9]
    assertExpectedSingle(5);
    remove();
    // 2: [6], 3: [8, 9]
    assertExpectedSingle(6);
    remove();
    // 2: [6], 3: [9]
    assertExpectedSingle(6);
    remove();
    // 2: [9]
    assertExpectedSingle(9);
    remove();
    assertNull();
}

TEST_F(TopRemoveTest, TopAddRemove) {
    // Test accumulator {$top: {output: "$output", sortBy: {sortKey: 1}}}
    init(boost::none, BSON("sortKey" << 1));

    assertNull();
    add(1, 1);
    assertExpectedSingle(1);
    add(1, 2);
    add(1, 3);
    remove();
    assertExpectedSingle(2);
}

TEST_F(TopRemoveTest, TopRemoveNoUnderflow) {
    // Test accumulator {$top: {output: "$output", sortBy: {sortKey: 1}}}
    init(boost::none, BSON("sortKey" << 1));
    testNoRemoveUnderflow();
}

using BottomRemoveTest = TopBottomNRemoveTest<AccumulatorTopBottomN<TopBottomSense::kBottom, true>>;
TEST_F(BottomRemoveTest, BottomRemove) {
    // Test accumulator {$bottom: {output: "$output", sortBy: {sortKey: 1}}}
    init(boost::none, BSON("sortKey" << 1));

    // Mostly round robin insert and remove.
    add(3, 1);
    add(3, 2);
    add(2, 4);
    add(1, 7);
    add(3, 3);
    add(2, 5);
    add(1, 8);
    add(2, 6);
    add(1, 9);

    // 1: [7, 8, 9], 2: [4, 5, 6], 3: [1, 2, 3]
    assertExpectedSingle(3);
    remove();
    // 1: [7, 8, 9], 2: [4, 5, 6], 3: [2, 3]
    assertExpectedSingle(3);
    remove();
    // 1: [7, 8, 9], 2: [4, 5, 6], 3: [3]
    assertExpectedSingle(3);
    remove();
    // 1: [7, 8, 9], 2: [5, 6], 3: [3]
    assertExpectedSingle(3);
    remove();
    // 1: [8, 9], 2: [5, 6], 3: [3]
    assertExpectedSingle(3);
    remove();
    // 1: [8, 9], 2: [5, 6]
    assertExpectedSingle(6);
    remove();
    // 1: [8, 9], 2: [6]
    assertExpectedSingle(6);
    remove();
    // 2: [9], 3: [6]
    assertExpectedSingle(6);
    remove();
    // 2: [9]
    assertExpectedSingle(9);
    remove();  // Empty.
    assertNull();
}

TEST_F(BottomRemoveTest, BottomAddRemove) {
    // Test accumulator {$bottom: {output: "$output", sortBy: {sortKey: 1}}}
    init(boost::none, BSON("sortKey" << 1));

    // Concurrent add/delete.
    add(1, 1);
    assertExpectedSingle(1);
    add(1, 2);
    add(1, 3);
    remove();
    assertExpectedSingle(3);
}

TEST_F(BottomRemoveTest, BottomRemoveNoUnderflow) {
    // Test accumulator {$bottom: {output: "$output", sortBy: {sortKey: 1}}}
    init(boost::none, BSON("sortKey" << 1));
    testNoRemoveUnderflow();
}

TEST(Accumulators, Rank) {
    auto expCtx = ExpressionContextForTest{};
    assertExpectedResults<AccumulatorRank>(
        &expCtx,
        {
            // Document number is correct.
            {{Value(0)}, Value(1)},
            {{Value(0), Value(2)}, Value(2)},
            {{Value(0), Value(2), Value(4)}, Value(3)},
            // Ties don't increment
            {{Value(1), Value(1)}, Value(1)},
            // Ties skip next value correctly.
            {{Value(1), Value(1), Value(3)}, Value(3)},
            {{Value(1), Value(1), Value(1), Value(3)}, Value(4)},
            {{Value(1), Value(1), Value(1), Value(3), Value(3), Value(7)}, Value(6)},
            // Expected results with empty values.
            {{Value{}}, Value(1)},
            {{Value{}, Value{}}, Value(1)},

        },
        true /* rank can't be merged */);
}

TEST(Accumulators, DenseRank) {
    auto expCtx = ExpressionContextForTest{};
    assertExpectedResults<AccumulatorDenseRank>(
        &expCtx,
        {
            // Document number is correct.
            {{Value(0)}, Value(1)},
            {{Value(0), Value(2)}, Value(2)},
            {{Value(0), Value(2), Value(4)}, Value(3)},
            // Ties don't increment
            {{Value(1), Value(1)}, Value(1)},
            // Ties don't skip values.
            {{Value(1), Value(1), Value(3)}, Value(2)},
            {{Value(1), Value(1), Value(1), Value(3)}, Value(2)},
            {{Value(1), Value(1), Value(1), Value(3), Value(3), Value(7)}, Value(3)},

        },
        true /* denseRank can't be merged */);
}

TEST(Accumulators, DocumentNumberRank) {
    auto expCtx = ExpressionContextForTest{};
    assertExpectedResults<AccumulatorDocumentNumber>(
        &expCtx,
        {
            // Document number is correct.
            {{Value(0)}, Value(1)},
            {{Value(0), Value(2)}, Value(2)},
            {{Value(0), Value(2), Value(4)}, Value(3)},
            // Ties increment
            {{Value(1), Value(1)}, Value(2)},
            {{Value(1), Value(1), Value(3)}, Value(3)},
            {{Value(1), Value(1), Value(1), Value(3)}, Value(4)},
            {{Value(1), Value(1), Value(1), Value(3), Value(3), Value(7)}, Value(6)},

        },
        true /* denseRank can't be merged */);
}

TEST(Accumulators, AddToSetRespectsCollation) {
    auto expCtx = ExpressionContextForTest{};
    auto collator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kAlwaysEqual);
    expCtx.setCollator(std::move(collator));
    assertExpectedResults<AccumulatorAddToSet>(&expCtx,
                                               {{{Value("a"_sd), Value("b"_sd), Value("c"_sd)},
                                                 Value(std::vector<Value>{Value("a"_sd)})}});
}

TEST(Accumulators, AddToSetRespectsMaxMemoryConstraint) {
    auto expCtx = ExpressionContextForTest{};
    const int maxMemoryBytes = 20ull;
    auto addToSet = AccumulatorAddToSet(&expCtx, maxMemoryBytes);
    ASSERT_THROWS_CODE(
        addToSet.process(
            Value("This is a large string. Certainly we must be over 20 bytes by now"_sd), false),
        AssertionException,
        ErrorCodes::ExceededMemoryLimit);
}

TEST(Accumulators, PushRespectsMaxMemoryConstraint) {
    auto expCtx = ExpressionContextForTest{};
    const int maxMemoryBytes = 20ull;
    auto addToSet = AccumulatorPush(&expCtx, maxMemoryBytes);
    ASSERT_THROWS_CODE(
        addToSet.process(
            Value("This is a large string. Certainly we must be over 20 bytes by now"_sd), false),
        AssertionException,
        ErrorCodes::ExceededMemoryLimit);
}

/* ------------------------- AccumulatorCorvariance(Samp/Pop) -------------------------- */

// Calculate covariance using the offline algorithm.
double offlineCovariance(const std::vector<Value>& input, bool isSamp) {
    // Edge cases return 0 though 'input' should not be empty. Empty input is tested elsewhere.
    if (input.size() <= 1)
        return 0;

    double adjustedN = isSamp ? input.size() - 1 : input.size();
    double meanX = 0;
    double meanY = 0;
    double cXY = 0;

    for (auto&& value : input) {
        meanX += value.getArray()[0].coerceToDouble();
        meanY += value.getArray()[1].coerceToDouble();
    }
    meanX /= input.size();
    meanY /= input.size();

    for (auto&& value : input) {
        cXY += (value.getArray()[0].coerceToDouble() - meanX) *
            (value.getArray()[1].coerceToDouble() - meanY);
    }

    return cXY / adjustedN;
}

// Test the accumulator-output covariance (using an online algorithm:
// https://en.wikipedia.org/wiki/Algorithms_for_calculating_variance#Online) is equal to the
// covariance calculated based on the offline algorithm (cov(x,y) = Σ((xi-avg(x))*(yi-avg(y)))/n)).
// If 'result' is given, the covariance should also be tested against the given result.
template <typename AccName>
static void assertCovariance(ExpressionContext* const expCtx,
                             const std::vector<Value>& input,
                             boost::optional<double> result = boost::none) {
    auto accum = AccName::create(expCtx);
    for (auto&& val : input) {
        accum->process(val, false);
    }
    double onlineCov = accum->getValue(false).coerceToDouble();
    double offlineCov =
        offlineCovariance(input, std::is_same_v<AccName, AccumulatorCovarianceSamp>);

    ASSERT_LTE(fabs(onlineCov - offlineCov), 1e-10);
    if (result) {
        ASSERT_LTE(fabs(onlineCov - *result), 1e-5);
    }
}

TEST(Accumulators, CovarianceEdgeCases) {
    auto expCtx = ExpressionContextForTest{};

    // The sample covariance of variables of single value should be undefined.
    const std::vector<Value> singlePoint = {
        Value(std::vector<Value>({Value(0), Value(1)})),
    };

    // This is actually an "undefined" case because NaN/Inf is not counted.
    const std::vector<Value> nonFiniteOnly = {
        Value(std::vector<Value>({Value(numeric_limits<double>::quiet_NaN()),
                                  Value(numeric_limits<double>::quiet_NaN())})),
        Value(std::vector<Value>({Value(numeric_limits<double>::infinity()),
                                  Value(numeric_limits<double>::infinity())})),
    };

    const std::vector<Value> mixedPoints = {
        Value(std::vector<Value>({Value(numeric_limits<double>::quiet_NaN()),
                                  Value(numeric_limits<double>::quiet_NaN())})),
        Value(std::vector<Value>({Value(numeric_limits<double>::infinity()),
                                  Value(numeric_limits<double>::infinity())})),
        Value(std::vector<Value>({Value(0), Value(1)})),
        Value(std::vector<Value>({Value(1), Value(2)})),
    };

    assertExpectedResults<AccumulatorCovariancePop>(
        &expCtx,
        {
            {{}, Value(BSONNULL)},
            {singlePoint, Value(0.0)},
            {nonFiniteOnly, Value(BSONNULL)},
            {mixedPoints, Value(numeric_limits<double>::quiet_NaN())},
        },
        true /* Covariance accumulator can't be merged */);

    assertExpectedResults<AccumulatorCovarianceSamp>(
        &expCtx,
        {
            {{}, Value(BSONNULL)},
            {singlePoint, Value(BSONNULL)},
            {nonFiniteOnly, Value(BSONNULL)},
            {mixedPoints, Value(numeric_limits<double>::quiet_NaN())},
        },
        true /* Covariance accumulator can't be merged */);
}

TEST(Accumulators, PopulationCovariance) {
    auto expCtx = ExpressionContextForTest{};

    // Some doubles as input.
    const std::vector<Value> multiplePoints = {
        Value(std::vector<Value>({Value(0), Value(1.5)})),
        Value(std::vector<Value>({Value(1.4), Value(2.5)})),
        Value(std::vector<Value>({Value(4.7), Value(3.6)})),
    };

    // Test both offline and online corvariance algorithm with a given result.
    assertCovariance<AccumulatorCovariancePop>(&expCtx, multiplePoints, 1.655556);
}

TEST(Accumulators, SampleCovariance) {
    auto expCtx = ExpressionContextForTest{};

    // Some doubles as input.
    std::vector<Value> multiplePoints = {
        Value(std::vector<Value>({Value(0), Value(1.5)})),
        Value(std::vector<Value>({Value(1.4), Value(2.5)})),
        Value(std::vector<Value>({Value(4.7), Value(3.6)})),
    };

    // Test both offline and online corvariance algorithm with a given result.
    assertCovariance<AccumulatorCovarianceSamp>(&expCtx, multiplePoints, 2.483334);
}

std::vector<Value> generateRandomVariables() {
    auto seed = Date_t::now().asInt64();
    LOGV2(5424001, "Generated new seed is {seed}", "seed"_attr = seed);

    std::vector<Value> output;
    PseudoRandom prng(seed);
    const int variableSize = prng.nextInt32(1000) + 2;

    for (int i = 0; i < variableSize; i++) {
        std::vector<Value> newXY;
        newXY.push_back(Value(prng.nextCanonicalDouble()));
        newXY.push_back(Value(prng.nextCanonicalDouble()));
        output.push_back(Value(newXY));
    }

    return output;
}

TEST(Accumulators, CovarianceWithRandomVariables) {
    auto expCtx = ExpressionContextForTest{};

    // Some randomly generated variables as input.
    std::vector<Value> randomVariables = generateRandomVariables();

    assertCovariance<AccumulatorCovariancePop>(&expCtx, randomVariables, boost::none);
    assertCovariance<AccumulatorCovarianceSamp>(&expCtx, randomVariables, boost::none);
}

/* ------------------------- AccumulatorMergeObjects -------------------------- */

TEST(AccumulatorMergeObjects, MergingZeroObjectsShouldReturnEmptyDocument) {
    auto expCtx = ExpressionContextForTest{};

    assertExpectedResults<AccumulatorMergeObjects>(&expCtx, {{{}, {Value(Document({}))}}});
}

TEST(AccumulatorMergeObjects, MergingWithSingleObjectShouldLeaveUnchanged) {
    auto expCtx = ExpressionContextForTest{};

    assertExpectedResults<AccumulatorMergeObjects>(&expCtx, {{{}, {Value(Document({}))}}});

    auto doc = Value(Document({{"a", 1}, {"b", 1}}));
    assertExpectedResults<AccumulatorMergeObjects>(&expCtx, {{{doc}, doc}});
}

TEST(AccumulatorMergeObjects, MergingDisjointObjectsShouldIncludeAllFields) {
    auto expCtx = ExpressionContextForTest{};
    auto first = Value(Document({{"a", 1}, {"b", 1}}));
    auto second = Value(Document({{"c", 1}}));
    assertExpectedResults<AccumulatorMergeObjects>(
        &expCtx, {{{first, second}, Value(Document({{"a", 1}, {"b", 1}, {"c", 1}}))}});
}

TEST(AccumulatorMergeObjects, MergingIntersectingObjectsShouldOverrideInOrderReceived) {
    auto expCtx = ExpressionContextForTest{};
    auto first = Value(Document({{"a", "oldValue"_sd}, {"b", 0}, {"c", 1}}));
    auto second = Value(Document({{"a", "newValue"_sd}}));
    assertExpectedResults<AccumulatorMergeObjects>(
        &expCtx, {{{first, second}, Value(Document({{"a", "newValue"_sd}, {"b", 0}, {"c", 1}}))}});
}

TEST(AccumulatorMergeObjects, MergingIntersectingEmbeddedObjectsShouldOverrideInOrderReceived) {
    auto expCtx = ExpressionContextForTest{};
    auto firstSubDoc = Document({{"a", 1}, {"b", 2}, {"c", 3}});
    auto secondSubDoc = Document({{"a", 2}, {"b", 1}});
    auto first = Value(Document({{"d", 1}, {"subDoc", firstSubDoc}}));
    auto second = Value(Document({{"subDoc", secondSubDoc}}));
    auto expected = Value(Document({{"d", 1}, {"subDoc", secondSubDoc}}));
    assertExpectedResults<AccumulatorMergeObjects>(&expCtx, {{{first, second}, expected}});
}

TEST(AccumulatorMergeObjects, MergingWithEmptyDocumentShouldIgnore) {
    auto expCtx = ExpressionContextForTest{};
    auto first = Value(Document({{"a", 0}, {"b", 1}, {"c", 1}}));
    auto second = Value(Document({}));
    auto expected = Value(Document({{"a", 0}, {"b", 1}, {"c", 1}}));
    assertExpectedResults<AccumulatorMergeObjects>(&expCtx, {{{first, second}, expected}});
}

}  // namespace AccumulatorTests
