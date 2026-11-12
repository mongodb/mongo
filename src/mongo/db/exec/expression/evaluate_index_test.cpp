// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


// IWYU pragma: no_include "boost/container/detail/std_fwd.hpp"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/memory_tracking/memory_usage_tracker.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/unittest.h"

#include <memory>


namespace mongo {
namespace expression_evaluation_test {
using namespace std::literals::string_view_literals;

TEST(ExpressionToHashedIndexKeyTest, StringInputSucceeds) {
    auto expCtx = ExpressionContextForTest{};
    const BSONObj obj = BSON("$toHashedIndexKey" << "hashThisStringLiteral"sv);
    auto expression = Expression::parseExpression(&expCtx, obj, expCtx.variablesParseState);
    Value result = expression->evaluate({}, &expCtx.variables);
    ASSERT_VALUE_EQ(result, Value::createIntOrLong(-5776344739422278694));
}

TEST(ExpressionToHashedIndexKeyTest, IntInputSucceeds) {
    auto expCtx = ExpressionContextForTest{};
    const BSONObj obj = BSON("$toHashedIndexKey" << 123);
    auto expression = Expression::parseExpression(&expCtx, obj, expCtx.variablesParseState);
    Value result = expression->evaluate({}, &expCtx.variables);
    ASSERT_VALUE_EQ(result, Value::createIntOrLong(-6548868637522515075));
}

TEST(ExpressionToHashedIndexKeyTest, TimestampInputSucceeds) {
    auto expCtx = ExpressionContextForTest{};
    const BSONObj obj = BSON("$toHashedIndexKey" << Timestamp(0, 0));
    auto expression = Expression::parseExpression(&expCtx, obj, expCtx.variablesParseState);
    Value result = expression->evaluate({}, &expCtx.variables);
    ASSERT_VALUE_EQ(result, Value::createIntOrLong(-7867208682377458672));
}

TEST(ExpressionToHashedIndexKeyTest, ObjectIdInputSucceeds) {
    auto expCtx = ExpressionContextForTest{};
    const BSONObj obj = BSON("$toHashedIndexKey" << OID("47cc67093475061e3d95369d"));
    auto expression = Expression::parseExpression(&expCtx, obj, expCtx.variablesParseState);
    Value result = expression->evaluate({}, &expCtx.variables);
    ASSERT_VALUE_EQ(result, Value::createIntOrLong(1576265281381834298));
}

TEST(ExpressionToHashedIndexKeyTest, DateInputSucceeds) {
    auto expCtx = ExpressionContextForTest{};
    const BSONObj obj = BSON("$toHashedIndexKey" << Date_t());
    auto expression = Expression::parseExpression(&expCtx, obj, expCtx.variablesParseState);
    Value result = expression->evaluate({}, &expCtx.variables);
    ASSERT_VALUE_EQ(result, Value::createIntOrLong(-1178696894582842035));
}

TEST(ExpressionToHashedIndexKeyTest, MissingInputValueSucceeds) {
    auto expCtx = ExpressionContextForTest{};
    const BSONObj obj = BSON("$toHashedIndexKey" << "$missingField");
    auto expression = Expression::parseExpression(&expCtx, obj, expCtx.variablesParseState);
    Value result = expression->evaluate({}, &expCtx.variables);
    ASSERT_VALUE_EQ(result, Value::createIntOrLong(2338878944348059895));
}

TEST(ExpressionToHashedIndexKeyTest, NullInputSucceeds) {
    auto expCtx = ExpressionContextForTest{};
    const BSONObj obj = BSON("$toHashedIndexKey" << BSONNULL);
    auto expression = Expression::parseExpression(&expCtx, obj, expCtx.variablesParseState);
    Value result = expression->evaluate({}, &expCtx.variables);
    ASSERT_VALUE_EQ(result, Value::createIntOrLong(2338878944348059895));
}

TEST(ExpressionToHashedIndexKeyTest, ExpressionInputSucceeds) {
    auto expCtx = ExpressionContextForTest{};
    const BSONObj obj = BSON("$toHashedIndexKey" << BSON("$pow" << BSON_ARRAY(2 << 4)));
    auto expression = Expression::parseExpression(&expCtx, obj, expCtx.variablesParseState);
    Value result = expression->evaluate({}, &expCtx.variables);
    ASSERT_VALUE_EQ(result, Value::createIntOrLong(2598032665634823220));
}

TEST(ExpressionToHashedIndexKeyTest, UndefinedInputSucceeds) {
    auto expCtx = ExpressionContextForTest{};
    const BSONObj obj = BSON("$toHashedIndexKey" << BSONUndefined);
    auto expression = Expression::parseExpression(&expCtx, obj, expCtx.variablesParseState);
    Value result = expression->evaluate({}, &expCtx.variables);
    ASSERT_VALUE_EQ(result, Value::createIntOrLong(40158834000849533LL));
}

/**
 * A collator factory handing out a string-reversing mock collator, so that each string in the input
 * contributes a predictable number of collation bytes.
 */
class ReversingCollatorFactory final : public CollatorFactoryInterface {
public:
    StatusWith<std::unique_ptr<CollatorInterface>> makeFromBSON(const BSONObj&) final {
        return {std::make_unique<CollatorInterfaceMock>(
            CollatorInterfaceMock::MockType::kReverseString)};
    }
};

void installReversingCollatorFactory(ExpressionContextForTest& expCtx) {
    CollatorFactoryInterface::set(expCtx.getOperationContext()->getServiceContext(),
                                  std::make_unique<ReversingCollatorFactory>());
}

// Emits a fixed-size comparison key regardless of its input.
class FixedKeyCollator final : public CollatorInterface {
public:
    explicit FixedKeyCollator(size_t keySize)
        : CollatorInterface(Collation(std::string("fixed"))), _keySize(keySize) {}

