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

#include "mongo/db/query/compiler/optimizer/join/predicate_extractor.h"

#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo::join_ordering {

class PredicateExtractorTest : public unittest::Test {
public:
    void setUp() final {
        _expCtx = new ExpressionContextForTest();
    }

    auto expCtx() {
        return _expCtx;
    }

    struct VariableDef {
        // Identifier of the variable
        std::string name;
        // Definition of the variable (RHS) which will be parsed as agg expression
        // For example, "$a", "$$NOW", "5", etc.
        std::string def;
    };

    struct TestCase {
        std::string expr;
        bool canSplit;
        std::vector<VariableDef> variables{
            {"a", "'$a'"}, {"b", "'$b'"}, {"c", "'$c'"}, {"d", "'$d'"}};
        std::vector<std::string> expectedJoinPredicates;
        std::string expectedSingleTablePredicates;
    };

    void assertSplit(const TestCase& tc) {
        // Assert testcase is well-formed
        if (!tc.canSplit) {
            ASSERT(tc.expectedJoinPredicates.empty());
            ASSERT(tc.expectedSingleTablePredicates.empty());
        }

        // Define variables so MatchExpression parser succeeds.
        std::vector<LetVariable> letVars;
        for (auto&& var : tc.variables) {
            auto id = expCtx()->variablesParseState.defineVariable(var.name);
            // All simulating references to let variables defined in aggregate command. These just
            // need a declaration in 'variablesParseState' but no entry in let variables.
            if (var.def.empty()) {
                continue;
            }
            auto bson = fromjson(str::stream{} << "{'': " << var.def << "}");
            auto varDef = Expression::parseOperand(
                expCtx().get(), bson.firstElement(), expCtx()->variablesParseState);
            letVars.emplace_back(var.name, varDef, id);
        }

        auto filterBson = fromjson(tc.expr);
        auto swMatchExpression =
            MatchExpressionParser::parse(filterBson,
                                         expCtx(),
                                         ExtensionsCallbackNoop(),
                                         MatchExpressionParser::kAllowAllSpecialFeatures);
        ASSERT_OK(swMatchExpression);

        auto splitExprs =
            splitJoinAndSingleCollectionPredicates(swMatchExpression.getValue().get(), letVars);
        if (!tc.canSplit) {
            ASSERT(!splitExprs.has_value());
            return;
        }
        ASSERT(splitExprs.has_value());

        ASSERT_EQ(tc.expectedJoinPredicates.size(), splitExprs->joinPredicates.size());
        size_t i{0};
        for (auto&& joinPredStr : tc.expectedJoinPredicates) {
            auto joinPredBson = fromjson(joinPredStr);
            auto expectedJoinPred = Expression::parseExpression(
                expCtx().get(), joinPredBson, expCtx()->variablesParseState);
            auto gotJoinPred = splitExprs->joinPredicates[i];
            ASSERT_VALUE_EQ(expectedJoinPred->serialize(), gotJoinPred->serialize());
            ++i;
        }

        auto stpBson = fromjson(tc.expectedSingleTablePredicates);
        auto swExpectedMatchExpression =
            MatchExpressionParser::parse(stpBson,
                                         expCtx(),
                                         ExtensionsCallbackNoop(),
                                         MatchExpressionParser::kAllowAllSpecialFeatures);

        ASSERT_OK(swExpectedMatchExpression);

        // Avoid calling equivalent with nullptr. In that case, there is no single table predicate
        if (splitExprs->singleTablePredicates) {
            ASSERT(swExpectedMatchExpression.getValue()->equivalent(
                splitExprs->singleTablePredicates.get()));
        } else {
            ASSERT(swExpectedMatchExpression.getValue()->isTriviallyTrue());
        }
    }

private:
    boost::intrusive_ptr<ExpressionContext> _expCtx;
};

TEST_F(PredicateExtractorTest, SimpleSplit) {
    assertSplit({
        .expr = "{a: 5}",
        .canSplit = true,
        .expectedSingleTablePredicates = "{a: 5}",
    });

    assertSplit({
        .expr = "{$expr: {$eq: ['$a', '$$b']}}",
        .canSplit = true,
        .expectedJoinPredicates = {"{$eq: ['$a', '$$b']}"},
    });

    assertSplit({
        .expr = "{a: 5, $expr: {$eq: ['$a', '$$b']}}",
        .canSplit = true,
        .expectedJoinPredicates = {"{$eq: ['$a', '$$b']}"},
        .expectedSingleTablePredicates = "{a: 5}",
    });

    assertSplit({
        .expr = "{a: 5, $expr: {$eq: ['$a', '$b']}}",
        .canSplit = true,
        .expectedSingleTablePredicates = "{a : 5, $expr : {$eq : [ '$a', '$b' ]}} ",
    });

    assertSplit({
        .expr = "{a: 5, $expr: {$eq: ['$$b', '$a']}}",
        .canSplit = true,
        .expectedJoinPredicates = {"{$eq: ['$$b', '$a']}"},
        .expectedSingleTablePredicates = "{a: 5}",
    });
}

