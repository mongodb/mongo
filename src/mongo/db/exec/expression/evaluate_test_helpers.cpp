// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/expression/evaluate_test_helpers.h"

#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace expression_evaluation_test {

/**
 * Creates an expression given by 'expressionName' and evaluates it using
 * 'operands' as inputs, returning the result.
 */
Value evaluateExpression(const std::string& expressionName,
                         const std::vector<ImplicitValue>& operands) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;
    const BSONObj obj = BSON(expressionName << Value(ImplicitValue::convertToValues(operands)));
    auto expression = Expression::parseExpression(&expCtx, obj, vps);
    Value result = expression->evaluate({}, &expCtx.variables);
    return result;
}

/**
 * Takes the name of an expression as its first argument and a list of pairs of arguments and
 * expected results as its second argument, and asserts that for the given expression the arguments
 * evaluate to the expected results.
 */
void assertExpectedResults(
    const std::string& expression,
    std::initializer_list<std::pair<std::initializer_list<ImplicitValue>, ImplicitValue>>
        operations) {
    for (auto&& op : operations) {
        try {
            Value result = evaluateExpression(expression, op.first);
            ASSERT_VALUE_EQ(op.second, result);
            ASSERT_EQUALS(op.second.getType(), result.getType());
        } catch (...) {
            LOGV2(9949805, "failed", "argument"_attr = ImplicitValue::convertToValues(op.first));
            throw;
        }
    }
}

/** Convert Value to a wrapped BSONObj with an empty string field name. */
BSONObj toBson(const Value& value) {
    BSONObjBuilder bob;
    value.addToBsonObj(&bob, "");
    return bob.obj();
}

/** Convert Document to BSON. */
BSONObj toBson(const Document& document) {
    return document.toBson();
}

/** Create a Document from a BSONObj. */
Document fromBson(BSONObj obj) {
    return Document(obj);
}

Document fromJson(const std::string& json) {
    return Document(fromjson(json));
}

}  // namespace expression_evaluation_test
}  // namespace mongo
