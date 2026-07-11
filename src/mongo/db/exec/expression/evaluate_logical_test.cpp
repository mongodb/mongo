// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include <boost/smart_ptr/intrusive_ptr.hpp>
// IWYU pragma: no_include "boost/container/detail/std_fwd.hpp"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"

#include <limits>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace expression_evaluation_test {
using namespace std::literals::string_view_literals;

using boost::intrusive_ptr;

namespace all_any_elements {
void runTest(Document spec) {
    auto expCtx = ExpressionContextForTest{};
    const Value args = spec["input"];
    if (!spec["expected"].missing()) {
        FieldIterator fields(spec["expected"].getDocument());
        while (fields.more()) {
            const Document::FieldPair field(fields.next());
            const Value expected = field.second;
            const BSONObj obj = BSON(field.first << args);
            VariablesParseState vps = expCtx.variablesParseState;
            const intrusive_ptr<Expression> expr = Expression::parseExpression(&expCtx, obj, vps);
            const Value result = expr->evaluate({}, &expCtx.variables);
            if (ValueComparator().evaluate(result != expected)) {
                std::string errMsg = str::stream()
                    << "for expression " << std::string{field.first} << " with argument "
                    << args.toString() << " full tree: " << expr->serialize().toString()
                    << " expected: " << expected.toString() << " but got: " << result.toString();
                FAIL(errMsg);
            }
            // TODO test optimize here
        }
    }
    if (!spec["error"].missing()) {
        MONGO_COMPILER_DIAGNOSTIC_PUSH
        MONGO_COMPILER_DIAGNOSTIC_IGNORED_TRANSITIONAL("-Wdangling-reference")
        const std::vector<Value>& asserters = spec["error"].getArray();
        MONGO_COMPILER_DIAGNOSTIC_POP
        size_t n = asserters.size();
        for (size_t i = 0; i < n; i++) {
            const BSONObj obj = BSON(asserters[i].getString() << args);
            VariablesParseState vps = expCtx.variablesParseState;
            ASSERT_THROWS(
                [&] {
                    // NOTE: parse and evaluatation failures are treated the
                    // same
                    const intrusive_ptr<Expression> expr =
                        Expression::parseExpression(&expCtx, obj, vps);
                    expr->evaluate({}, &expCtx.variables);
                }(),
                AssertionException);
        }
    }
}

TEST(ExpressionAllAnyElementsTest, JustFalse) {
    runTest(DOC("input" << DOC_ARRAY(DOC_ARRAY(false)) << "expected"
                        << DOC("$allElementsTrue" << false << "$anyElementTrue" << false)));
}

TEST(ExpressionAllAnyElementsTest, JustTrue) {
    runTest(DOC("input" << DOC_ARRAY(DOC_ARRAY(true)) << "expected"
                        << DOC("$allElementsTrue" << true << "$anyElementTrue" << true)));
}

TEST(ExpressionAllAnyElementsTest, OneTrueOneFalse) {
    runTest(DOC("input" << DOC_ARRAY(DOC_ARRAY(true << false)) << "expected"
                        << DOC("$allElementsTrue" << false << "$anyElementTrue" << true)));
}

TEST(ExpressionAllAnyElementsTest, Empty) {
    runTest(DOC("input" << DOC_ARRAY(std::vector<Value>()) << "expected"
                        << DOC("$allElementsTrue" << true << "$anyElementTrue" << false)));
}

TEST(ExpressionAllAnyElementsTest, TrueViaInt) {
    runTest(DOC("input" << DOC_ARRAY(DOC_ARRAY(1)) << "expected"
                        << DOC("$allElementsTrue" << true << "$anyElementTrue" << true)));
}

TEST(ExpressionAllAnyElementsTest, FalseViaInt) {
    runTest(DOC("input" << DOC_ARRAY(DOC_ARRAY(0)) << "expected"
                        << DOC("$allElementsTrue" << false << "$anyElementTrue" << false)));
}

TEST(ExpressionAllAnyElementsTest, Null) {
    runTest(DOC("input" << DOC_ARRAY(BSONNULL) << "error"
                        << DOC_ARRAY("$allElementsTrue"sv << "$anyElementTrue"sv)));
}

}  // namespace all_any_elements

