// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/optimizer/join/predicate_extractor.h"

#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/compiler/dependency_analysis/pipeline_dependency_graph.h"
#include "mongo/db/query/compiler/optimizer/join/agg_join_model_fixture.h"
#include "mongo/db/query/compiler/optimizer/join/unit_test_helpers.h"
#include "mongo/unittest/unittest.h"

#include <string_view>

namespace mongo::join_ordering {
using namespace std::literals::string_view_literals;

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
        auto dsc = DocumentSourceMatch::parse(expCtx(), BSON("$match" << filterBson));

        auto splitExprs = splitJoinAndSingleCollectionPredicates(
            dynamic_cast<const DocumentSourceMatch*>(dsc.begin()->get()), letVars);
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
            ASSERT_VALUE_EQ(expectedJoinPred->serialize(), gotJoinPred.serialize());
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

// Verifies that the JoinPredicateExpr's localField() captures any trailing path suffix on the let
// variable reference. The 'expectedJoinPredicates' check above only inspects the serialized
// expression, which is unchanged from the input and so cannot distinguish '$$a' from '$$a.foo' in
// the local field path.
TEST_F(PredicateExtractorTest, LocalFieldIncludesPathSuffixOnLetVariable) {
    std::vector<LetVariable> letVars;
    auto idA = expCtx()->variablesParseState.defineVariable("a");
    {
        auto bson = fromjson("{'': '$x'}");
        auto def = Expression::parseOperand(
            expCtx().get(), bson.firstElement(), expCtx()->variablesParseState);
        letVars.emplace_back("a", def, idA);
    }

    DocumentSourceContainer dsc;
    auto parseMatch = [&](std::string_view json) {
        auto bson = fromjson(json);
        dsc = DocumentSourceMatch::parse(expCtx(), BSON("$match" << bson));
        auto match = dynamic_cast<const DocumentSourceMatch*>(dsc.begin()->get());
        ASSERT_NE(nullptr, match);
        return std::move(match);
    };

    // Variable reference on the right with a path suffix: '$$a.y' must resolve to local path
    // 'x.y' (the let variable's RHS 'x', concatenated with the trailing 'y').
    {
        auto me = parseMatch("{$expr: {$eq: ['$z', '$$a.y']}}");
        auto split = splitJoinAndSingleCollectionPredicates(me, letVars);
        ASSERT(split.has_value());
        ASSERT_EQ(1, split->joinPredicates.size());
        ASSERT_EQ("x.y", split->joinPredicates[0].localField().fullPath());
        ASSERT_EQ("z", split->joinPredicates[0].foreignField().fullPath());
    }

    // Variable reference on the left side; same semantics.
    {
        auto me = parseMatch("{$expr: {$eq: ['$$a.y', '$z']}}");
        auto split = splitJoinAndSingleCollectionPredicates(me, letVars);
        ASSERT(split.has_value());
        ASSERT_EQ(1, split->joinPredicates.size());
        ASSERT_EQ("x.y", split->joinPredicates[0].localField().fullPath());
        ASSERT_EQ("z", split->joinPredicates[0].foreignField().fullPath());
    }

    // Multi-component suffix on the variable reference.
    {
        auto me = parseMatch("{$expr: {$eq: ['$z', '$$a.y.w']}}");
        auto split = splitJoinAndSingleCollectionPredicates(me, letVars);
        ASSERT(split.has_value());
        ASSERT_EQ(1, split->joinPredicates.size());
        ASSERT_EQ("x.y.w", split->joinPredicates[0].localField().fullPath());
    }

    // No suffix.
    {
        auto me = parseMatch("{$expr: {$eq: ['$z', '$$a']}}");
        auto split = splitJoinAndSingleCollectionPredicates(me, letVars);
        ASSERT(split.has_value());
        ASSERT_EQ(1, split->joinPredicates.size());
        ASSERT_EQ("x", split->joinPredicates[0].localField().fullPath());
    }
}

/**
 * Test suite for extractExprPredicates.
 */
class ExtractExprPredicatesTest : public unittest::Test {
public:
    static constexpr auto pipelineStr = R"([
            {$lookup: {from: "first", as: "first", pipeline: []}},
            {$unwind: "$first"},
            {$lookup: {from: "second", as: "second", pipeline: []}},
            {$unwind: "$second"}
        ])";

    ExprPredicatesResult extract(std::string_view json) {
        const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());

        auto pipeline = makePipelineForTest(pipelineStr, {"first", "second"}, expCtx);
        auto bson = fromjson(json);
        auto match = DocumentSourceMatch::create(bson, expCtx);
        // Note: optimization results in $unwinds being absorbed.
        auto* lookup1 = dynamic_cast<DocumentSourceLookUp*>(pipeline->getSources().begin()->get());
        ASSERT_NE(lookup1, nullptr);
        auto* lookup2 = dynamic_cast<DocumentSourceLookUp*>(pipeline->getSources().rbegin()->get());
        ASSERT_NE(lookup2, nullptr);
        pipeline->addFinalSource(match);

        AggJoinModelFixture::markFieldsAsScalar(
            *pipeline,
            {"a", "b", "c", "d"},
            {{"first", {"a", "b", "e"}}, {"second", {"a", "b", "c", "d", "e"}}});

        auto canMainCollPathBeArray = [expCtx](std::string_view path) {
            return expCtx->canPathBeArrayForNss(FieldRef(path), expCtx->getNamespaceString());
        };
        auto graph = pipeline::dependency_graph::DependencyGraph(pipeline->getSources(),
                                                                 canMainCollPathBeArray);
        PathResolver pathResolver(_baseNodeId, graph);

        ASSERT_TRUE(pathResolver.trackEmbedPath(*lookup1, _firstNodeId));
        ASSERT_TRUE(pathResolver.trackEmbedPath(*lookup2, _secondNodeId));

        graph.resize(pipeline->getSources().end());
        return extractExprPredicates(pathResolver, match.get());
    }

