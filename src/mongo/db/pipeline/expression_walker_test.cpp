// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/expression_walker.h"

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
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/string_map.h"

#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeinfo>
#include <vector>


namespace mongo {
namespace {

class ExpressionWalkerTest : public AggregationContextFixture {
protected:
    auto jsonToPipeline(std::string_view jsonArray) {
        const auto inputBson = fromjson("{pipeline: " + std::string{jsonArray} + "}");

        ASSERT_EQUALS(inputBson["pipeline"].type(), BSONType::array);
        auto rawPipeline = parsePipelineFromBSON(inputBson["pipeline"]);
        NamespaceString testNss =
            NamespaceString::createNamespaceString_forTest("test", "collection");
        auto command = AggregateCommandRequest{testNss, rawPipeline};

        return pipeline_factory::makePipeline(
            command.getPipeline(), getExpCtx(), pipeline_factory::kOptionsMinimal);
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
