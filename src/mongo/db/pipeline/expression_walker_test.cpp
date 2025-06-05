/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/pipeline/expression_walker.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/string_map.h"

#include <memory>
#include <string>
#include <type_traits>
#include <typeinfo>
#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

class ExpressionWalkerTest : public AggregationContextFixture {
protected:
    auto jsonToPipeline(StringData jsonArray) {
        const auto inputBson = fromjson("{pipeline: " + jsonArray + "}");

        ASSERT_EQUALS(inputBson["pipeline"].type(), BSONType::array);
        auto rawPipeline = parsePipelineFromBSON(inputBson["pipeline"]);
        NamespaceString testNss =
            NamespaceString::createNamespaceString_forTest("test", "collection");
        auto command = AggregateCommandRequest{testNss, rawPipeline};

        return Pipeline::parse(command.getPipeline(), getExpCtx());
    }

    auto parseExpression(std::string expressionString) {
        return Expression::parseExpression(
            getExpCtxRaw(), fromjson(expressionString), getExpCtx()->variablesParseState);
    }
};

using namespace std::string_literals;
using namespace expression_walker;

TEST_F(ExpressionWalkerTest, NothingTreeWalkSucceedsAndReturnsVoid) {
    struct {
        void postVisit(Expression*) {}
    } nothingWalker;
    auto expression = std::unique_ptr<Expression>{};
    static_assert(
        std::is_same_v<decltype(walk<Expression>(expression.get(), &nothingWalker)), void>);
    walk<Expression>(expression.get(), &nothingWalker);
}

TEST_F(ExpressionWalkerTest, PrintWalkReflectsMutation) {
    struct {
        void preVisit(Expression* expression) {
            if (typeid(*expression) == typeid(ExpressionConcat))
                string += "{$concat: [";
            if (auto constant = dynamic_cast<ExpressionConstant*>(expression))
                string += "\""s + constant->getValue().getString() + "\"";
        }
        void inVisit(unsigned long long, Expression* expression) {
            string += ", ";
        }
        void postVisit(Expression* expression) {
            if (typeid(*expression) == typeid(ExpressionConcat))
                string += "]}";
        }

        std::string string;
    } stringWalker;

    auto expressionString = "{$concat: [\"black\", \"green\", \"yellow\"]}"s;
    auto expression = parseExpression(expressionString);
    walk<Expression>(expression.get(), &stringWalker);
    ASSERT_EQ(stringWalker.string, expressionString);

    struct {
        auto preVisit(Expression* expression) {
            if (auto constant = dynamic_cast<ExpressionConstant*>(expression))
                if (constant->getValue().getString() == "black")
                    return std::make_unique<ExpressionConstant>(expCtx, Value{"white"s});
            return std::unique_ptr<ExpressionConstant>{};
        }
        ExpressionContext* const expCtx;
    } whiteWalker{getExpCtxRaw()};

    auto res = walk<Expression>(expression.get(), &whiteWalker);
    ASSERT_FALSE(res);
    stringWalker.string.clear();
    walk<Expression>(expression.get(), &stringWalker);
    ASSERT_EQ(stringWalker.string, "{$concat: [\"white\", \"green\", \"yellow\"]}"s);
}

TEST_F(ExpressionWalkerTest, RootNodeReplacable) {
    struct {
        auto postVisit(Expression* expression) {
            return std::make_unique<ExpressionConstant>(expCtx, Value{"soup"s});
        }
        ExpressionContext* const expCtx;
    } replaceWithSoup{getExpCtxRaw()};

    auto expressionString = "{$add: [2, 3, 4, {$atan2: [1, 0]}]}"s;
    auto expression = parseExpression(expressionString);
    auto resultExpression = walk<Expression>(expression.get(), &replaceWithSoup);
    ASSERT_VALUE_EQ(dynamic_cast<ExpressionConstant*>(resultExpression.get())->getValue(),
                    Value{"soup"s});
    // The input Expression, as a side effect, will have all its branches changed to soup by this
    // rewrite.
    for (auto&& child : dynamic_cast<ExpressionAdd*>(expression.get())->getChildren())
        ASSERT_VALUE_EQ(dynamic_cast<ExpressionConstant*>(child.get())->getValue(), Value{"soup"s});
}