/**
 * Expressions registered with REGISTER_EXPRESSION_WITH_FEATURE_FLAG with feature flags that are not
 * active by default are not available for parsing in unit tests, since at MONGO_INITIALIZER-time,
 * the feature flag is false, so the expression isn't registered. This function calls the parse
 * function on an expression class directly to bypass the global parser map.
 */
template <typename T>
Value evaluateUnregisteredExpression(std::vector<ImplicitValue> operands) {
    auto expCtx = ExpressionContextForTest{};
    auto val = Value(ImplicitValue::convertToValues(operands));
    const BSONObj obj = BSON("" << val);
    auto expr = T::parse(&expCtx, obj.firstElement(), expCtx.variablesParseState);
    return expr->evaluate({}, &expCtx.variables);
}

/**
 * Version of assertExpectedResults() that bypasses the global parser map and always parses
 * expressions of the templated type.
 */
template <typename T>
void assertExpectedResultsUnregistered(
    std::initializer_list<std::pair<std::initializer_list<ImplicitValue>, ImplicitValue>>
        operations) {
    for (auto&& op : operations) {
        try {
            Value result = evaluateUnregisteredExpression<T>(op.first);
            ASSERT_VALUE_EQ(op.second, result);
            ASSERT_EQUALS(op.second.getType(), result.getType());
        } catch (...) {
            LOGV2(6688000, "failed", "argument"_attr = ImplicitValue::convertToValues(op.first));
            throw;
        }
    }
}

TEST(ExpressionBitAndTest, BitAndCorrectness) {
    assertExpectedResultsUnregistered<ExpressionBitAnd>({
        // Explicit correctness cases.
        {{0b0, 0b0}, 0b0},
        {{0b0, 0b1}, 0b0},
        {{0b1, 0b0}, 0b0},
        {{0b1, 0b1}, 0b1},

        {{0b00, 0b00}, 0b00},
        {{0b00, 0b01}, 0b00},
        {{0b01, 0b00}, 0b00},
        {{0b01, 0b01}, 0b01},

        {{0b00, 0b00}, 0b00},
        {{0b00, 0b11}, 0b00},
        {{0b11, 0b00}, 0b00},
        {{0b11, 0b11}, 0b11},
    });
}

TEST(ExpressionBitAndTest, BitAndInt) {
    assertExpectedResultsUnregistered<ExpressionBitAnd>({
        // Empty operand list should evaluate to the identity for the operation.
        {{}, -1},
        // Singleton cases.
        {{0}, 0},
        {{256}, 256},
        // Binary cases
        {{5, 2}, 5 & 2},
        {{255, 0}, 255 & 0},
        // Ternary cases
        {{5, 2, 10}, 5 & 2 & 10},
    });
}

TEST(ExpressionBitAndTest, BitAndLong) {
    assertExpectedResultsUnregistered<ExpressionBitAnd>({
        // Singleton cases.
        {{0LL}, 0LL},
        {{1LL << 40}, 1LL << 40},
        {{256LL}, 256LL},
        // Binary cases.
        {{5LL, 2LL}, 5LL & 2LL},
        {{255LL, 0LL}, 255LL & 0LL},
        // Ternary cases.
        {{5, 2, 10}, 5 & 2 & 10},
    });
}

TEST(ExpressionBitAndTest, BitAndMixedTypes) {
    // Any NumberLong widens the resulting type to NumberLong.
    assertExpectedResultsUnregistered<ExpressionBitAnd>({
        // Binary cases
        {{5LL, 2}, 5LL & 2},
        {{5, 2LL}, 5 & 2LL},
        {{255LL, 0}, 255LL & 0},
        {{255, 0LL}, 255 & 0LL},
    });
}

TEST(ExpressionBitOrTest, BitOrInt) {
    assertExpectedResultsUnregistered<ExpressionBitOr>({
        {{}, 0},
        // Singleton cases.
        {{0}, 0},
        {{256}, 256},
        // Binary cases
        {{5, 2}, 5 | 2},
        {{255, 0}, 255 | 0},
        // Ternary cases
        {{5, 2, 10}, 5 | 2 | 10},
    });
}

TEST(ExpressionBitOrTest, BitOrLong) {
    assertExpectedResultsUnregistered<ExpressionBitOr>({
        // Singleton cases.
        {{0LL}, 0LL},
        {{256LL}, 256LL},
        // Binary cases.
        {{5LL, 2LL}, 5LL | 2LL},
        {{255LL, 0LL}, 255LL | 0LL},
        // Ternary cases.
        {{5, 2, 10}, 5 | 2 | 10},
    });
}