TEST_F(PredicateExtractorTest, MultipleJoinPredicates) {
    // Agg expression conjunction
    assertSplit({
        .expr = "{a:5, $expr: {$and: [{$eq: ['$a', '$$b']}, {$eq: ['$c', '$$d']}]}}",
        .canSplit = true,
        .expectedJoinPredicates = {"{$eq: ['$a', '$$b']}", "{$eq: ['$c', '$$d']}"},
        .expectedSingleTablePredicates = "{a: 5}",
    });

    // Match expresssion conjunction
    assertSplit({
        .expr = "{$and: [{a: 5}, {$expr: {$eq: ['$a', '$$b']}}, {$expr: {$eq: ['$c', '$$d']}}]}",
        .canSplit = true,
        .expectedJoinPredicates = {"{$eq: ['$a', '$$b']}", "{$eq: ['$c', '$$d']}"},
        .expectedSingleTablePredicates = "{a: 5}",
    });

    // Nested conjunction join predicates
    assertSplit({
        .expr = R"(
            {$and: [
                {$and: [{$expr: {$eq: ['$a', '$$a']}}, {$expr: {$eq: ['$b', '$$b']}}]},
                {a: 5},
                {$expr: {$eq: ['$c', '$$c']}}
            ]}
        )",
        .canSplit = true,
        .expectedJoinPredicates = {"{$eq: ['$a', '$$a']}",
                                   "{$eq: ['$b', '$$b']}",
                                   "{$eq: ['$c', '$$c']}"},
        .expectedSingleTablePredicates = "{a: 5}",
    });

    // We don't attempt to deduplicate join predicates. This will occur when we insert them into
    // the join graph.
    assertSplit({
        .expr = "{$expr: {$and: [{$eq: ['$a', '$$b']}, {$eq: ['$$b', '$a']}]}}",
        .canSplit = true,
        .expectedJoinPredicates = {"{$eq: ['$a', '$$b']}", "{$eq: ['$$b', '$a']}"},
    });

    assertSplit({
        .expr = "{$expr: {$and: [{$eq: ['$a', '$$b']}, {$eq: ['$a', 5]}]}}",
        .canSplit = true,
        .expectedJoinPredicates = {"{$eq: ['$a', '$$b']}"},
        .expectedSingleTablePredicates = "{$expr: {$eq: ['$a', 5]}}",
    });
}

TEST_F(PredicateExtractorTest, DisjunctiveJoinPredicates) {
    assertSplit({
        .expr = "{$or: [{$expr: {$eq: ['$a', '$$b']}}, {$expr: {$eq: ['$c', '$$d']}}]}",
        .canSplit = false,
    });

    assertSplit({
        .expr = "{$nor: [{$expr: {$eq: ['$a', '$$b']}}, {$expr: {$eq: ['$c', '$$d']}}]}",
        .canSplit = false,
    });

    assertSplit({
        .expr = "{$expr: {$or: [{$eq: ['$a', '$$b']}, {$eq: ['$c', '$$d']}]}}",
        .canSplit = false,
    });

    assertSplit({
        .expr = R"(
            {$expr: {
                $and: [
                    {$eq: ['$a', '$$a']},
                    {$or: [
                        {$eq: ['$b', '$$b']},
                        {$eq: ['$c', '$$c']}
                    ]}
                ]
            }})",
        .canSplit = false,
    });

    assertSplit({
        .expr = R"(
            {
                $expr: {$eq: ['$a', '$$a']},
                $or: [
                    {$expr: {$eq: ['$b', '$$b']}},
                    {$expr: {$eq: ['$c', '$$c']}}
                ]
            })",
        .canSplit = false,
    });
}

TEST_F(PredicateExtractorTest, NegatedJoinPredicates) {
    assertSplit({
        .expr = "{$nor: [{$expr: {$eq: ['$a', '$$b']}}, {$expr: {$eq: ['$c', '$$d']}}]}",
        .canSplit = false,
    });

    assertSplit({
        .expr = "{a: 5, $nor: [{$expr: {$eq: ['$a', '$$b']}}]}",
        .canSplit = false,
    });

    assertSplit({
        .expr = "{$expr: {$not: {$eq: ['$a', '$$b']}}}",
        .canSplit = false,
    });
}

TEST_F(PredicateExtractorTest, EqualityOnSystemVariables) {
    assertSplit({
        .expr = "{$expr: {$eq: ['$a', '$$ROOT']}}",
        .canSplit = true,
        .expectedSingleTablePredicates = "{$expr: {$eq: ['$a', '$$ROOT']}}",
    });

    assertSplit({
        .expr = "{$expr: {$eq: ['$a', '$$NOW']}}",
        .canSplit = true,
        .expectedSingleTablePredicates = "{$expr: {$eq: ['$a', '$$NOW']}}",
    });

    assertSplit({
        .expr = "{$expr: {$eq: ['$a', '$$CLUSTER_TIME']}}",
        .canSplit = true,
        .expectedSingleTablePredicates = "{$expr: {$eq: ['$a', '$$CLUSTER_TIME']}}",

    });

    assertSplit({
        .expr = "{$expr: {$eq: ['$a', '$$USER_ROLES']}}",
        .canSplit = true,
        .expectedSingleTablePredicates = "{$expr: {$eq: ['$a', '$$USER_ROLES']}}",
    });

    assertSplit({
        .expr = "{$expr: {$eq: ['$$a', '$$ROOT']}}",
        .canSplit = false,
    });

    assertSplit({
        .expr = "{$expr: {$eq: ['$$ROOT', '$$a']}}",
        .canSplit = false,
    });
}