TEST_F(ExpressionWalkerTest, InVisitCanCount) {
    struct {
        void inVisit(unsigned long long count, Expression*) {
            counter.push_back(count);
        }
        std::vector<unsigned long long> counter;
    } countWalker;

    auto expressionString = "{$and: [true, false, true, true, false, true]}"s;
    auto expression = parseExpression(expressionString);
    walk<Expression>(expression.get(), &countWalker);
    ASSERT(countWalker.counter == std::vector({1ull, 2ull, 3ull, 4ull, 5ull}));
}

TEST_F(ExpressionWalkerTest, ConstPrintWalk) {
    struct {
        void preVisit(const Expression* expression) {
            if (typeid(*expression) == typeid(ExpressionConcat))
                string += "{$concat: [";
            if (auto constant = dynamic_cast<const ExpressionConstant*>(expression))
                string += "\""s + constant->getValue().getString() + "\"";
        }
        void inVisit(unsigned long long, const Expression* expression) {
            string += ", ";
        }
        void postVisit(const Expression* expression) {
            if (typeid(*expression) == typeid(ExpressionConcat))
                string += "]}";
        }

        std::string string;
    } constStringWalker;

    auto expressionString = "{$concat: [\"black\", \"green\", \"yellow\"]}"s;
    auto expression = parseExpression(expressionString);
    walk<const Expression>(expression.get(), &constStringWalker);
    ASSERT_EQ(constStringWalker.string, expressionString);
}

TEST_F(ExpressionWalkerTest, ConstInVisitCanCount) {
    struct {
        void inVisit(unsigned long long count, const Expression*) {
            counter.push_back(count);
        }
        std::vector<unsigned long long> counter;
    } constCountWalker;

    auto expressionString = "{$and: [true, false, true, true, false, true]}"s;
    auto expression = parseExpression(expressionString);
    walk<const Expression>(expression.get(), &constCountWalker);
    ASSERT(constCountWalker.counter == std::vector({1ull, 2ull, 3ull, 4ull, 5ull}));
}

TEST_F(ExpressionWalkerTest, SubstitutePathOnlySubstitutesPrefix) {
    StringMap<std::string> renames{{"a", "b"}};
    SubstituteFieldPathWalker substituteWalker(renames);
    auto expression = parseExpression("{$concat: ['$a', '$b', '$a.a', '$b.a', '$$NOW']}");
    walk<Expression>(expression.get(), &substituteWalker);
    ASSERT_BSONOBJ_EQ(fromjson("{$concat: ['$b', '$b', '$b.a', '$b.a', '$$NOW']}"),
                      expression->serialize().getDocument().toBson());
}

TEST_F(ExpressionWalkerTest, SubstitutePathSubstitutesWhenThereAreDottedFields) {
    StringMap<std::string> renames{{"a.b.c", "x"}, {"c", "q.r"}, {"d.e", "y"}};
    SubstituteFieldPathWalker substituteWalker(renames);
    auto expression = parseExpression("{$concat: ['$a.b', '$a.b.c', '$c', '$d.e.f']}");
    walk<Expression>(expression.get(), &substituteWalker);
    ASSERT_BSONOBJ_EQ(fromjson("{$concat: ['$a.b', '$x', '$q.r', '$y.f']}"),
                      expression->serialize().getDocument().toBson());
}

TEST_F(ExpressionWalkerTest, SubstitutePathSubstitutesWhenExpressionIsNested) {
    StringMap<std::string> renames{{"a.b", "x"}, {"c", "y"}};
    SubstituteFieldPathWalker substituteWalker(renames);
    auto expression =
        parseExpression("{$multiply: [{$add: ['$a.b', '$c']}, {$ifNull: ['$a.b.c', '$d']}]}");
    walk<Expression>(expression.get(), &substituteWalker);
    ASSERT_BSONOBJ_EQ(fromjson("{$multiply: [{$add: ['$x', '$y']}, {$ifNull: ['$x.c', '$d']}]}"),
                      expression->serialize().getDocument().toBson());
}

TEST_F(ExpressionWalkerTest, SubstitutePathDoesNotSubstitutesWhenExpressionHasNoFieldPaths) {
    StringMap<std::string> renames{{"a.b", "x"}, {"c", "y"}};
    SubstituteFieldPathWalker substituteWalker(renames);
    auto expression = parseExpression("{$multiply: [1, 2, 3, 4]}");
    walk<Expression>(expression.get(), &substituteWalker);
    ASSERT_BSONOBJ_EQ(fromjson("{$multiply: [{$const: 1}, {$const: 2}, {$const: 3}, {$const: 4}]}"),
                      expression->serialize().getDocument().toBson());
}

}  // namespace
}  // namespace mongo