protected:
    static constexpr NodeId _baseNodeId = 0;
    static constexpr NodeId _firstNodeId = 1;
    static constexpr NodeId _secondNodeId = 2;
};

/**
 * A join predicate extracted from a single $expr equality expression.
 */
TEST_F(ExtractExprPredicatesTest, SimpleEquality) {
    static constexpr std::string_view json = "{$expr: {$eq: ['$first.a', '$second.b']}}";
    auto result = extract(json);
    ASSERT_TRUE(result.expressionIsFullyAbsorbed);
    ASSERT_EQ(1, result.predicates.size());
}

/**
 * A join predicate cannot be extracted from an equality of the same collection fields.
 */
TEST_F(ExtractExprPredicatesTest, SameCollectionEquality) {
    static constexpr std::string_view json = "{$expr: {$eq: ['$first.a', '$first.b']}}";
    auto result = extract(json);
    ASSERT_FALSE(result.expressionIsFullyAbsorbed);
    ASSERT_EQ(0, result.predicates.size());
}

/**
 * Join predicates extracted from conjuction of $expr equalities.
 */
TEST_F(ExtractExprPredicatesTest, ExpressionAnd) {
    static constexpr std::string_view json =
        "{$expr: {$and: [{$eq: ['$a', '$second.a']}, {$eq: ['$first.b', '$second.b']}]}}";
    auto result = extract(json);
    ASSERT_TRUE(result.expressionIsFullyAbsorbed);
    ASSERT_EQ(2, result.predicates.size());
}

/**
 * No join predicates can be extracted from rooted $or.
 */
TEST_F(ExtractExprPredicatesTest, RootedOr) {
    static constexpr std::string_view json = R"(
        {
            $or: [
                { $expr: { $eq: ["$second.b", "$first.a"] } },
                { $and: [{ $expr: { $eq: ["$second.b", "$a"] } }, 
                         { $expr: { $eq: ["$second.c", "$b"] } } ] 
                },
                { $expr: { $eq: ["$b", "$first.a"] } }
            ]
        })";
    auto result = extract(json);
    ASSERT_FALSE(result.expressionIsFullyAbsorbed);
    ASSERT_EQ(0, result.predicates.size());
}

/**
 * No join predicates can be extracted from rooted $or expression.
 */
TEST_F(ExtractExprPredicatesTest, ExpressionOr) {
    static constexpr std::string_view json =
        "{$expr: {$or: [{$eq: ['$a', '$second.a']}, {$eq: ['$first.b', '$second.b']}]}}";
    auto result = extract(json);
    ASSERT_FALSE(result.expressionIsFullyAbsorbed);
    ASSERT_EQ(0, result.predicates.size());
}

/**
 * Join predicates can be extracted with rooted $and and $nested $or.
 * The whole expression cannot be fully absorbed though.
 */
TEST_F(ExtractExprPredicatesTest, NestedOr) {
    static constexpr std::string_view json = R"(
        {
            $and: [
                { $expr: { $eq: ["$second.b", "$first.a"] } },
                { $or: [{ $expr: { $eq: ["$second.b", "$a"] } }, 
                        { $expr: { $eq: ["$second.c", "$b"] } },
                        { $expr: { $eq: ["$second.d", "$d"] } } ] 
                }
            ]
        })";
    auto result = extract(json);
    ASSERT_FALSE(result.expressionIsFullyAbsorbed);
    ASSERT_EQ(1, result.predicates.size());
}

