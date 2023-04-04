/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace ExpressionTests {
namespace {

std::string redactFieldNameForTest(StringData s) {
    return str::stream() << "HASH<" << s << ">";
}

TEST(RedactionTest, ExpressionLet) {
    SerializationOptions options;
    options.identifierRedactionPolicy = redactFieldNameForTest;
    options.redactIdentifiers = true;

    auto expCtx = ExpressionContextForTest{};

    auto expression = Expression::parseExpression(&expCtx,
                                                  fromjson(R"(
        {$let: {vars: {foo: 35}, in: {$gt: ["$$foo", 23]}}}
    )"),
                                                  expCtx.variablesParseState);
    ASSERT_DOCUMENT_EQ_AUTO(  // NOLINT
        R"({
            "$let": {
                "vars": {
                    "HASH<foo>": {
                        "$const": 35
                    }
                },
                "in": {
                    "$gt": [
                        "$$HASH<foo>",
                        {
                            "$const": 23
                        }
                    ]
                }
            }
        })",
        expression->serialize(options).getDocument());

    expression = Expression::parseExpression(&expCtx,
                                             fromjson(R"(
        {$let: {vars: {foo: 35, myNow: "$$NOW", obj: {hello: 23}}, in: {$gt: ["$$myNow", "$$obj.hello"]}}}
    )"),
                                             expCtx.variablesParseState);

    ASSERT_DOCUMENT_EQ_AUTO(  // NOLINT
        R"({
            "$let": {
                "vars": {
                    "HASH<foo>": {
                        "$const": 35
                    },
                    "HASH<myNow>": "$$NOW",
                    "HASH<obj>": {
                        "HASH<hello>": {
                            "$const": 23
                        }
                    }
                },
                "in": {
                    "$gt": [
                        "$$HASH<myNow>",
                        "$$HASH<obj>.HASH<hello>"
                    ]
                }
            }
        })",
        expression->serialize(options).getDocument());
}
}  // namespace
}  // namespace ExpressionTests
}  // namespace mongo
