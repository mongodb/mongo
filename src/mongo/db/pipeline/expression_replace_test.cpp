// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace {
using namespace std::literals::string_view_literals;
using namespace mongo;

auto parse(const std::string& expressionName, ImplicitValue operand) {
    auto pair =
        std::pair{std::make_unique<ExpressionContextForTest>(), boost::intrusive_ptr<Expression>{}};
    VariablesParseState vps = pair.first->variablesParseState;
    Value operandValue = operand;
    const BSONObj obj = BSON(expressionName << operandValue);
    pair.second = Expression::parseExpression(pair.first.get(), obj, vps);
    return pair;
}

TEST(ExpressionReplaceTest, Expects3NamedArgs) {
    ASSERT_THROWS(parse("$replaceOne", 1), AssertionException);
    ASSERT_THROWS(parse("$replaceOne", BSON_ARRAY(1 << 2)), AssertionException);
    ASSERT_THROWS(parse("$replaceOne", BSONNULL), AssertionException);
    ASSERT_THROWS(parse("$replaceOne", "string"sv), AssertionException);
    parse("$replaceOne", Document{{"input", 1}, {"find", 1}, {"replacement", 1}});

    ASSERT_THROWS(parse("$replaceAll", 1), AssertionException);
    ASSERT_THROWS(parse("$replaceAll", BSON_ARRAY(1 << 2)), AssertionException);
    ASSERT_THROWS(parse("$replaceAll", BSONNULL), AssertionException);
    ASSERT_THROWS(parse("$replaceAll", "string"sv), AssertionException);
    parse("$replaceAll", Document{{"input", 1}, {"find", 1}, {"replacement", 1}});
}

}  // namespace