/**
 * Join predicates extracted from nested conjunctions.
 * This case is possible in a fuzzer with optimization off.
 */
TEST_F(ExtractExprPredicatesTest, NestedAnd) {
    static constexpr std::string_view json = R"(
        {
            $and: [
                { $expr: { $eq: ["$second.b", "$first.a"] } },
                { $and: [{ $expr: { $eq: ["$second.b", "$a"] } }, 
                         { $expr: { $eq: ["$second.c", "$b"] } } ] 
                },
                { $expr: { $eq: ["$b", "$first.a"] } }
            ]
        })";
    auto result = extract(json);
    ASSERT_TRUE(result.expressionIsFullyAbsorbed);
    ASSERT_EQ(4, result.predicates.size());
}

/**
 * An expression that contains a non-equality predicate cannot be fully absorbed.
 */
TEST_F(ExtractExprPredicatesTest, ExpressionAndWithGt) {
    static constexpr std::string_view json = R"(
        {
            $expr: {
                $and: [
                { $eq: ["$second.b", "$first.b"] },
                { $gt: ["$second.e", "$first.e"] },
                { $eq: ["$a", "$second.a"] }
                ]
            }
        })";
    auto result = extract(json);
    ASSERT_FALSE(result.expressionIsFullyAbsorbed);
    ASSERT_EQ(2, result.predicates.size());
}

/**
 * An equality against a bare single-component system variable reference (e.g. '$$NOW', '$$ROOT',
 * '$$CURRENT') cannot be a join predicate and must not crash while extracting predicates. These
 * paths have a single component, so attempting to strip the variable prefix would tassert 16409.
 */
TEST_F(ExtractExprPredicatesTest, EqualityAgainstBareSystemVariable) {
    for (std::string_view json : {"{$expr: {$eq: ['$$NOW', '$first.a']}}"sv,
                                  "{$expr: {$eq: ['$first.a', '$$NOW']}}"sv,
                                  "{$expr: {$eq: ['$$ROOT', '$first.a']}}"sv,
                                  "{$expr: {$eq: ['$first.a', '$$ROOT']}}"sv,
                                  "{$expr: {$eq: ['$$CURRENT', '$first.a']}}"sv,
                                  "{$expr: {$eq: ['$first.a', '$$CURRENT']}}"sv,
                                  "{$expr: {$eq: ['$$NOW', '$$ROOT']}}"sv}) {
        auto result = extract(json);
        ASSERT_FALSE(result.expressionIsFullyAbsorbed) << json;
        ASSERT_EQ(0, result.predicates.size()) << json;
    }
}

/**
 * An equality against a system variable with a subfield (e.g. '$$NOW.x', '$$CLUSTER_TIME.foo')
 * also cannot be a join predicate. The path length > 1 so it passes the bare-variable check, but
 * isVariableReference() is true and stripping the variable name would yield a spurious field path.
 */
TEST_F(ExtractExprPredicatesTest, EqualityAgainstSystemVariableWithSubfield) {
    for (std::string_view json : {"{$expr: {$eq: ['$$NOW.x', '$first.a']}}"sv,
                                  "{$expr: {$eq: ['$first.a', '$$NOW.x']}}"sv,
                                  "{$expr: {$eq: ['$$CLUSTER_TIME.foo', '$first.a']}}"sv,
                                  "{$expr: {$eq: ['$first.a', '$$CLUSTER_TIME.foo']}}"sv,
                                  "{$expr: {$eq: ['$$NOW.x', '$$CLUSTER_TIME.foo']}}"sv}) {
        auto result = extract(json);
        ASSERT_FALSE(result.expressionIsFullyAbsorbed) << json;
        ASSERT_EQ(0, result.predicates.size()) << json;
    }
}

/**
 * An expression that contains non-expr $eq predicate cannot be fully absorbed.
 */
TEST_F(ExtractExprPredicatesTest, MatchNonExprEquality) {
    static constexpr std::string_view json = R"(
        {
            $and: [
                { $expr: { $eq: ["$second.b", "$first.a"] } },
                { a: 2 },
                { $expr: { $eq: ["$b", "$first.a"] } }
            ]
        })";
    auto result = extract(json);
    ASSERT_FALSE(result.expressionIsFullyAbsorbed);
    ASSERT_EQ(2, result.predicates.size());
}
}  // namespace mongo::join_ordering