TEST(ExpressionBitOrTest, BitOrMixedTypes) {
    // Any NumberLong widens the resulting type to NumberLong.
    assertExpectedResultsUnregistered<ExpressionBitOr>({
        // Binary cases
        {{5LL, 2}, 5LL | 2},
        {{5, 2LL}, 5 | 2LL},
        {{255LL, 0}, 255LL | 0},
        {{255, 0LL}, 255 | 0LL},
    });
}

TEST(ExpressionBitXorTest, BitXorInt) {
    assertExpectedResultsUnregistered<ExpressionBitXor>({
        {{}, 0},
        // Singleton cases.
        {{0}, 0},
        {{256}, 256},
        // Binary cases
        {{5, 2}, 5 ^ 2},
        {{255, 0}, 255 ^ 0},
        // Ternary cases
        {{5, 2, 10}, 5 ^ 2 ^ 10},
    });
}

TEST(ExpressionBitXorTest, BitXorLong) {
    assertExpectedResultsUnregistered<ExpressionBitXor>({
        // Singleton cases.
        {{0LL}, 0LL},
        {{256LL}, 256LL},
        // Binary cases.
        {{5LL, 2LL}, 5LL ^ 2LL},
        {{255LL, 0LL}, 255LL ^ 0LL},
        // Ternary cases.
        {{5, 2, 10}, 5 ^ 2 ^ 10},
    });
}

TEST(ExpressionBitXorTest, BitXorMixedTypes) {
    // Any NumberLong widens the resulting type to NumberLong.
    assertExpectedResultsUnregistered<ExpressionBitXor>({
        // Binary cases
        {{5LL, 2}, 5LL ^ 2},
        {{5, 2LL}, 5 ^ 2LL},
        {{255LL, 0}, 255LL ^ 0},
        {{255, 0LL}, 255 ^ 0LL},
    });
}


TEST(ExpressionBitNotTest, Int) {
    int min = std::numeric_limits<int>::min();
    int max = std::numeric_limits<int>::max();
    assertExpectedResultsUnregistered<ExpressionBitNot>({
        {{0}, -1},
        {{-1}, 0},
        {{1}, -2},
        {{3}, -4},
        {{100}, -101},
        {{min}, ~min},
        {{max}, ~max},
        {{max}, min},
        {{min}, max},
    });
}

TEST(ExpressionBitNotTest, Long) {
    long long min = std::numeric_limits<long long>::min();
    long long max = std::numeric_limits<long long>::max();
    assertExpectedResultsUnregistered<ExpressionBitNot>({
        {{0LL}, -1LL},
        {{-1LL}, 0LL},
        {{1LL}, -2LL},
        {{3LL}, -4LL},
        {{100LL}, -101LL},
        {{2147483649LL}, ~2147483649LL},
        {{-2147483655LL}, ~(-2147483655LL)},
        {{min}, ~min},
        {{max}, ~max},
        {{max}, min},
        {{min}, max},
    });
}

TEST(ExpressionBitNotTest, OtherNumerics) {
    ASSERT_THROWS_CODE(evaluateUnregisteredExpression<ExpressionBitNot>({1.5}),
                       AssertionException,
                       ErrorCodes::TypeMismatch);
    ASSERT_THROWS_CODE(evaluateUnregisteredExpression<ExpressionBitNot>({Decimal128("0")}),
                       AssertionException,
                       ErrorCodes::TypeMismatch);
}

TEST(ExpressionBitNotTest, NonNumerics) {
    ASSERT_THROWS_CODE(
        evaluateUnregisteredExpression<ExpressionBitNot>({"hi"sv}), AssertionException, 28765);
    ASSERT_THROWS_CODE(
        evaluateUnregisteredExpression<ExpressionBitNot>({true}), AssertionException, 28765);
}

TEST(ExpressionBitNotTest, Arrays) {
    ASSERT_THROWS_CODE(
        evaluateUnregisteredExpression<ExpressionBitNot>({1, 2, 3}), AssertionException, 16020);
    ASSERT_THROWS_CODE(evaluateUnregisteredExpression<ExpressionBitNot>({1LL, 2LL, 3LL}),
                       AssertionException,
                       16020);
}

}  // namespace expression_evaluation_test
}  // namespace mongo