    std::unique_ptr<CollatorInterface> clone() const final {
        return std::make_unique<FixedKeyCollator>(_keySize);
    }
    std::shared_ptr<CollatorInterface> cloneShared() const final {
        return std::make_shared<FixedKeyCollator>(_keySize);
    }
    int compare(std::string_view, std::string_view) const final {
        return 0;
    }
    ComparisonKey getComparisonKey(std::string_view) const final {
        return makeComparisonKey(std::string(_keySize, 'x'));
    }

private:
    size_t _keySize;
};

// Hands out FixedKeyCollator instances with a fixed key size.
class FixedKeyCollatorFactory final : public CollatorFactoryInterface {
public:
    explicit FixedKeyCollatorFactory(size_t keySize = 32) : _keySize(keySize) {}

    StatusWith<std::unique_ptr<CollatorInterface>> makeFromBSON(const BSONObj&) final {
        return {std::make_unique<FixedKeyCollator>(_keySize)};
    }

private:
    size_t _keySize;
};

TEST(ExpressionInternalKeyStringValueTest, SortKeyMemoryIsBoundedWithinEvaluation) {
    auto expCtx = ExpressionContextForTest{};

    // A fixed 32-byte key per element; the limit admits the input copy plus one key, not two.
    constexpr size_t kKeySize = 32;
    CollatorFactoryInterface::set(expCtx.getOperationContext()->getServiceContext(),
                                  std::make_unique<FixedKeyCollatorFactory>(kKeySize));

    const std::vector<Value> input{Value{"a"sv}, Value{"b"sv}, Value{"c"sv}, Value{"d"sv}};
    const int64_t copiedInputBytes = Value(input).wrap("").objsize();
    SimpleMemoryUsageTracker tracker{
        MemoryUsageLimit{copiedInputBytes + static_cast<int64_t>(kKeySize)}};
    EvaluationContext ctx{.tracker = &tracker};

    const BSONObj obj = BSON("$_internalKeyStringValue"
                             << BSON("input" << "$a"sv << "collation" << BSON("locale" << "en"sv)));
    auto expression = Expression::parseExpression(&expCtx, obj, expCtx.variablesParseState);

    ASSERT_THROWS_CODE(expression->evaluate(Document{{"a"sv, input}}, &expCtx.variables, ctx),
                       AssertionException,
                       ErrorCodes::ExceededMemoryLimit);

    ASSERT_EQ(tracker.inUseTrackedMemoryBytes(), 0);
}

TEST(ExpressionInternalKeyStringValueTest, MemoryIsBoundedWithoutCollation) {
    auto expCtx = ExpressionContextForTest{};
    installReversingCollatorFactory(expCtx);

    // Limit to the copied input BSON size, so the encoded key-string is what trips the limit.
    const std::vector<Value> input{Value{"0123456789"sv}, Value{"0123456789"sv}};
    const int64_t copiedInputBytes = Value(input).wrap("").objsize();
    SimpleMemoryUsageTracker tracker{MemoryUsageLimit{copiedInputBytes}};
    EvaluationContext ctx{.tracker = &tracker};

    const BSONObj obj = BSON("$_internalKeyStringValue" << BSON("input" << "$a"sv));
    auto expression = Expression::parseExpression(&expCtx, obj, expCtx.variablesParseState);
    ASSERT_THROWS_CODE(expression->evaluate(Document{{"a"sv, input}}, &expCtx.variables, ctx),
                       AssertionException,
                       ErrorCodes::ExceededMemoryLimit);

    ASSERT_EQ(tracker.inUseTrackedMemoryBytes(), 0);
}

}  // namespace expression_evaluation_test
}  // namespace mongo