TEST_F(PredicateExtractorTest, NonEquiJoinPredicates) {
    assertSplit({
        .expr = "{a: 5, $expr: {$eq: ['$$a', '$$b']}}",
        .canSplit = false,
    });

    assertSplit({
        .expr = "{a: 5, $expr: {$eq: ['$$b', 5]}}",
        .canSplit = false,
    });

    assertSplit({
        .expr = "{a: 5, $expr: {$gt: ['$a', '$$a']}}",
        .canSplit = false,
    });
}

TEST_F(PredicateExtractorTest, ComplexSingleTablePredicates) {
    assertSplit({
        .expr = "{$or: [{$nor: [{a: 5, b: 6}]}, {c: 7}], d: 8}",
        .canSplit = true,
        .expectedSingleTablePredicates = "{$or: [{$nor: [{a: 5, b: 6}]}, {c: 7}], d: 8}",
    });

    assertSplit({
        .expr = "{$where: 'this.a==\"foo\"'}",
        .canSplit = true,
        .expectedSingleTablePredicates = "{$where: 'this.a==\"foo\"'}",
    });

    assertSplit({
        .expr = "{$expr: {$and: [{$gt: ['$a', '$b']}, {$eq: ['$c', 5]}]}}",
        .canSplit = true,
        .expectedSingleTablePredicates = "{$expr: {$and: [{$gt: ['$a', '$b']}, {$eq: ['$c', 5]}]}}",
    });
}

TEST_F(PredicateExtractorTest, LetVariablesNotFieldPaths) {
    assertSplit({
        .expr = "{$expr: {$eq: ['$a', '$$b']}}",
        .canSplit = false,
        .variables = {{.name = "b", .def = "5"}},
    });

    assertSplit({
        .expr = "{$expr: {$eq: ['$a', '$$b']}}",
        .canSplit = false,
        .variables = {{.name = "b", .def = "'$$ROOT'"}},
    });

    assertSplit({
        .expr = "{$expr: {$eq: ['$a', '$$b']}}",
        .canSplit = false,
        .variables = {{.name = "b", .def = "'$$NOW'"}},
    });

    // TODO SERVER-115652: This should be splittable because the variable 'b' is a constant.
    assertSplit({
        .expr = "{$expr: {$eq: ['$a', '$$b']}}",
        .canSplit = false,
        .variables = {{.name = "b", .def = "{$add: ['$b', 5]}"}},
    });

    assertSplit({
        .expr = "{$expr: {$eq: ['$a', '$$a']}}",
        .canSplit = false,
        // No definition for "a". This simulates the top-level aggregation command defining a
        // variable.
        .variables = {{.name = "a"}},
    });
}

TEST_F(PredicateExtractorTest, PredicateWithLetExpr) {
    // TODO SERVER-115652: This should be splittable as the $let defines a new scope and shadows the
    // definition of $$b variable with a constant.
    assertSplit({
        .expr = "{$expr: {$let: {vars: {b: 5}, in: {$eq: ['$a', '$$b']}}}}",
        .canSplit = false,
        .variables = {{.name = "b", .def = "'$b'"}},
    });

    // TODO SERVER-115652: This should be splittable into a join and residual predicate.
    assertSplit({
        .expr = R"(
            {$expr: {$let: {
                vars: {c: 5},
                in: {$and: [{$eq: ['$a', '$$a']}, {$eq: ['$b', 5]}]}
            }}})",
        .canSplit = false,
        .variables = {{.name = "a", .def = "'$a'"}},
    });
}

TEST_F(PredicateExtractorTest, JoinPredicatesOverDottedFields) {
    assertSplit({
        .expr = "{$expr: {$eq: ['$a.foo', '$$a']}}",
        .canSplit = true,
        .expectedJoinPredicates = {"{$eq: ['$a.foo', '$$a']}"},
    });

    assertSplit({
        .expr = "{$expr: {$eq: ['$a', '$$a.foo']}}",
        .canSplit = true,
        .expectedJoinPredicates = {"{$eq: ['$a', '$$a.foo']}"},
    });

    assertSplit({
        .expr = "{$expr: {$eq: ['$a.foo', '$$a.foo']}}",
        .canSplit = true,
        .expectedJoinPredicates = {"{$eq: ['$a.foo', '$$a.foo']}"},
    });
}

}  // namespace mongo::join_ordering
