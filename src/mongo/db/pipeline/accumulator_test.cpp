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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include <cmath>
#include <memory>

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/accumulator_for_window_functions.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/logv2/log.h"

namespace AccumulatorTests {

using boost::intrusive_ptr;
using std::numeric_limits;
using std::string;

/**
 * Takes the name of an AccumulatorState as its template argument and a list of pairs of arguments
 * and expected results as its second argument, and asserts that for the given AccumulatorState the
 * arguments evaluate to the expected results.
 */
template <typename AccName>
static void assertExpectedResults(
    ExpressionContext* const expCtx,
    std::initializer_list<std::pair<std::vector<Value>, Value>> operations,
    bool skipMerging = false) {
    for (auto&& op : operations) {
        try {
            // Asserts that result equals expected result when not sharded.
            {
                auto accum = AccName::create(expCtx);
                for (auto&& val : op.first) {
                    accum->process(val, false);
                }
                Value result = accum->getValue(false);
                ASSERT_VALUE_EQ(op.second, result);
                ASSERT_EQUALS(op.second.getType(), result.getType());
            }

            // Asserts that result equals expected result when all input is on one shard.
            if (!skipMerging) {
                auto accum = AccName::create(expCtx);
                auto shard = AccName::create(expCtx);
                for (auto&& val : op.first) {
                    shard->process(val, false);
                }
                accum->process(shard->getValue(true), true);
                Value result = accum->getValue(false);
                ASSERT_VALUE_EQ(op.second, result);
                ASSERT_EQUALS(op.second.getType(), result.getType());
            }

            // Asserts that result equals expected result when each input is on a separate shard.
            if (!skipMerging) {
                auto accum = AccName::create(expCtx);
                for (auto&& val : op.first) {
                    auto shard = AccName::create(expCtx);
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
