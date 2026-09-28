/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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


// IWYU pragma: no_include "boost/container/detail/std_fwd.hpp"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <vector>


namespace mongo {
namespace expression_evaluation_test {

TEST(ExpressionToHashedIndexKeyTest, StringInputSucceeds) {
    auto expCtx = ExpressionContextForTest{};
    const BSONObj obj = BSON("$toHashedIndexKey" << "hashThisStringLiteral"_sd);
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

TEST(ExpressionInternalKeyStringValueTest, CollationBytesAreBounded) {
    QueryTestServiceContext serviceContext;
    auto opCtx = serviceContext.makeOperationContext();
    ExpressionContextForTest expCtx(opCtx.get());
    installReversingCollatorFactory(expCtx);

    // The mock collator reverses strings, so each element contributes its own length in collation
    // bytes. Set the limit low enough that an input of a few elements crosses it.
    RAIIServerParameterControllerForTest limit{"internalQueryKeyStringValueMaxCollationBytes", 16};

    const BSONObj obj = BSON("$_internalKeyStringValue"
                             << BSON("input" << "$a" << "collation" << BSON("locale" << "en")));
    auto expression = Expression::parseExpression(&expCtx, obj, expCtx.variablesParseState);

    ASSERT_THROWS_CODE(
        expression->evaluate(
            Document{{"a", std::vector<Value>{Value{"0123456789"_sd}, Value{"0123456789"_sd}}}},
            &expCtx.variables),
        AssertionException,
        ErrorCodes::ExceededMemoryLimit);

    ASSERT_DOES_NOT_THROW(
        expression->evaluate(Document{{"a", "0123456789"_sd}}, &expCtx.variables));
}

TEST(ExpressionInternalKeyStringValueTest, NoCollationIsNotBoundedByCollationLimit) {
    QueryTestServiceContext serviceContext;
    auto opCtx = serviceContext.makeOperationContext();
    ExpressionContextForTest expCtx(opCtx.get());
    installReversingCollatorFactory(expCtx);

    RAIIServerParameterControllerForTest limit{"internalQueryKeyStringValueMaxCollationBytes", 1};

    const BSONObj obj = BSON("$_internalKeyStringValue" << BSON("input" << "$a"));
    auto expression = Expression::parseExpression(&expCtx, obj, expCtx.variablesParseState);
    ASSERT_DOES_NOT_THROW(expression->evaluate(
        Document{{"a", std::vector<Value>{Value{"0123456789"_sd}, Value{"0123456789"_sd}}}},
        &expCtx.variables));
}

}  // namespace expression_evaluation_test
}  // namespace mongo
