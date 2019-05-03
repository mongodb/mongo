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

#include "mongo/platform/basic.h"

#include <string>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/bson/json.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_walker.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class ExpressionWalkerTest : public AggregationContextFixture {
protected:
    auto jsonToPipeline(StringData jsonArray) {
        const auto inputBson = fromjson("{pipeline: " + jsonArray + "}");

        ASSERT_EQUALS(inputBson["pipeline"].type(), BSONType::Array);
        auto rawPipeline =
            uassertStatusOK(AggregationRequest::parsePipelineFromBSON(inputBson["pipeline"]));
        NamespaceString testNss("test", "collection");
        AggregationRequest request(testNss, rawPipeline);

        return uassertStatusOK(Pipeline::parse(request.getPipeline(), getExpCtx()));
    }

    auto parseExpression(std::string expressionString) {
        return Expression::parseExpression(
            getExpCtx(), fromjson(expressionString), getExpCtx()->variablesParseState);
    }
};

using namespace std::string_literals;
using namespace expression_walker;

TEST_F(ExpressionWalkerTest, NullTreeWalkSucceeds) {
    struct {
        boost::intrusive_ptr<Expression> preVisit(boost::intrusive_ptr<Expression>& expression) {
            return expression;
        }
        void inVisit(unsigned long long, boost::intrusive_ptr<Expression>&) {}
        boost::intrusive_ptr<Expression> postVisit(boost::intrusive_ptr<Expression>& expression) {
            return expression;
        }
    } nothingWalker;
    auto expression = boost::intrusive_ptr<Expression>();
    walk(nothingWalker, expression);
}

TEST_F(ExpressionWalkerTest, PrintWalkReflectsMutation) {
    struct {
        boost::intrusive_ptr<Expression> preVisit(boost::intrusive_ptr<Expression>& expression) {
            if (typeid(*expression) == typeid(ExpressionConcat))
                string += "{$concat: [";
            if (auto constant = dynamic_cast<ExpressionConstant*>(expression.get()))
                string += "\""s + constant->getValue().getString() + "\"";
            return expression;
        }
        void inVisit(unsigned long long, boost::intrusive_ptr<Expression>& expression) {
            string += ", ";
        }
        boost::intrusive_ptr<Expression> postVisit(boost::intrusive_ptr<Expression>& expression) {
            if (typeid(*expression) == typeid(ExpressionConcat))
                string += "]}";
            return expression;
        }

        std::string string;
    } stringWalker;

    auto expressionString = "{$concat: [\"black\", \"green\", \"yellow\"]}"s;
    auto expression = parseExpression(expressionString);
    walk(stringWalker, expression);
    ASSERT_EQ(stringWalker.string, expressionString);

    struct {
        boost::intrusive_ptr<Expression> preVisit(boost::intrusive_ptr<Expression>& expression) {
            if (auto constant = dynamic_cast<ExpressionConstant*>(expression.get()))
                if (constant->getValue().getString() == "black")
                    return ExpressionConstant::create(expCtx, Value("white"s));
            return expression;
        }
        void inVisit(unsigned long long, boost::intrusive_ptr<Expression>& expression) {}
        boost::intrusive_ptr<Expression> postVisit(boost::intrusive_ptr<Expression>& expression) {
            return expression;
        }
        const boost::intrusive_ptr<ExpressionContext>& expCtx;
    } whiteWalker{getExpCtx()};

    walk(whiteWalker, expression);
    stringWalker.string.clear();
    walk(stringWalker, expression);
    ASSERT_EQ(stringWalker.string, "{$concat: [\"white\", \"green\", \"yellow\"]}"s);
}

TEST_F(ExpressionWalkerTest, InVisitCanCount) {
    struct {
        boost::intrusive_ptr<Expression> preVisit(boost::intrusive_ptr<Expression>& expression) {
            return expression;
        }
        void inVisit(unsigned long long count, boost::intrusive_ptr<Expression>&) {
            counter.push_back(count);
        }
        boost::intrusive_ptr<Expression> postVisit(boost::intrusive_ptr<Expression>& expression) {
            return expression;
        }
        std::vector<unsigned long long> counter;
    } countWalker;

    auto expressionString = "{$and: [true, false, true, true, false, true]}"s;
    auto expression = parseExpression(expressionString);
    walk(countWalker, expression);
    ASSERT(countWalker.counter == std::vector({1ull, 2ull, 3ull, 4ull, 5ull}));
}

}  // namespace
}  // namespace mongo
