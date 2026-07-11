// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/optimizer/join/agg_join_model.h"

#include "mongo/db/query/compiler/optimizer/join/agg_join_model_fixture.h"
#include "mongo/db/query/compiler/parsers/matcher/parsed_match_expression_for_test.h"
#include "mongo/unittest/golden_test.h"
#include "mongo/unittest/unittest.h"

#include <string_view>

namespace mongo::join_ordering {
namespace {

using namespace std::literals::string_view_literals;

unittest::GoldenTestConfig goldenTestConfig{"src/mongo/db/test_output/query/join/agg_join_model"};

using mongo::ParsedMatchExpressionForTest;
using PipelineAnalyzerTest = AggJoinModelFixture;

std::vector<std::string> sortedAndChildStrings(const MatchExpression* expr) {
    ASSERT_EQ(expr->matchType(), MatchExpression::AND);
    auto* andExpr = static_cast<const AndMatchExpression*>(expr);
    size_t start = 0;
    if (andExpr->numChildren() > 0 &&
        andExpr->getChild(0)->matchType() == MatchExpression::EXPRESSION) {
        // Treat leading $expr child as redundant and ignore it.
        start = 1;
    }
    std::vector<std::string> out;
    out.reserve(andExpr->numChildren());
    for (size_t i = start; i < andExpr->numChildren(); ++i) {
        out.push_back(andExpr->getChild(i)->toString());
    }
    std::sort(out.begin(), out.end());
    return out;
}

TEST_F(PipelineAnalyzerTest, InferSingleTablePredicateOnSameField) {
    // For join A.a = B.a and STP B.a = 3, we can infer access path A.a = 3
    auto query = R"([
    {
        $lookup: {
            from: "B",
            localField: "a",
            foreignField: "a",
            pipeline: [{$match: {a: 3}}],
            as: "final"
        }
    },
    { $unwind: "$final" }
    ])";

    auto pipeline = makePipeline(query, {"B"});
    markFieldsAsScalar(*pipeline, {"a"sv}, {{"B", {"a"sv}}});
    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));
    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);
    auto& joinModel = swJoinModel.getValue();
    auto& joinGraph = joinModel.getGraph();

    ASSERT_EQ(joinGraph.numNodes(), 2);

    auto* baseCollCQ = joinGraph.accessPathAt((NodeId)0);
    ASSERT_EQ(baseCollCQ->nss().coll(), "pipeline_test");
    auto expectedChildren = "{ a: { $eq: 3 } }";
    ASSERT_EQ(expectedChildren, baseCollCQ->getPrimaryMatchExpression()->toString());

    auto* bCollCQ = joinGraph.accessPathAt((NodeId)1);
    ASSERT_EQ(bCollCQ->nss().coll(), "B");
    expectedChildren = "{ a: { $eq: 3 } }";
    ASSERT_EQ(expectedChildren, bCollCQ->getPrimaryMatchExpression()->toString());
}

TEST_F(PipelineAnalyzerTest, InferSingleTablePredicateOnDiffField) {
    // For join A.a = B.b and STP B.b = 3, we can infer access path A.a = 3
    auto query = R"([
    {
        $lookup: {
            from: "B",
            localField: "a",
            foreignField: "b",
            pipeline: [{$match: {b: 3}}],
            as: "final"
        }
    },
    { $unwind: "$final" }
    ])";

    auto pipeline = makePipeline(query, {"B"});
    markFieldsAsScalar(*pipeline, {"a"sv}, {{"B", {"b"sv}}});
    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));
    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);
    auto& joinModel = swJoinModel.getValue();
    auto& joinGraph = joinModel.getGraph();

    ASSERT_EQ(joinGraph.numNodes(), 2);

    auto* baseCollCQ = joinGraph.accessPathAt((NodeId)0);
    ASSERT_EQ(baseCollCQ->nss().coll(), "pipeline_test");
    auto expectedChildren = "{ a: { $eq: 3 } }";
    ASSERT_EQ(expectedChildren, baseCollCQ->getPrimaryMatchExpression()->toString());

    auto* bCollCQ = joinGraph.accessPathAt((NodeId)1);
    ASSERT_EQ(bCollCQ->nss().coll(), "B");
    expectedChildren = "{ b: { $eq: 3 } }";
    ASSERT_EQ(expectedChildren, bCollCQ->getPrimaryMatchExpression()->toString());

    // For join A.a = B.b and STP A.a = 100, we can infer access path B.b = 100.
    query = R"([
        { $match: { a: 100 } },
        {
            $lookup: {
                from: "B",
                localField: "a",
                foreignField: "b",
                as: "joined"
            }
        },
        { $unwind: "$joined" }
    ])";

    pipeline = makePipeline(query, {"B"});
    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));
    swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);
    auto& joinModel2 = swJoinModel.getValue();
    auto& joinGraph2 = joinModel2.getGraph();
    ASSERT_EQ(joinGraph2.numNodes(), 2);

    baseCollCQ = joinGraph2.accessPathAt((NodeId)0);
    ASSERT_EQ(baseCollCQ->nss().coll(), "pipeline_test");
    expectedChildren = "{ a: { $eq: 100 } }";
    ASSERT_EQ(expectedChildren, baseCollCQ->getPrimaryMatchExpression()->toString());

    bCollCQ = joinGraph2.accessPathAt((NodeId)1);
    ASSERT_EQ(bCollCQ->nss().coll(), "B");
    expectedChildren = "{ b: { $eq: 100 } }";
    ASSERT_EQ(expectedChildren, bCollCQ->getPrimaryMatchExpression()->toString());
}

TEST_F(PipelineAnalyzerTest, PropagateSomeButNotAllSTPs) {
    // For join A.a = B.b and STPs A.x = 100, B.b = 3, we propagate A.a = 3 but *not*  B.x = 100.
    const auto query = R"([
        { $match: { x: 100 } },
        {
            $lookup: {
                from: "B",
                localField: "a",
                foreignField: "b",
                pipeline: [{$match: {b: 3}}],
                as: "joined"
            }
        },
        { $unwind: "$joined" }
    ])";
    auto pipeline = makePipeline(query, {"B"});
    markFieldsAsScalar(*pipeline, {"a"sv}, {{"B", {"b"sv}}});
    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));
    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);
    auto& joinModel = swJoinModel.getValue();
    auto& joinGraph = joinModel.getGraph();

    const auto* baseCollCQ = joinGraph.accessPathAt((NodeId)0);
    ASSERT_EQ(baseCollCQ->nss().coll(), "pipeline_test");
    auto expectedChildren = "{ $and: [ { a: { $eq: 3 } }, { x: { $eq: 100 } } ] }";
    ASSERT_EQ(expectedChildren, baseCollCQ->getPrimaryMatchExpression()->toString());

    const auto* bCollCQ = joinGraph.accessPathAt((NodeId)1);
    ASSERT_EQ(bCollCQ->nss().coll(), "B");
    expectedChildren = "{ b: { $eq: 3 } }";
    ASSERT_EQ(expectedChildren, bCollCQ->getPrimaryMatchExpression()->toString());
}

TEST_F(PipelineAnalyzerTest, PropagateSTPsThruJoinChain) {
    // For joins A.a = B.a, A.b = B.b and C.a = B.a and STP B.a = 3 and B.b = 4, we
    // can infer A.a = 3, A.b = 4 and C.a = 3.
    const auto query = R"([
    {
        $lookup: {
            from: "B",
            as: "joined",
            let: { a_val: "$a", b_val: "$b" },
            pipeline: [
                {
                    $match: {
                        $expr: {
                            $and: [
                                { $eq: ["$a", "$$a_val"] },
                                { $eq: ["$b", "$$b_val"] },
                                { $eq: ["$a", 3] },
                                { $eq: ["$b", 4] }
                            ]
                        }
                    }
                }
            ]
        }
    },
    { $unwind: "$joined" },
    {
        $lookup: {
            from: "C",
            localField: "joined.a",
            foreignField: "a",
            as: "final"
        }
    },
    { $unwind: "$final" }
    ])";

    auto pipeline = makePipeline(query, {"B", "C"});
    markFieldsAsScalar(*pipeline, {"a"sv, "b"sv}, {{"B", {"a"sv, "b"sv}}, {"C", {"a"sv}}});
    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));
    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);
    auto& joinModel = swJoinModel.getValue();
    auto& joinGraph = joinModel.getGraph();
    ASSERT_EQ(joinGraph.numNodes(), 3);

    const auto* baseCollCQ = joinGraph.accessPathAt((NodeId)0);
    ASSERT_EQ(baseCollCQ->nss().coll(), "pipeline_test");
    auto expectedChildren =
        "{ $and: [ { $expr: { $eq: [ \"$a\", { $const: 3 } ] } }, { $expr: { $eq: [ \"$b\", { "
        "$const: 4 } ] } }, { a: { $_internalExprEq: 3 } }, { b: { $_internalExprEq: 4 } } ] }";

    ASSERT_EQ(expectedChildren, baseCollCQ->getPrimaryMatchExpression()->toString());

    const auto* bCollCQ = joinGraph.accessPathAt((NodeId)1);
    ASSERT_EQ(bCollCQ->nss().coll(), "B");
    expectedChildren =
        "{ $and: [ { $expr: { $and: [ { $eq: [ \"$a\", { $const: 3 } ] }, { $eq: [ \"$b\", { "
        "$const: 4 } ] } ] } }, { a: { $_internalExprEq: 3 } }, { b: { $_internalExprEq: 4 } } ] }";
    ASSERT_EQ(expectedChildren, bCollCQ->getPrimaryMatchExpression()->toString());

    const auto* cCollCQ = joinGraph.accessPathAt((NodeId)2);
    ASSERT_EQ(cCollCQ->nss().coll(), "C");
    expectedChildren =
        "{ $and: [ { $expr: { $eq: [ \"$a\", { $const: 3 } ] } }, { a: { $_internalExprEq: 3 } } ] "
        "}";
    ASSERT_EQ(expectedChildren, cCollCQ->getPrimaryMatchExpression()->toString());
}

TEST_F(PipelineAnalyzerTest, JoinChainWithPartialSTPPropagation) {
    /**
     * For joins B.b = A.a, B.x = A.x and STPs A.y = 5, B.x = 100 and B.p = 10 and B.m = 500, we
     * propagate A.x = 100.
     *
     * For joins C.c = joinedB.b, C.x = joinedB.x, C.n = B.m and STP C.q = 42, we propagate C.x
     * 100 and C.n = 500.
     *
     * For join D.d = joinedC.c and join D.o = joinedC.n and STP D.r ∈ {7,8}, we propagate D.d =
     * 500.
     */
    const auto query = R"([
        { $match: { y: 5 } },
        {
            $lookup: {
                from: "B",
                let: { a_val: "$a", x_val: "$x" },
                pipeline: [
                    { $match: {
                        $expr: {
                            $and: [
                                { $eq: ["$b", "$$a_val"] },
                                { $eq: ["$x", "$$x_val"] },
                                { $eq: ["$x", 100] },
                                { $eq: ["$p", 10] },
                                { $eq: ["$m", 500]}
                            ]
                        }
                    }}
                ],
                as: "joinedB"
            }
        },
        { $unwind: "$joinedB" },
        {
            $lookup: {
                from: "C",
                let: { b_val: "$joinedB.b", x_val: "$joinedB.x", m_val: "$joinedB.m" },
                pipeline: [
                    { $match: {
                        $expr: {
                            $and: [
                                { $eq: ["$c", "$$b_val"] },
                                { $eq: ["$x", "$$x_val"] },
                                { $eq: ["$n", "$$m_val"] },
                                { $eq: ["$q", 42] }
                            ]
                        }
                    }}
                ],
                as: "joinedC"
            }
        },
        { $unwind: "$joinedC" },
        {
            $lookup: {
                from: "D",
                let: { c_val: "$joinedC.c", n_val: "$joinedC.n" },
                pipeline: [
                    { $match: {
                        $expr: {
                            $and: [
                                { $eq: ["$d", "$$c_val"] },
                                { $eq: ["$o", "$$n_val"] },
                                { $or: [
                                    { $eq: ["$r", 7] },
                                    { $eq: ["$r", 8] }
                                ] }
                            ]
                        }
                    }}
                ],
                as: "joinedD"
            }
        },
        { $unwind: "$joinedD" }
    ])";

    auto pipeline = makePipeline(query, {"B", "C", "D"});
    markFieldsAsScalar(
        *pipeline,
        {"a"sv, "x"sv},
        {{"B", {"b"sv, "x"sv, "m"sv}}, {"C", {"c"sv, "x"sv, "n"sv}}, {"D", {"d"sv, "o"sv}}});
    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));
    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);
    auto& joinModel = swJoinModel.getValue();
    auto& joinGraph = joinModel.getGraph();
    ASSERT_EQ(joinGraph.numNodes(), 4);

    auto* baseCollCQ = joinGraph.accessPathAt((NodeId)0);
    ASSERT_EQ(baseCollCQ->nss().coll(), "pipeline_test");
    auto expectedChildren =
        "{ $and: [ { y: { $eq: 5 } }, { $expr: { $eq: [ \"$x\", { $const: 100 } ] } }, { x: { "
        "$_internalExprEq: 100 } } ] }";
    ASSERT_EQ(expectedChildren, baseCollCQ->getPrimaryMatchExpression()->toString());

    auto* bCollCQ = joinGraph.accessPathAt((NodeId)1);
    ASSERT_EQ(bCollCQ->nss().coll(), "B");
    expectedChildren =
        "{ $and: [ { $expr: { $and: [ { $eq: [ \"$x\", { $const: 100 } ] }, { $eq: [ \"$p\", { "
        "$const: 10 } ] }, { $eq: [ \"$m\", { $const: 500 } ] } ] } }, { m: { $_internalExprEq: "
        "500 } }, { p: { $_internalExprEq: 10 } }, { x: { $_internalExprEq: 100 } } ] }";
    ASSERT_EQ(expectedChildren, bCollCQ->getPrimaryMatchExpression()->toString());

    auto* cCollCQ = joinGraph.accessPathAt((NodeId)2);
    ASSERT_EQ(cCollCQ->nss().coll(), "C");
    std::vector<std::string> expectedChildrenVec = {
        "{ $expr: { $eq: [ \"$q\", { $const: 42 } ] } }",
        "{ $expr: { $eq: [ \"$x\", { $const: 100 } ] } }",
        "{ n: { $_internalExprEq: 500 } }",
        "{ q: { $_internalExprEq: 42 } }",
        "{ x: { $_internalExprEq: 100 } }"};
    ASSERT_EQ(expectedChildrenVec, sortedAndChildStrings(cCollCQ->getPrimaryMatchExpression()));

    auto* dCollCQ = joinGraph.accessPathAt((NodeId)3);
    ASSERT_EQ(dCollCQ->nss().coll(), "D");
    expectedChildren =
        "{ $and: [ { $or: [ { r: { $_internalExprEq: 7 } }, { r: { $_internalExprEq: 8 } } ] }, { "
        "$expr: { $or: [ { $eq: [ \"$r\", { $const: 7 } ] }, { $eq: [ \"$r\", { $const: 8 } ] } ] "
        "} }, { $expr: { $eq: [ \"$o\", { $const: 500 } ] } }, { o: { $_internalExprEq: 500 } } ] "
        "}";
    ASSERT_EQ(expectedChildren, dCollCQ->getPrimaryMatchExpression()->toString());
}

TEST_F(PipelineAnalyzerTest, DoNotPropagateOrNorNinSingleTablePredicates) {
    // For the join A.joinKey1 = B.joinKey1 and STP B.joinKey1 ∈ {10,20} - we do not
    // propagate the STP to A because the STP is an $or.
    // For the join B.joinKey2 = C.joinKey2 and STP C.joinKey2 ∉ {400, 500, 600}
    // we do not propagate the STP to B because the STP is a $nor.
    const auto query = R"([
    {
        $match: {
        $nor: [
            { joinKey1: 10 },
            { joinKey1: 20 }
        ]
        }
    },
    {
        $lookup: {
        from: "B",
        localField: "joinKey1",
        foreignField: "joinKey1",
        as: "B",
        pipeline: [
            {
            $match: {
                $expr: {
                $and: [
                    { $eq: ["$a", true] },
                    {
                    $or: [
                        { $eq: ["$joinKey2", 1] },
                        { $eq: ["$joinKey2", -1] }
                    ]
                    }
                ]
                }
            }
            }
        ]
        }
    },
    { $unwind: "$B" },
    {
        $lookup: {
        from: "C",
        localField: "B.joinKey2",
        foreignField: "joinKey2",
        as: "C",
        pipeline: [
            {
            $match: {
                joinKey2: { $nin: [400, 500, 600] }
            }
            }
        ]
        }
    },
    { $unwind: "$C" }
    ])";

    auto pipeline = makePipeline(query, {"B", "C"});

    markFieldsAsScalar(*pipeline,
                       {"joinKey1"sv, "joinKey2"sv},
                       {{"B", {"joinKey1"sv, "joinKey2"sv}}, {"C", {"joinKey2"sv}}});

    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));
    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);
    auto& joinModel = swJoinModel.getValue();
    auto& joinGraph = joinModel.getGraph();
    ASSERT_EQ(joinGraph.numNodes(), 3);

    auto* baseCollCQ = joinGraph.accessPathAt((NodeId)0);
    ASSERT_EQ(baseCollCQ->nss().coll(), "pipeline_test");
    auto expectedChildren = "{ $nor: [ { joinKey1: { $eq: 10 } }, { joinKey1: { $eq: 20 } } ] }";
    ASSERT_EQ(expectedChildren, baseCollCQ->getPrimaryMatchExpression()->toString());

    auto* bCollCQ = joinGraph.accessPathAt((NodeId)1);
    ASSERT_EQ(bCollCQ->nss().coll(), "B");
    expectedChildren =
        "{ $and: [ { $or: [ { joinKey2: { $_internalExprEq: 1 } }, { joinKey2: { $_internalExprEq: "
        "-1 } } ] }, { $expr: { $and: [ { $eq: [ \"$a\", { $const: true } ] }, { $or: [ { $eq: [ "
        "\"$joinKey2\", { $const: 1 } ] }, { $eq: [ \"$joinKey2\", { $const: -1 } ] } ] } ] } }, { "
        "a: { $_internalExprEq: true } } ] }";
    ASSERT_EQ(expectedChildren, bCollCQ->getPrimaryMatchExpression()->toString());

    auto* cCollCQ = joinGraph.accessPathAt((NodeId)2);
    ASSERT_EQ(cCollCQ->nss().coll(), "C");
    expectedChildren = "{ joinKey2: { $not: { $in: [ 400, 500, 600 ] } } }";
    ASSERT_EQ(expectedChildren, cCollCQ->getPrimaryMatchExpression()->toString());
}


TEST_F(PipelineAnalyzerTest, PropagateInSingleTablePredicate) {
    // At the moment the only disjunctive predicate we support propagating is $in.
    // Consider join A.a = B.b and STP {$or: [{B.b: 100}, {B.b: 200}]}. The STP
    // gets optimized/rewritten to { b: { $in: [ 100, 200 ] } }, making it therefore eligible
    // for propagation to A.
    const auto query = R"([
    {
        $lookup: {
        from: "B",
        localField: "a",
        foreignField: "b",
        as: "joinedB",
        pipeline: [
            {
            $match: {
                $and: [
                {
                    $or: [
                    { b: 100 },
                    { b: 200 }
                    ]
                }
                ]
            }
            }
        ]
        }
    },
    { $unwind: "$joinedB" }
    ])";
    auto pipeline = makePipeline(query, {"B"});
    markFieldsAsScalar(*pipeline, {"a"sv, "c"sv}, {{"B", {"b"sv}}});
    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));
    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);
    auto& joinModel = swJoinModel.getValue();
    auto& joinGraph = joinModel.getGraph();

    const auto* baseCollCQ = joinGraph.accessPathAt((NodeId)0);
    ASSERT_EQ(baseCollCQ->nss().coll(), "pipeline_test");
    auto expectedChildren = "{ a: { $in: [ 100, 200 ] } }";
    ASSERT_EQ(expectedChildren, baseCollCQ->getPrimaryMatchExpression()->toString());

    const auto* bCollCQ = joinGraph.accessPathAt((NodeId)1);
    ASSERT_EQ(bCollCQ->nss().coll(), "B");
    expectedChildren = "{ b: { $in: [ 100, 200 ] } }";
    ASSERT_EQ(expectedChildren, bCollCQ->getPrimaryMatchExpression()->toString());
}

TEST_F(PipelineAnalyzerTest, PreserveEqExprSemantics) {

    // This query has STPs with a mix of $eq and $expr semantics.
    // For join A.a = B.b and B.b = 5 ($eq semantics) and A.c = C.c and C.c = 10 ($expr semantics),
    // we need to propagate A.a = 5 with $eq semantics and A.c = 10 with $expr semantics
    const auto query = R"([
    {
        $lookup: {
        from: "B",
        localField: "a",
        foreignField: "b",   
        pipeline: [
            { $match: { b: 5 } }
        ],
        as: "Bdocs"
        }
    },
    { $unwind: "$Bdocs" },
    {
        $lookup: {
        from: "C",
        let: { c_val: "$c" },
        pipeline: [
            {
            $match: {
                $expr: {
                $and: [
                    { $eq: ["$c", "$$c_val"] },
                    { $eq: ["$c", 10] }
                ]
                }
            }
            }
        ],
        as: "Cdocs"
        }
    },
    { $unwind: "$Cdocs" }
    ])";
    auto pipeline = makePipeline(query, {"B", "C"});
    markFieldsAsScalar(*pipeline, {"a"sv, "c"sv}, {{"B", {"b"sv}}, {"C", {"c"sv}}});
    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));
    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);
    auto& joinModel = swJoinModel.getValue();
    auto& joinGraph = joinModel.getGraph();

    const auto* baseCollCQ = joinGraph.accessPathAt((NodeId)0);
    ASSERT_EQ(baseCollCQ->nss().coll(), "pipeline_test");
    auto expectedChildren =
        "{ $and: [ { a: { $eq: 5 } }, { $expr: { $eq: [ \"$c\", { $const: 10 } ] } }, { c: { "
        "$_internalExprEq: 10 } } ] }";
    ASSERT_EQ(expectedChildren, baseCollCQ->getPrimaryMatchExpression()->toString());

    const auto* bCollCQ = joinGraph.accessPathAt((NodeId)1);
    ASSERT_EQ(bCollCQ->nss().coll(), "B");
    expectedChildren = "{ b: { $eq: 5 } }";
    ASSERT_EQ(expectedChildren, bCollCQ->getPrimaryMatchExpression()->toString());

    const auto* cCollCQ = joinGraph.accessPathAt((NodeId)2);
    ASSERT_EQ(cCollCQ->nss().coll(), "C");
    expectedChildren =
        "{ $and: [ { $expr: { $eq: [ \"$c\", { $const: 10 } ] } }, { c: { $_internalExprEq: 10 } } "
        "] }";
    ASSERT_EQ(expectedChildren, cCollCQ->getPrimaryMatchExpression()->toString());
}

TEST_F(PipelineAnalyzerTest, PreserveEqExprSemanticsInAJoinCycle) {

    // This query is a four-node cyclic join graph with five join predicates that contain a mix of
    // $eq and $expr semantics.

    // 1. A.x = B.x where A.x = 1  (propagate B.x = 1 to B, $eq semantics)
    // 2. A.z = C.z, A.x = C.x (propagate A.z = 5, $expr semantics and propagate C.x = 1, $eq
    // semantics)
    // 3. B.y = C.y where C.y = 10 (propagate B.y = 10 $expr semantics).
    // 4. D.w = C.w where D.w = 2 (propagate C.w = 2, $expr semantics) and D.p = A.x (propagate D.p
    // = 1, $eq semantics)
    const auto query = R"([
        { $match: { x: 1 } },
        { $lookup: { from: "B", localField: "x", foreignField: "x", as: "bDocs" } },
        { $unwind: "$bDocs" },
        {
        $lookup: {
            from: "C",
            let: { b_y: "$bDocs.y", a_z: "$z", a_x: "$x" },
            pipeline: [
            { $match: { $expr: {
                $and: [
                    { $eq: ["$y", "$$b_y"] },
                    { $eq: ["$z", "$$a_z"] },
                    { $eq: ["$x", "$$a_x"] }, 
                    { $eq: ["$y", 10] },
                    { $eq: ["$z", 5] }
                ]
            } } }
            ],
            as: "cDocs"
        }
        },
        { $unwind: "$cDocs" },
        {
        $lookup: {
            from: "D",
            let: { c_w: "$cDocs.w", a_x: "$x" },
            pipeline: [
            { $match: { $expr: {
                $and: [
                    { $eq: ["$w", "$$c_w"] },
                    { $eq: ["$p", "$$a_x"] },
                    { $eq: ["$w", 2] }
                ]
            } } }
            ],
            as: "dDocs"
        }
        },
        { $unwind: "$dDocs" }
        ])";
    auto pipeline = makePipeline(query, {"B", "C", "D"});
    markFieldsAsScalar(
        *pipeline,
        {"x"sv, "z"sv},
        {{"B", {"x"sv, "y"sv}}, {"C", {"y"sv, "z"sv, "x"sv, "w"sv}}, {"D", {"w"sv, "p"sv}}});
    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));
    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);
    auto& joinModel = swJoinModel.getValue();
    auto& joinGraph = joinModel.getGraph();

    const auto* baseCollCQ = joinGraph.accessPathAt((NodeId)0);
    ASSERT_EQ(baseCollCQ->nss().coll(), "pipeline_test");
    auto expectedChildren =
        "{ $and: [ { x: { $eq: 1 } }, { $expr: { $eq: [ \"$z\", { $const: 5 } ] } }, { z: { "
        "$_internalExprEq: 5 } } ] }";
    ASSERT_EQ(expectedChildren, baseCollCQ->getPrimaryMatchExpression()->toString());

    const auto* bCollCQ = joinGraph.accessPathAt((NodeId)1);
    ASSERT_EQ(bCollCQ->nss().coll(), "B");
    expectedChildren =
        "{ $and: [ { x: { $eq: 1 } }, { $expr: { $eq: [ \"$y\", { $const: 10 } ] } }, { y: { "
        "$_internalExprEq: 10 } } ] }";
    ASSERT_EQ(expectedChildren, bCollCQ->getPrimaryMatchExpression()->toString());

    const auto* cCollCQ = joinGraph.accessPathAt((NodeId)2);
    ASSERT_EQ(cCollCQ->nss().coll(), "C");
    expectedChildren =
        "{ $and: [ { x: { $eq: 1 } }, { $expr: { $and: [ { $eq: [ \"$y\", { $const: 10 } ] }, { "
        "$eq: [ \"$z\", { $const: 5 } ] } ] } }, { $expr: { $eq: [ \"$w\", { $const: 2 } ] } }, { "
        "w: { $_internalExprEq: 2 } }, { y: { $_internalExprEq: 10 } }, { "
        "z: { $_internalExprEq: 5 } } ] }";
    ASSERT_EQ(expectedChildren, cCollCQ->getPrimaryMatchExpression()->toString());

    const auto* dCollCQ = joinGraph.accessPathAt((NodeId)3);
    ASSERT_EQ(dCollCQ->nss().coll(), "D");
    expectedChildren =
        "{ $and: [ { p: { $eq: 1 } }, { $expr: { $eq: [ \"$w\", { $const: 2 } ] } }, { w: { "
        "$_internalExprEq: 2 } } ] }";
    ASSERT_EQ(expectedChildren, dCollCQ->getPrimaryMatchExpression()->toString());
}


TEST_F(PipelineAnalyzerTest,
       PipelineEligibleForJoinReorderingNoLocalForeignFieldsSimpleSingleTablePredicate) {
    const auto query = R"([
            {$lookup: {from: "B", as: "fromB", pipeline: [{$match: {a: 1}}]}},
            {$unwind: "$fromB"}
        ])";

    auto pipeline = makePipeline(query, {"A", "B"});

    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    // TODO SERVER-116034: Support cross-products.
    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_NOT_OK(swJoinModel);
}

TEST_F(PipelineAnalyzerTest, PipelinePrefixEligibleForJoinReorderingNoLocalForeignFields) {
    const auto query = R"([
            {$lookup: {from: "A", localField: "a", foreignField: "b", as: "fromA"}},
            {$unwind: "$fromA"},
            {$lookup: {from: "B", as: "fromB", pipeline: [{$match: {a: 1}}]}},
            {$unwind: "$fromB"}
        ])";

    auto pipeline = makePipeline(query, {"A", "B"});
    markFieldsAsScalar(*pipeline, {"a"sv}, {{"A", {"b"sv}}});

    // This pipeline's prefix is eligible for reordering.
    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    // TODO SERVER-116034: Support cross-products.
    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_NOT_OK(swJoinModel);
}

TEST_F(PipelineAnalyzerTest, PipelineEligibleForJoinReorderingSingleLookupUnwind) {
    unittest::GoldenTestContext goldenCtx(&goldenTestConfig);
    const auto query = R"([
            {$lookup: {from: "A", localField: "a", foreignField: "b", as: "fromA"}},
            {$unwind: "$fromA"}
        ])";

    auto pipeline = makePipeline(query, {"A"});
    markFieldsAsScalar(*pipeline, {"a"sv}, {{"A", {"b"sv}}});
    // This pipeline is eligible for reordering.
    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);

    auto& joinModel = swJoinModel.getValue();
    goldenCtx.outStream() << joinModel.toString(true) << std::endl;
}

TEST_F(PipelineAnalyzerTest, LetLocalFieldPrefixedByAsField) {
    // A $expr/let join predicate whose local field ('X.y') is prefixed by the $lookup "as" field
    // ('X'). The local reference is evaluated against the input document before the foreign result
    // overwrites 'X', so 'X.y' must resolve to the base collection -- not to the just-added foreign
    // node. Resolving it to the foreign node would attribute both sides of the predicate to the
    // same node and produce an illegal self-edge.
    const auto query = R"([
            {$lookup: {from: "A", as: "X", let: {l: "$X.y"},
                       pipeline: [{$match: {$expr: {$eq: ["$z", "$$l"]}}}] }
            },
            {$unwind: "$X"}
        ])";

    auto pipeline = makePipeline(query, {"A"});
    markFieldsAsScalar(*pipeline, {"X.y"sv}, {{"A", {"z"sv}}});

    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);

    const auto& joinModel = swJoinModel.getValue();
    const auto& joinGraph = joinModel.getGraph();
    ASSERT_EQ(joinGraph.numNodes(), 2);
    ASSERT_EQ(joinGraph.numEdges(), 1);

    // The single edge must connect two distinct nodes (the base and the foreign collection),
    // i.e. it must not be a self-edge.
    const auto& edge = joinGraph.getEdge((EdgeId)0);
    ASSERT_NE(edge.left, edge.right);
    ASSERT_EQ(1, edge.predicates.size());
    ASSERT_EQ(JoinPredicate::ExprEq, edge.predicates[0].op);

    // The local side 'X.y' must be attributed to the base collection (node 0), not the foreign
    // node created from the $lookup.
    const auto& resolvedPaths = joinModel.getResolvedPaths();
    bool foundLocalOnBase = false;
    for (const auto& rp : resolvedPaths) {
        if (rp.fieldName.fullPath() == "X.y") {
            ASSERT_EQ((NodeId)0, rp.nodeId);
            foundLocalOnBase = true;
        }
    }
    ASSERT_TRUE(foundLocalOnBase);
}

TEST_F(PipelineAnalyzerTest, PipelineIneligibleForJoinReordering) {
    const auto query = R"([
            {$lookup: {from: "A", localField: "a", foreignField: "b", as: "fromA"}}
        ])";

    auto pipeline = makePipeline(query, {"A"});

    ASSERT_FALSE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));
}

TEST_F(PipelineAnalyzerTest, PipelineIneligibleForJoinReorderingNonAbsorbableUnwind) {
    const auto query = R"([
            {$lookup: {from: "B", localField: "a", foreignField: "b", as: "fromB"}},
            {$unwind: "$hello"}
        ])";

    auto pipeline = makePipeline(query, {"B"});

    ASSERT_FALSE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));
}

TEST_F(PipelineAnalyzerTest, TwoLookupUnwinds) {
    unittest::GoldenTestContext goldenCtx(&goldenTestConfig);
    const auto query = R"([
            {$lookup: {from: "A", localField: "a", foreignField: "b", as: "fromA"}},
            {$unwind: "$fromA"},
            {$lookup: {from: "B", localField: "a", foreignField: "b", as: "fromB"}},
            {$unwind: "$fromB"}
        ])";

    auto pipeline = makePipeline(query, {"A", "B"});
    markFieldsAsScalar(*pipeline, {"a"sv}, {{"A", {"b"sv}}, {"B", {"b"sv}}});

    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    auto& joinModel = swJoinModel.getValue();
    goldenCtx.outStream() << joinModel.toString(true) << std::endl;
}

TEST_F(PipelineAnalyzerTest, MatchOnMainCollection) {
    unittest::GoldenTestContext goldenCtx(&goldenTestConfig);
    const auto query = R"([
            {$match: {c: 1}},
            {$lookup: {from: "A", localField: "a", foreignField: "b", as: "fromA"}},
            {$unwind: "$fromA"},
            {$lookup: {from: "B", localField: "a", foreignField: "b", as: "fromB"}},
            {$unwind: "$fromB"}
        ])";

    auto pipeline = makePipeline(query, {"A", "B"});
    markFieldsAsScalar(*pipeline, {"a"sv}, {{"A", {"b"sv}}, {"B", {"b"sv}}});

    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);
    auto& joinModel = swJoinModel.getValue();
    goldenCtx.outStream() << joinModel.toString(true) << std::endl;
}

TEST_F(PipelineAnalyzerTest, MatchInSubPipeline) {
    unittest::GoldenTestContext goldenCtx(&goldenTestConfig);
    const auto query = R"([
            {$lookup: {from: "A", localField: "a", foreignField: "b", as: "fromA",
                       pipeline: [{$match: {d: 11}}] }
            },
            {$unwind: "$fromA"},
            {$lookup: {from: "B", localField: "a", foreignField: "b", as: "fromB"}},
            {$unwind: "$fromB"}
        ])";

    auto pipeline = makePipeline(query, {"A", "B"});
    markFieldsAsScalar(*pipeline, {"a"sv}, {{"A", {"b"sv}}, {"B", {"b"sv}}});

    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);

    const auto& joinModel = swJoinModel.getValue();
    const auto& joinGraph = joinModel.getGraph();
    ASSERT_EQ(joinGraph.numNodes(), 3);
    const auto* cq = joinGraph.accessPathAt((NodeId)1);
    ASSERT_EQ(cq->nss().coll(), "A");
    ASSERT_EQ("{ d: { $eq: 11 } }", cq->getPrimaryMatchExpression()->toString());
    goldenCtx.outStream() << joinModel.toString(true) << std::endl;
}

TEST_F(PipelineAnalyzerTest, AbsorbedFilterNonPipelineLookup) {
    const auto query = R"([
            {$lookup: {from: "A", localField: "a", foreignField: "b", as: "fromA"}},
            {$unwind: "$fromA"},
            {$match: {"fromA.d": 11}}
        ])";

    auto pipeline = makePipeline(query, {"A"});
    markFieldsAsScalar(*pipeline, {"a"sv}, {{"A", {"b"sv}}});

    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);

    const auto& joinModel = swJoinModel.getValue();
    ASSERT_EQ(joinModel.getGraph().numNodes(), 2);
    const auto* baseCq = joinModel.getGraph().accessPathAt((NodeId)0);
    ASSERT_EQ("{}", baseCq->getPrimaryMatchExpression()->toString());
    const auto* cq = joinModel.getGraph().accessPathAt((NodeId)1);
    ASSERT_EQ(cq->nss().coll(), "A");
    ASSERT_EQ("{ d: { $eq: 11 } }", cq->getPrimaryMatchExpression()->toString());
}

TEST_F(PipelineAnalyzerTest, AbsorbedFilterEmptyPipeline) {
    // pipeline:[] with a trailing $match that gets absorbed. The introspection pipeline is empty,
    // so the foreign CQ filter comes entirely from the absorbed match.
    const auto query = R"([
            {$lookup: {from: "A", localField: "a", foreignField: "b", as: "fromA",
                       pipeline: [] }
            },
            {$unwind: "$fromA"},
            {$match: {"fromA.d": 11}}
        ])";

    auto pipeline = makePipeline(query, {"A"});
    markFieldsAsScalar(*pipeline, {"a"sv}, {{"A", {"b"sv}}});

    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);

    const auto& joinModel = swJoinModel.getValue();
    ASSERT_EQ(joinModel.getGraph().numNodes(), 2);
    const auto* baseCq = joinModel.getGraph().accessPathAt((NodeId)0);
    ASSERT_EQ("{}", baseCq->getPrimaryMatchExpression()->toString());
    const auto* cq = joinModel.getGraph().accessPathAt((NodeId)1);
    ASSERT_EQ(cq->nss().coll(), "A");
    ASSERT_EQ("{ d: { $eq: 11 } }", cq->getPrimaryMatchExpression()->toString());
}

TEST_F(PipelineAnalyzerTest, EmptyPipelineNoFilterEligible) {
    // pipeline: [] without a trailing $match remains eligible — guards against the ineligibility
    // check on absorbed filter over-firing for the no-filter case.
    const auto query = R"([
            {$lookup: {from: "A", localField: "a", foreignField: "b", as: "fromA",
                       pipeline: [] }
            },
            {$unwind: "$fromA"}
        ])";

    auto pipeline = makePipeline(query, {"A"});
    markFieldsAsScalar(*pipeline, {"a"sv}, {{"A", {"b"sv}}});

    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));
}

TEST_F(PipelineAnalyzerTest, EmptyPipelineNoJoinPredicateRejected) {
    // $lookup with pipeline:[] and NO localField/foreignField has no source of a join
    // predicate. isLookupEligible() lets this shape through since it is dealing with the general
    // shape. But constructJoinModel would add the foreign node WITHOUT any connecting edge: no
    // localField/foreignField pair, no $expr in the empty sub-pipeline, no downstream
    // cross-collection $match. The resulting graph would be disconnected, which constructJoinModel
    // rejects with InternalErrorNotSupported because cross-products are not currently supported by
    // the join optimizer.
    const auto query = R"([
            {$lookup: {from: "A", as: "fromA", pipeline: [] } },
            {$unwind: "$fromA"}
        ])";

    auto pipeline = makePipeline(query, {"A"});

    // The pipeline passes the surface-level eligibility check (some $lookup is eligible).
    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    // But constructJoinModel rejects the would-be-disconnected graph.
    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_EQ(swJoinModel.getStatus(), ErrorCodes::InternalErrorNotSupported);
}

TEST_F(PipelineAnalyzerTest, NumericLocalFieldIneligibleJoinPredicate) {
    const auto query = R"([
            {$lookup: {from: "A", localField: "a.0", foreignField: "b", as: "fromA"}},
            {$unwind: "$fromA"}
        ])";

    auto pipeline = makePipeline(query, {"A"});
    markFieldsAsScalar(*pipeline, {"a.0"sv}, {{"A", {"b"sv}}});
    // Structurally eligible ($lookup + $unwind pair exists) ...
    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));
    // ... but the numeric path component in localField makes the join predicate ineligible.
    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_NOT_OK(swJoinModel);
}

TEST_F(PipelineAnalyzerTest, NumericForeignFieldIneligibleJoinPredicate) {
    const auto query = R"([
            {$lookup: {from: "A", localField: "a", foreignField: "b.0", as: "fromA"}},
            {$unwind: "$fromA"}
        ])";

    auto pipeline = makePipeline(query, {"A"});
    markFieldsAsScalar(*pipeline, {"a"sv}, {{"A", {"b.0"sv}}});
    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));
    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_NOT_OK(swJoinModel);
}

TEST_F(PipelineAnalyzerTest, NumericMidPathComponentIneligibleJoinPredicate) {
    const auto query = R"([
            {$lookup: {from: "A", localField: "a.0.b", foreignField: "c", as: "fromA"}},
            {$unwind: "$fromA"}
        ])";

    auto pipeline = makePipeline(query, {"A"});
    markFieldsAsScalar(*pipeline, {"a.0.b"sv}, {{"A", {"c"sv}}});
    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));
    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_NOT_OK(swJoinModel);
}

TEST_F(PipelineAnalyzerTest, SubPipelineMatchPlusAbsorbedFilter) {
    // pipeline:[$match] with a trailing $match that gets absorbed. The foreign CQ's filter is the
    // conjunction of the sub-pipeline match and the absorbed match.
    const auto query = R"([
            {$lookup: {from: "A", localField: "a", foreignField: "b", as: "fromA",
                       pipeline: [{$match: {d: 11}}] }
            },
            {$unwind: "$fromA"},
            {$match: {"fromA.e": 5}}
        ])";

    auto pipeline = makePipeline(query, {"A"});
    markFieldsAsScalar(*pipeline, {"a"sv}, {{"A", {"b"sv}}});

    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);

    const auto& joinModel = swJoinModel.getValue();
    ASSERT_EQ(joinModel.getGraph().numNodes(), 2);
    const auto* baseCq = joinModel.getGraph().accessPathAt((NodeId)0);
    ASSERT_EQ("{}", baseCq->getPrimaryMatchExpression()->toString());
    const auto* cq = joinModel.getGraph().accessPathAt((NodeId)1);
    ASSERT_EQ(cq->nss().coll(), "A");
    ASSERT_EQ("{ $and: [ { d: { $eq: 11 } }, { e: { $eq: 5 } } ] }",
              cq->getPrimaryMatchExpression()->toString());
}

TEST_F(PipelineAnalyzerTest, SubPipelineMultiPredicateMatchOrderingNoAbsorbedFilter) {
    // Pipeline-form $lookup with multiple STPs inside the sub-pipeline $match and NO trailing
    // absorbed $match. The STPs are authored in the order "z" then "a"; the final foreign CQ
    // must contain both predicates correctly in the normalized CQ order, i.e. lexicographically
    // ordered field names.
    const auto query = R"([
            {$lookup: {from: "A", localField: "a", foreignField: "b", as: "fromA",
                       pipeline: [{$match: {z: 99, a: 1}}] }
            },
            {$unwind: "$fromA"}
        ])";

    auto pipeline = makePipeline(query, {"A"});
    markFieldsAsScalar(*pipeline, {"a"sv}, {{"A", {"b"sv}}});

    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);

    const auto& joinModel = swJoinModel.getValue();
    ASSERT_EQ(joinModel.getGraph().numNodes(), 2);
    const auto* baseCq = joinModel.getGraph().accessPathAt((NodeId)0);
    ASSERT_EQ("{}", baseCq->getPrimaryMatchExpression()->toString());
    const auto* cq = joinModel.getGraph().accessPathAt((NodeId)1);
    ASSERT_EQ(cq->nss().coll(), "A");
    // Final CQ children are sorted by MatchExpression type first; for same-type predicates
    // (here both $eq) the field path is the secondary tiebreak, so "a" comes before "z"
    // regardless of the BSON authoring order ("z" first, "a" second) in the sub-pipeline.
    ASSERT_EQ("{ $and: [ { a: { $eq: 1 } }, { z: { $eq: 99 } } ] }",
              cq->getPrimaryMatchExpression()->toString());
}

TEST_F(PipelineAnalyzerTest, SubPipelineMatchPlusAbsorbedFilterPreservesPipelineOrder) {
    // Verifies the order of children in the conjunction STP from combining the sub-pipeline
    // $match and the absorbed filter $match. Children are sorted by MatchExpression type,
    // with field path as a secondary tiebreak within the same type — both predicates here
    // are $eq, so the path-based secondary sort puts "a" before "z".
    const auto query = R"([
            {$lookup: {from: "A", localField: "a", foreignField: "b", as: "fromA",
                       pipeline: [{$match: {z: 99}}] }
            },
            {$unwind: "$fromA"},
            {$match: {"fromA.a": 1}}
        ])";

    auto pipeline = makePipeline(query, {"A"});
    markFieldsAsScalar(*pipeline, {"a"sv}, {{"A", {"b"sv}}});

    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);

    const auto& joinModel = swJoinModel.getValue();
    ASSERT_EQ(joinModel.getGraph().numNodes(), 2);
    const auto* baseCq = joinModel.getGraph().accessPathAt((NodeId)0);
    ASSERT_EQ("{}", baseCq->getPrimaryMatchExpression()->toString());
    const auto* cq = joinModel.getGraph().accessPathAt((NodeId)1);
    ASSERT_EQ(cq->nss().coll(), "A");
    // The CQ-construction sort places "a" (absorbed, later in pipeline) before "z"
    // (sub-pipeline, earlier in pipeline). The AndMatchExpression handed to CQ construction
    // was built in pipeline order (z then a); the type-then-path sort applied during CQ
    // normalization rearranges them, so the observed ordering is a property of the CQ layer,
    // not the AND-assembly in extractPredicatesFromLookup.
    ASSERT_EQ("{ $and: [ { a: { $eq: 1 } }, { z: { $eq: 99 } } ] }",
              cq->getPrimaryMatchExpression()->toString());
}

TEST_F(PipelineAnalyzerTest, SubPipelineCorrelatedMatchPlusAbsorbedFilter) {
    // pipeline:[$expr-correlated $match] with a trailing $match that gets absorbed. The
    // correlated predicate becomes a join edge; the absorbed match becomes a single-table filter
    // on the foreign CQ.
    const auto query = R"([
            {$lookup: {from: "A",
                       let: {a: "$a"},
                       pipeline: [{$match: {$expr: {$eq: ["$b", "$$a"]}}}],
                       as: "fromA" }
            },
            {$unwind: "$fromA"},
            {$match: {"fromA.e": 5}}
        ])";

    auto pipeline = makePipeline(query, {"A"});
    markFieldsAsScalar(*pipeline, {"a"sv}, {{"A", {"b"sv}}});

    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);

    const auto& joinModel = swJoinModel.getValue();
    ASSERT_EQ(joinModel.getGraph().numNodes(), 2);
    const auto* baseCq = joinModel.getGraph().accessPathAt((NodeId)0);
    ASSERT_EQ("{}", baseCq->getPrimaryMatchExpression()->toString());
    const auto* cq = joinModel.getGraph().accessPathAt((NodeId)1);
    ASSERT_EQ(cq->nss().coll(), "A");
    ASSERT_EQ("{ e: { $eq: 5 } }", cq->getPrimaryMatchExpression()->toString());
}

TEST_F(PipelineAnalyzerTest, SubPipelineMatchPlusAbsorbedFilterMixedBaseAndAsField) {
    // pipeline:[$match] with a trailing $match that has predicates on BOTH the as-field and a
    // base-collection field. The pipeline optimizer splits the trailing match: base part is
    // pushed before the lookup as a prefix $match (handled in the base CQ), as-field part is
    // absorbed into the lookup. The foreign CQ ends up with the conjunction of the sub-pipeline
    // match and the absorbed as-field part.
    const auto query = R"([
            {$lookup: {from: "A", localField: "a", foreignField: "b", as: "fromA",
                       pipeline: [{$match: {d: 11}}] }
            },
            {$unwind: "$fromA"},
            {$match: {"fromA.e": 5, "z": 7}}
        ])";

    auto pipeline = makePipeline(query, {"A"});
    markFieldsAsScalar(*pipeline, {"a"sv}, {{"A", {"b"sv}}});

    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);

    const auto& joinModel = swJoinModel.getValue();
    ASSERT_EQ(joinModel.getGraph().numNodes(), 2);
    const auto* baseCq = joinModel.getGraph().accessPathAt((NodeId)0);
    ASSERT_EQ("{ z: { $eq: 7 } }", baseCq->getPrimaryMatchExpression()->toString());
    const auto* cq = joinModel.getGraph().accessPathAt((NodeId)1);
    ASSERT_EQ(cq->nss().coll(), "A");
    ASSERT_EQ("{ $and: [ { d: { $eq: 11 } }, { e: { $eq: 5 } } ] }",
              cq->getPrimaryMatchExpression()->toString());
}

TEST_F(PipelineAnalyzerTest, SubPipelineMixedCorrelatedAndUncorrelatedPlusAbsorbedFilter) {
    // Sub-pipeline $match has both a correlated $expr (becomes the join edge) AND a non-correlated
    // single-table predicate. The trailing $match is absorbed and combined with the single-table
    // part of the sub-pipeline split.
    const auto query = R"([
            {$lookup: {from: "A",
                       let: {a: "$a"},
                       pipeline: [{$match: {$and: [{$expr: {$eq: ["$b", "$$a"]}}, {d: 11}]}}],
                       as: "fromA" }
            },
            {$unwind: "$fromA"},
            {$match: {"fromA.e": 5}}
        ])";

    auto pipeline = makePipeline(query, {"A"});
    markFieldsAsScalar(*pipeline, {"a"sv}, {{"A", {"b"sv}}});

    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);

    const auto& joinModel = swJoinModel.getValue();
    ASSERT_EQ(joinModel.getGraph().numNodes(), 2);
    const auto* baseCq = joinModel.getGraph().accessPathAt((NodeId)0);
    ASSERT_EQ("{}", baseCq->getPrimaryMatchExpression()->toString());
    const auto* cq = joinModel.getGraph().accessPathAt((NodeId)1);
    ASSERT_EQ(cq->nss().coll(), "A");
    ASSERT_EQ("{ $and: [ { d: { $eq: 11 } }, { e: { $eq: 5 } } ] }",
              cq->getPrimaryMatchExpression()->toString());
}

TEST_F(PipelineAnalyzerTest, SubPipelineMultiPredicateMatchPlusAbsorbedFilter) {
    // Sub-pipeline $match has multiple single-table predicates. The trailing $match is absorbed
    // and combined with the multi-predicate sub-pipeline result.
    const auto query = R"([
            {$lookup: {from: "A", localField: "a", foreignField: "b", as: "fromA",
                       pipeline: [{$match: {d: 11, x: 7}}] }
            },
            {$unwind: "$fromA"},
            {$match: {"fromA.e": 5}}
        ])";

    auto pipeline = makePipeline(query, {"A"});
    markFieldsAsScalar(*pipeline, {"a"sv}, {{"A", {"b"sv}}});

    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);

    const auto& joinModel = swJoinModel.getValue();
    ASSERT_EQ(joinModel.getGraph().numNodes(), 2);
    const auto* baseCq = joinModel.getGraph().accessPathAt((NodeId)0);
    ASSERT_EQ("{}", baseCq->getPrimaryMatchExpression()->toString());
    const auto* cq = joinModel.getGraph().accessPathAt((NodeId)1);
    ASSERT_EQ(cq->nss().coll(), "A");
    // The CanonicalQuery construction flattens nested $and and sorts children by
    // MatchExpression type, with field path as a secondary tiebreak within the same type.
    // All three children here are $eq, so the path-based secondary sort applies (d, e, x).
    ASSERT_EQ("{ $and: [ { d: { $eq: 11 } }, { e: { $eq: 5 } }, { x: { $eq: 7 } } ] }",
              cq->getPrimaryMatchExpression()->toString());
}

TEST_F(PipelineAnalyzerTest, SubPipelineNonEqStpPlusNonEqAbsorbedFilter) {
    // Sub-pipeline $match has a non-$eq predicate ($gt); trailing $match also has a non-$eq
    // predicate ($lt). Verifies that non-$eq comparison operators are preserved on both sides
    // when combined into the foreign CQ.
    const auto query = R"([
            {$lookup: {from: "A", localField: "a", foreignField: "b", as: "fromA",
                       pipeline: [{$match: {d: {$gt: 10}}}] }
            },
            {$unwind: "$fromA"},
            {$match: {"fromA.e": {$lt: 100}}}
        ])";

    auto pipeline = makePipeline(query, {"A"});
    markFieldsAsScalar(*pipeline, {"a"sv}, {{"A", {"b"sv}}});

    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);

    const auto& joinModel = swJoinModel.getValue();
    ASSERT_EQ(joinModel.getGraph().numNodes(), 2);
    const auto* baseCq = joinModel.getGraph().accessPathAt((NodeId)0);
    ASSERT_EQ("{}", baseCq->getPrimaryMatchExpression()->toString());
    const auto* cq = joinModel.getGraph().accessPathAt((NodeId)1);
    ASSERT_EQ(cq->nss().coll(), "A");
    // CQ-level sortMatchExpressionTree orders children by MatchExpression type first, not by
    // field name: $lt sorts before $gt regardless of path. So the trailing-absorbed-$lt comes
    // first, the sub-pipeline-$gt second.
    ASSERT_EQ("{ $and: [ { e: { $lt: 100 } }, { d: { $gt: 10 } } ] }",
              cq->getPrimaryMatchExpression()->toString());
}

TEST_F(PipelineAnalyzerTest, SubPipelineNestedOrPlusAbsorbedFilter) {
    // Sub-pipeline $match has a complex shape: a non-rooted $or nested under an implicit
    // top-level $and. (A rooted $or would trigger the SERVER-121502 fallback and disqualify
    // the lookup; nesting it inside an $and avoids that path.) Combined with a plain absorbed
    // $match, the foreign CQ should preserve the $or structure alongside the other predicates.
    const auto query = R"([
            {$lookup: {from: "A", localField: "a", foreignField: "b", as: "fromA",
                       pipeline: [{$match: {d: 5, $or: [{x: 1}, {x: 2}]}}] }
            },
            {$unwind: "$fromA"},
            {$match: {"fromA.e": 7}}
        ])";

    auto pipeline = makePipeline(query, {"A"});
    markFieldsAsScalar(*pipeline, {"a"sv}, {{"A", {"b"sv}}});

    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);

    const auto& joinModel = swJoinModel.getValue();
    ASSERT_EQ(joinModel.getGraph().numNodes(), 2);
    const auto* cq = joinModel.getGraph().accessPathAt((NodeId)1);
    ASSERT_EQ(cq->nss().coll(), "A");
    // The MatchExpression optimizer collapses $or of equality predicates on the same field
    // into a single $in.
    ASSERT_EQ("{ $and: [ { d: { $eq: 5 } }, { e: { $eq: 7 } }, { x: { $in: [ 1, 2 ] } } ] }",
              cq->getPrimaryMatchExpression()->toString());
}

TEST_F(PipelineAnalyzerTest, SubPipelineNonCorrelatedExprStpPlusAbsorbedFilter) {
    // Sub-pipeline $match contains a non-correlated $expr (references "$d" on the foreign
    // collection, not a $$letVar). This is a single-table predicate, not a join predicate,
    // because there's no correlation with the outer collection. The trailing plain $match is
    // absorbed and combined with the $expr STP in the foreign CQ.
    const auto query = R"([
            {$lookup: {from: "A", localField: "a", foreignField: "b", as: "fromA",
                       pipeline: [{$match: {$expr: {$gt: ["$d", 10]}}}] }
            },
            {$unwind: "$fromA"},
            {$match: {"fromA.e": 5}}
        ])";

    auto pipeline = makePipeline(query, {"A"});
    markFieldsAsScalar(*pipeline, {"a"sv}, {{"A", {"b"sv}}});

    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);

    const auto& joinModel = swJoinModel.getValue();
    ASSERT_EQ(joinModel.getGraph().numNodes(), 2);
    const auto* cq = joinModel.getGraph().accessPathAt((NodeId)1);
    ASSERT_EQ(cq->nss().coll(), "A");
    ASSERT_EQ(
        "{ $and: [ { e: { $eq: 5 } }, "
        "{ $expr: { $gt: [ \"$d\", { $const: 10 } ] } }, "
        "{ d: { $_internalExprGt: 10 } } ] }",
        cq->getPrimaryMatchExpression()->toString());
}

TEST_F(PipelineAnalyzerTest, TwoMatchesBothOnAsFieldPipelineForm) {
    // Two consecutive trailing $match stages on the as-field of a pipeline-form $lookup. The
    // pipeline optimizer merges the two trailing matches before absorption; the merged result is
    // combined with the sub-pipeline match in the foreign CQ.
    const auto query = R"([
            {$lookup: {from: "A", localField: "a", foreignField: "b", as: "fromA",
                       pipeline: [{$match: {d: 11}}] }
            },
            {$unwind: "$fromA"},
            {$match: {"fromA.e": 5}},
            {$match: {"fromA.f": 7}}
        ])";

    auto pipeline = makePipeline(query, {"A"});
    markFieldsAsScalar(*pipeline, {"a"sv}, {{"A", {"b"sv}}});

    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);

    const auto& joinModel = swJoinModel.getValue();
    ASSERT_EQ(joinModel.getGraph().numNodes(), 2);
    const auto* baseCq = joinModel.getGraph().accessPathAt((NodeId)0);
    ASSERT_EQ("{}", baseCq->getPrimaryMatchExpression()->toString());
    const auto* cq = joinModel.getGraph().accessPathAt((NodeId)1);
    ASSERT_EQ(cq->nss().coll(), "A");
    ASSERT_EQ("{ $and: [ { d: { $eq: 11 } }, { e: { $eq: 5 } }, { f: { $eq: 7 } } ] }",
              cq->getPrimaryMatchExpression()->toString());
}

TEST_F(PipelineAnalyzerTest, SubPipelineNonEquijoinExprPlusAbsorbedFilter) {
    // Sub-pipeline $match contains a non-equijoin correlated $expr predicate (e.g., $gt). This
    // is not supported by the splitter, which returns nullopt. extractPredicatesFromLookup
    // surfaces this as a QueryFeatureNotAllowed error. The classic engine handles such queries
    // via the non-optimized path.
    const auto query = R"([
            {$lookup: {from: "A",
                       let: {a: "$a"},
                       pipeline: [{$match: {$expr: {$gt: ["$b", "$$a"]}}}],
                       as: "fromA" }
            },
            {$unwind: "$fromA"},
            {$match: {"fromA.e": 5}}
        ])";

    auto pipeline = makePipeline(query, {"A"});
    markFieldsAsScalar(*pipeline, {"a"sv}, {{"A", {"b"sv}}});

    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_NOT_OK(swJoinModel);
    ASSERT_EQ(swJoinModel.getStatus().code(), ErrorCodes::QueryFeatureNotAllowed);
}

TEST_F(PipelineAnalyzerTest, TwoMatchesBothOnAsField) {
    const auto query = R"([
            {$lookup: {from: "A", localField: "a", foreignField: "b", as: "fromA"}},
            {$unwind: "$fromA"},
            {$match: {"fromA.c": 5}},
            {$match: {"fromA.d": 11}}
        ])";

    auto pipeline = makePipeline(query, {"A"});
    markFieldsAsScalar(*pipeline, {"a"sv}, {{"A", {"b"sv}}});

    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);

    const auto& joinModel = swJoinModel.getValue();
    ASSERT_EQ(joinModel.getGraph().numNodes(), 2);
    const auto* cq = joinModel.getGraph().accessPathAt((NodeId)1);
    ASSERT_EQ(cq->nss().coll(), "A");
    ASSERT_EQ("{ $and: [ { c: { $eq: 5 } }, { d: { $eq: 11 } } ] }",
              cq->getPrimaryMatchExpression()->toString());
}

TEST_F(PipelineAnalyzerTest, TwoMatchesFirstOnAsFieldSecondOnBaseField) {
    const auto query = R"([
            {$lookup: {from: "A", localField: "a", foreignField: "b", as: "fromA"}},
            {$unwind: "$fromA"},
            {$match: {"fromA.d": 11}},
            {$match: {"e": 5}}
        ])";

    auto pipeline = makePipeline(query, {"A"});
    markFieldsAsScalar(*pipeline, {"a"sv}, {{"A", {"b"sv}}});

    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);

    const auto& joinModel = swJoinModel.getValue();
    ASSERT_EQ(joinModel.getGraph().numNodes(), 2);
    const auto* baseCq = joinModel.getGraph().accessPathAt((NodeId)0);
    ASSERT_EQ("{ e: { $eq: 5 } }", baseCq->getPrimaryMatchExpression()->toString());
    const auto* cq = joinModel.getGraph().accessPathAt((NodeId)1);
    ASSERT_EQ(cq->nss().coll(), "A");
    ASSERT_EQ("{ d: { $eq: 11 } }", cq->getPrimaryMatchExpression()->toString());
}

TEST_F(PipelineAnalyzerTest, TwoMatchesFirstOnBaseFieldSecondOnAsField) {
    const auto query = R"([
            {$lookup: {from: "A", localField: "a", foreignField: "b", as: "fromA"}},
            {$unwind: "$fromA"},
            {$match: {"e": 5}},
            {$match: {"fromA.d": 11}}
        ])";

    auto pipeline = makePipeline(query, {"A"});
    markFieldsAsScalar(*pipeline, {"a"sv}, {{"A", {"b"sv}}});

    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);

    const auto& joinModel = swJoinModel.getValue();
    ASSERT_EQ(joinModel.getGraph().numNodes(), 2);
    const auto* baseCq = joinModel.getGraph().accessPathAt((NodeId)0);
    ASSERT_EQ("{ e: { $eq: 5 } }", baseCq->getPrimaryMatchExpression()->toString());
    const auto* cq = joinModel.getGraph().accessPathAt((NodeId)1);
    ASSERT_EQ(cq->nss().coll(), "A");
    ASSERT_EQ("{ d: { $eq: 11 } }", cq->getPrimaryMatchExpression()->toString());
}

TEST_F(PipelineAnalyzerTest, TwoMatchesSameFieldBothOnAsField) {
    const auto query = R"([
            {$lookup: {from: "A", localField: "a", foreignField: "b", as: "fromA"}},
            {$unwind: "$fromA"},
            {$match: {"fromA.d": {$gt: 5}}},
            {$match: {"fromA.d": {$lt: 10}}}
        ])";

    auto pipeline = makePipeline(query, {"A"});
    markFieldsAsScalar(*pipeline, {"a"sv}, {{"A", {"b"sv}}});

    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);

    const auto& joinModel = swJoinModel.getValue();
    ASSERT_EQ(joinModel.getGraph().numNodes(), 2);
    const auto* cq = joinModel.getGraph().accessPathAt((NodeId)1);
    ASSERT_EQ(cq->nss().coll(), "A");
    ASSERT_EQ("{ $and: [ { d: { $lt: 10 } }, { d: { $gt: 5 } } ] }",
              cq->getPrimaryMatchExpression()->toString());
}

TEST_F(PipelineAnalyzerTest, TwoMatchesEachOnDifferentCollection) {
    const auto query = R"([
            {$lookup: {from: "A", localField: "a", foreignField: "b", as: "fromA"}},
            {$unwind: "$fromA"},
            {$lookup: {from: "B", localField: "a", foreignField: "c", as: "fromB"}},
            {$unwind: "$fromB"},
            {$match: {"fromA.d": 11}},
            {$match: {"fromB.e": 7}}
        ])";

    auto pipeline = makePipeline(query, {"A", "B"});
    markFieldsAsScalar(*pipeline, {"a"sv}, {{"A", {"b"sv}}, {"B", {"c"sv}}});

    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);

    const auto& joinModel = swJoinModel.getValue();
    ASSERT_EQ(joinModel.getGraph().numNodes(), 3);
    const auto* baseCq = joinModel.getGraph().accessPathAt((NodeId)0);
    ASSERT_EQ("{}", baseCq->getPrimaryMatchExpression()->toString());
    const auto* cqA = joinModel.getGraph().accessPathAt((NodeId)1);
    ASSERT_EQ(cqA->nss().coll(), "A");
    ASSERT_EQ("{ d: { $eq: 11 } }", cqA->getPrimaryMatchExpression()->toString());
    const auto* cqB = joinModel.getGraph().accessPathAt((NodeId)2);
    ASSERT_EQ(cqB->nss().coll(), "B");
    ASSERT_EQ("{ e: { $eq: 7 } }", cqB->getPrimaryMatchExpression()->toString());
}

TEST_F(PipelineAnalyzerTest, MatchBetweenTwoLookupUnwinds) {
    const auto query = R"([
            {$lookup: {from: "A", localField: "a", foreignField: "b", as: "fromA"}},
            {$unwind: "$fromA"},
            {$match: {"fromA.d": 11}},
            {$lookup: {from: "B", localField: "a", foreignField: "c", as: "fromB"}},
            {$unwind: "$fromB"}
        ])";

    auto pipeline = makePipeline(query, {"A", "B"});
    markFieldsAsScalar(*pipeline, {"a"sv}, {{"A", {"b"sv}}, {"B", {"c"sv}}});

    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);

    const auto& joinModel = swJoinModel.getValue();
    ASSERT_EQ(joinModel.getGraph().numNodes(), 3);
    const auto* cqA = joinModel.getGraph().accessPathAt((NodeId)1);
    ASSERT_EQ(cqA->nss().coll(), "A");
    ASSERT_EQ("{ d: { $eq: 11 } }", cqA->getPrimaryMatchExpression()->toString());
    const auto* cqB = joinModel.getGraph().accessPathAt((NodeId)2);
    ASSERT_EQ(cqB->nss().coll(), "B");
    ASSERT_EQ("{}", cqB->getPrimaryMatchExpression()->toString());
}

TEST_F(PipelineAnalyzerTest, SingleMatchOnBothBaseAndAsField) {
    const auto query = R"([
            {$lookup: {from: "A", localField: "a", foreignField: "b", as: "fromA"}},
            {$unwind: "$fromA"},
            {$match: {"fromA.d": 11, "e": 5}}
        ])";

    auto pipeline = makePipeline(query, {"A"});
    markFieldsAsScalar(*pipeline, {"a"sv}, {{"A", {"b"sv}}});

    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);

    const auto& joinModel = swJoinModel.getValue();
    ASSERT_EQ(joinModel.getGraph().numNodes(), 2);
    const auto* baseCq = joinModel.getGraph().accessPathAt((NodeId)0);
    ASSERT_EQ("{ e: { $eq: 5 } }", baseCq->getPrimaryMatchExpression()->toString());
    const auto* cq = joinModel.getGraph().accessPathAt((NodeId)1);
    ASSERT_EQ(cq->nss().coll(), "A");
    ASSERT_EQ("{ d: { $eq: 11 } }", cq->getPrimaryMatchExpression()->toString());
}

TEST_F(PipelineAnalyzerTest, SingleMatchOnTwoDifferentAsFields) {
    const auto query = R"([
            {$lookup: {from: "A", localField: "a", foreignField: "b", as: "fromA"}},
            {$unwind: "$fromA"},
            {$lookup: {from: "B", localField: "a", foreignField: "c", as: "fromB"}},
            {$unwind: "$fromB"},
            {$match: {"fromA.d": 11, "fromB.e": 7}}
        ])";

    auto pipeline = makePipeline(query, {"A", "B"});
    markFieldsAsScalar(*pipeline, {"a"sv}, {{"A", {"b"sv}}, {"B", {"c"sv}}});

    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);

    const auto& joinModel = swJoinModel.getValue();
    ASSERT_EQ(joinModel.getGraph().numNodes(), 3);
    const auto* baseCq = joinModel.getGraph().accessPathAt((NodeId)0);
    ASSERT_EQ("{}", baseCq->getPrimaryMatchExpression()->toString());
    const auto* cqA = joinModel.getGraph().accessPathAt((NodeId)1);
    ASSERT_EQ(cqA->nss().coll(), "A");
    ASSERT_EQ("{ d: { $eq: 11 } }", cqA->getPrimaryMatchExpression()->toString());
    const auto* cqB = joinModel.getGraph().accessPathAt((NodeId)2);
    ASSERT_EQ(cqB->nss().coll(), "B");
    ASSERT_EQ("{ e: { $eq: 7 } }", cqB->getPrimaryMatchExpression()->toString());
}

TEST_F(PipelineAnalyzerTest, AbsorbedFilterOnChainedLookup) {
    const auto query = R"([
            {$lookup: {from: "A", localField: "a", foreignField: "b", as: "fromA"}},
            {$unwind: "$fromA"},
            {$lookup: {from: "B", localField: "fromA.c", foreignField: "d", as: "fromB"}},
            {$unwind: "$fromB"},
            {$match: {"fromB.e": 7}}
        ])";

    auto pipeline = makePipeline(query, {"A", "B"});
    markFieldsAsScalar(*pipeline, {"a"sv}, {{"A", {"b"sv, "c"sv}}, {"B", {"d"sv}}});

    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);

    const auto& joinModel = swJoinModel.getValue();
    ASSERT_EQ(joinModel.getGraph().numNodes(), 3);
    const auto* baseCq = joinModel.getGraph().accessPathAt((NodeId)0);
    ASSERT_EQ("{}", baseCq->getPrimaryMatchExpression()->toString());
    const auto* cqA = joinModel.getGraph().accessPathAt((NodeId)1);
    ASSERT_EQ(cqA->nss().coll(), "A");
    ASSERT_EQ("{}", cqA->getPrimaryMatchExpression()->toString());
    const auto* cqB = joinModel.getGraph().accessPathAt((NodeId)2);
    ASSERT_EQ(cqB->nss().coll(), "B");
    ASSERT_EQ("{ e: { $eq: 7 } }", cqB->getPrimaryMatchExpression()->toString());
}

TEST_F(PipelineAnalyzerTest, GroupOnMainCollection) {
    const auto query = R"([
            {$group: {_id: "$key", a: {$avg: "$c"}}},
            {$lookup: {from: "A", localField: "a", foreignField: "b", as: "fromA"}},
            {$unwind: "$fromA"},
            {$lookup: {from: "B", localField: "a", foreignField: "b", as: "fromB"}},
            {$unwind: "$fromB"}
        ])";

    auto pipeline = makePipeline(query, {"A", "B"});
    markFieldsAsScalar(*pipeline, {"a"sv}, {{"A", {"b"sv}}, {"B", {"b"sv}}});
    ASSERT_FALSE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));
}

TEST_F(PipelineAnalyzerTest, ConflictingLocalFields) {
    // Conflicting prefix in the local field

    auto query = R"([
        {$lookup: {from: "B", as: "a.b", localField: "x", foreignField: "y"}},
        {$unwind: "$a.b"},
        {$lookup: {from: "C", as: "c", localField: "a", foreignField: "z"}},
        {$unwind: "$c"}
    ])";

    auto pipeline = makePipeline(query, {"B", "C"});
    markFieldsAsScalar(*pipeline, {"x"sv, "a"sv}, {{"B", {"y"sv}}, {"C", {"z"sv}}});
    // We don't detect ineligibility of local path fields here.
    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));
    // But we do here, and shorten the prefix.
    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);
    ASSERT_EQ(swJoinModel.getValue().getGraph().numNodes(), 2);
    ASSERT_EQ(swJoinModel.getValue().getGraph().numEdges(), 1);
}

TEST_F(PipelineAnalyzerTest, LocalFieldExactlyMatchesPriorAsField) {
    // The second $lookup's localField "a" is *exactly* the first $lookup's "as" field (as opposed
    // to an ancestor of it, as in ConflictingLocalFields). Resolving it reaches a different branch
    // in PathResolver::resolve(): the embed path is a prefix of the field path (equal paths count
    // as a prefix), so the early conflict bail is skipped and the path is stripped. This must not
    // crash - the path refers to the bare embedded field, which is not traceable to a base
    // collection, so the second $lookup can't be incorporated. We bail gracefully by ending the
    // prefix there, leaving a valid 2-node join graph (base + B).
    auto query = R"([
        {$lookup: {from: "B", as: "a", localField: "x", foreignField: "y"}},
        {$unwind: "$a"},
        {$lookup: {from: "C", as: "c", localField: "a", foreignField: "z"}},
        {$unwind: "$c"}
    ])";

    auto pipeline = makePipeline(query, {"B", "C"});
    markFieldsAsScalar(*pipeline, {"x"sv, "a"sv}, {{"B", {"y"sv}}, {"C", {"z"sv}}});
    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));
    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);
    ASSERT_EQ(swJoinModel.getValue().getGraph().numNodes(), 2);
    ASSERT_EQ(swJoinModel.getValue().getGraph().numEdges(), 1);
}

TEST_F(PipelineAnalyzerTest, ConflictingLocalFieldExprSyntax) {
    const auto query = R"([
    {$lookup: {from: "B", as: "foo.b", localField: "x", foreignField: "y"}},
    {$unwind: "$foo.b"},
    {
        $lookup: {
            from: "A",
            let: {foo: "$foo", bar: "$bar"},
            pipeline: [
                {
                    $match: {
                        $expr: {
                            $and: [
                                {$eq: ["$foo", "$$foo"]},
                                {$eq: ["$bar", "$$bar"]}
                            ]
                        }
                    }
                }
            ],
            as: "a"
        }
    },
    {$unwind: "$a"}
    ])";

    auto pipeline = makePipeline(query, {"B", "A"});
    markFieldsAsScalar(
        *pipeline, {"x"sv, "foo"sv, "bar"sv}, {{"B", {"y"sv}}, {"A", {"foo"sv, "bar"sv}}});
    // We don't detect ineligibility of local path fields here.
    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));
    // But we do here, and shorten the prefix.
    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);
    ASSERT_EQ(swJoinModel.getValue().getGraph().numNodes(), 2);
    ASSERT_EQ(swJoinModel.getValue().getGraph().numEdges(), 1);
}

TEST_F(PipelineAnalyzerTest, CompatibleAsFields) {
    auto query = R"([
            {$lookup: {from: "B", as: "x.y", localField: "x.c", foreignField: "c"}},
            {$unwind: "$x.y"},
            {$lookup: {from: "C", as: "x.z", localField: "x.y.d", foreignField: "d"}},
            {$unwind: "$x.z"}
        ])";
    auto pipeline = makePipeline(query, {"B", "C"});
    markFieldsAsScalar(*pipeline, {"x.c"sv}, {{"B", {"c"sv, "d"sv}}, {"C", {"d"sv}}});

    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));
    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);
}

TEST_F(PipelineAnalyzerTest, GroupInMiddleIneligible) {
    unittest::GoldenTestContext goldenCtx(&goldenTestConfig);
    const auto query = R"([
            {$lookup: {from: "A", localField: "a", foreignField: "b", as: "fromA"}},
            {$unwind: "$fromA"},
            {$group: {_id: "$key", a: {$avg: "$c"}}},
            {$lookup: {from: "B", localField: "a", foreignField: "b", as: "fromB"}},
            {$unwind: "$fromB"}
        ])";

    auto pipeline = makePipeline(query, {"A", "B"});
    markFieldsAsScalar(*pipeline, {"a"sv}, {{"A", {"b"sv}}});

    // We don't detect ineligibility here.
    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    // This should show that our suffix starts at the $group.
    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);
    auto& joinModel = swJoinModel.getValue();
    goldenCtx.outStream() << joinModel.toString(true) << std::endl;
}

TEST_F(PipelineAnalyzerTest, GroupInSubPipeline) {
    const auto query = R"([
            {$lookup: {from: "A", localField: "a", foreignField: "b", as: "fromA",
                       pipeline: [{$group: {_id: "$key", a: {$avg: "$c"}}}] }
            },
            {$unwind: "$fromA"},
            {$lookup: {from: "B", localField: "a", foreignField: "b", as: "fromB"}},
            {$unwind: "$fromB"}
        ])";

    auto pipeline = makePipeline(query, {"A", "B"});
    markFieldsAsScalar(*pipeline, {"a"}, {{"A", {"b"}}, {"B", {"b"}}});
    ASSERT_FALSE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));
}

TEST_F(PipelineAnalyzerTest, IneligibleSubPipelineStage) {
    const auto query = R"([
            {$lookup: {from: "A", localField: "a", foreignField: "b", as: "fromA",
                       pipeline: [{$lookup: {from: "B", localField: "b", foreignField: "b", as:
                       "innerB"}}]}
            },
            {$unwind: "$fromA"},
            {$lookup: {from: "B", localField: "a", foreignField: "b", as: "fromB"}},
            {$unwind: "$fromB"}
        ])";

    auto pipeline = makePipeline(query, {"A", "B"});
    markFieldsAsScalar(*pipeline, {"a"sv}, {{"A", {"b"sv}}, {"B", {"b"sv}}});

    ASSERT_FALSE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));
}

TEST_F(PipelineAnalyzerTest, LongPrefix) {
    unittest::GoldenTestContext goldenCtx(&goldenTestConfig);
    const auto query = R"([
            {$match: {c: 1}},
            {$project: {k: 0}},
            {$lookup: {from: "A", localField: "a", foreignField: "b", as: "fromA"}},
            {$unwind: "$fromA"},
            {$lookup: {from: "B", localField: "a", foreignField: "b", as: "fromB"}},
            {$unwind: "$fromB"}
        ])";

    auto pipeline = makePipeline(query, {"A", "B"});
    markFieldsAsScalar(*pipeline, {"a"sv}, {{"A", {"b"sv}}, {"B", {"b"sv}}});

    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);
    auto& joinModel = swJoinModel.getValue();
    goldenCtx.outStream() << joinModel.toString(true) << std::endl;
}

TEST_F(PipelineAnalyzerTest, PipelineInEligibleForSortStage) {
    const auto sortPrefixQuery = R"([
            {$match: {c: 1}},
            {$sort: {e: 1}},
            {$project: {k: 0}},
            {$lookup: {from: "A", localField: "a", foreignField: "b", as: "fromA"}},
            {$unwind: "$fromA"},
            {$lookup: {from: "B", localField: "a", foreignField: "b", as: "fromB"}},
            {$unwind: "$fromB"}
        ])";

    auto pipeline = makePipeline(sortPrefixQuery, {"A", "B"});
    markFieldsAsScalar(*pipeline, {"a"}, {{"A", {"b"}}, {"B", {"b"}}});
    ASSERT_FALSE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));
}

TEST_F(PipelineAnalyzerTest, LocalFieldOverride) {
    unittest::GoldenTestContext goldenCtx(&goldenTestConfig);
    const auto query = R"([
            {$lookup: {from: "A", localField: "a", foreignField: "b", as: "a"}},
            {$unwind: "$a"},
            {$lookup: {from: "B", localField: "b", foreignField: "b", as: "b"}},
            {$unwind: "$b"}
        ])";

    auto pipeline = makePipeline(query, {"A", "B"});
    markFieldsAsScalar(*pipeline, {"a"sv, "b"sv}, {{"A", {"b"sv}}, {"B", {"b"sv}}});

    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);
    auto& joinModel = swJoinModel.getValue();
    goldenCtx.outStream() << joinModel.toString(true) << std::endl;
}

TEST_F(PipelineAnalyzerTest, PipelineWithProjectsJoinPredicatesUnmodifiedOk) {
    unittest::GoldenTestContext goldenCtx(&goldenTestConfig);
    const auto query = R"([
            {$project: {_id: 0}},
            {$lookup: {from: "A", localField: "a", foreignField: "b", as: "x", pipeline: [
                {$project: {_id: 0}}
            ]}},
            {$unwind: "$x"}
        ])";

    auto pipeline = makePipeline(query, {"A"});
    markFieldsAsScalar(*pipeline, {"a", "b"}, {{"A", {"b"}}});

    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);
    auto& joinModel = swJoinModel.getValue();
    goldenCtx.outStream() << joinModel.toString(true) << std::endl;
}

TEST_F(PipelineAnalyzerTest, PipelineWithRenamedBaseCollectionJoinPredFieldBails) {
    const auto query = R"([
            {$project: {a: "$foo"}},
            {$lookup: {from: "A", localField: "a", foreignField: "b", as: "x"}},
            {$unwind: "$x"}
        ])";

    auto pipeline = makePipeline(query, {"A"});
    markFieldsAsScalar(*pipeline, {"a", "foo"}, {{"A", {"b"}}});

    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    // We bail because we detect that field "a" was last modified by a non-$lookup stage.
    // TODO SERVER-128365: Support renames within CQ.
    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_NOT_OK(swJoinModel);
}

TEST_F(PipelineAnalyzerTest, PipelineWithRenamedBaseCollectionExprJoinPredFieldBails) {
    const auto query = R"([
            {$project: {a: "$foo"}},
            {$lookup: {from: "A", as: "x", let: {aa: "$a"}, pipeline: [
                {$match: {$expr: {$eq: ["$$aa", "$b"]}}}
            ]}},
            {$unwind: "$x"}
        ])";

    auto pipeline = makePipeline(query, {"A"});
    markFieldsAsScalar(*pipeline, {"a", "foo"}, {{"A", {"b"}}});

    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    // We bail because we detect that field "a" was last modified by a non-$lookup stage.
    // TODO SERVER-128365: Support renames within CQ.
    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_NOT_OK(swJoinModel);
}

TEST_F(PipelineAnalyzerTest, PipelineWithRenamedBaseCollectionTrailingExprJoinPredFieldBails) {
    const auto query = R"([
            {$project: {a: "$foo"}},
            {$lookup: {from: "A", as: "x", pipeline: []}},
            {$unwind: "$x"},
            {$match: {$expr: {$eq: ["$a", "$x.b"]}}}
        ])";

    auto pipeline = makePipeline(query, {"A"});
    markFieldsAsScalar(*pipeline, {"a", "foo"}, {{"A", {"b"}}});

    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    // We bail because we detect that field "a" was last modified by a non-$lookup stage.
    // TODO SERVER-128365: Support renames within CQ.
    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_NOT_OK(swJoinModel);
}

TEST_F(PipelineAnalyzerTest, PipelineWithProjectsJoinPredicateModifiedForJoinBails) {
    const auto query = R"([
            {$lookup: {from: "A", localField: "a", foreignField: "b", as: "x", pipeline: [
                {$project: {b: "$c.d.e.f"}}
            ]}},
            {$unwind: "$x"}
        ])";

    auto pipeline = makePipeline(query, {"A"});
    markFieldsAsScalar(*pipeline, {"a", "b"}, {{"A", {"b"}}});

    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    // TODO SERVER-128365: Support renames within CQ.
    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_NOT_OK(swJoinModel);
}

TEST_F(PipelineAnalyzerTest, PipelineWithProjectsExprJoinPredicateModifiedForJoinBails) {
    const auto query = R"([
            {$lookup: {from: "A", as: "a", let: {aa: "$a"}, pipeline: [
                {$match: {$expr: {$eq: ["$$aa", "$b"]}}},
                {$project: {b: "$c.d.e.f"}}
            ]}},
            {$unwind: "$a"}
        ])";

    auto pipeline = makePipeline(query, {"A"});
    markFieldsAsScalar(*pipeline, {"a", "b"}, {{"A", {"b"}}});

    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    // TODO SERVER-128365: Support renames within CQ.
    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_NOT_OK(swJoinModel);
}

TEST_F(PipelineAnalyzerTest, PrefixTooComplexForCQPushdownBails) {
    const auto query = R"([
            {$project: {_id: 0}},
            {$addFields: {bar: "$a"}},
            {$match: {bar: {$gt: 0}}},
            {$lookup: {from: "A", localField: "a", foreignField: "b", as: "a"}},
            {$unwind: "$a"}
        ])";

    auto pipeline = makePipeline(query, {"A"});
    markFieldsAsScalar(*pipeline, {"a", "b"}, {{"A", {"b"}}});

    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_NOT_OK(swJoinModel);
}

TEST_F(PipelineAnalyzerTest, InferredPredicateDoesntDiscardProjections) {
    unittest::GoldenTestContext goldenCtx(&goldenTestConfig);
    const auto query = R"([
            {$project: {_id: 0}},
            {$lookup: {from: "A", localField: "a", foreignField: "b", as: "x", pipeline: [
                {$match: {b: {$eq: 3}}},
                {$project: {_id: 0}}
            ]}},
            {$unwind: "$x"}
        ])";

    auto pipeline = makePipeline(query, {"A"});
    markFieldsAsScalar(*pipeline, {"a", "b"}, {{"A", {"b"}}});

    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);
    auto& joinModel = swJoinModel.getValue();
    goldenCtx.outStream() << joinModel.toString(true) << std::endl;
}

TEST_F(PipelineAnalyzerTest, SubPipelineTooComplexForCQPushdownBails) {
    const auto query = R"([
            {$lookup: {from: "A", localField: "a", foreignField: "b", as: "a", pipeline: [
                {$project: {_id: 0}},
                {$addFields: {bar: "$a"}},
                {$match: {bar: {$gt: 0}}}
            ]}},
            {$unwind: "$a"}
        ])";

    auto pipeline = makePipeline(query, {"A"});
    markFieldsAsScalar(*pipeline, {"a", "b"}, {{"A", {"b"}}});

    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_NOT_OK(swJoinModel);
}

// Tests that a $project stage as the only stage in a subpipeline (no $match) that excludes
// non-join-predicate fields is correctly handled: the model is built successfully and the
// projection is preserved in the foreign node's access path.
TEST_F(PipelineAnalyzerTest, SubpipelineProjectOnlyExcludesNonJoinFieldOk) {
    unittest::GoldenTestContext goldenCtx(&goldenTestConfig);
    const auto query = R"([
        {$lookup: {from: "A", localField: "a", foreignField: "b", as: "x", pipeline: [
            {$project: {_id: 0, c: 0}}
        ]}},
        {$unwind: "$x"}
    ])";

    auto pipeline = makePipeline(query, {"A"});
    markFieldsAsScalar(*pipeline, {"a", "b"}, {{"A", {"b", "c"}}});

    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);
    auto& joinModel = swJoinModel.getValue();
    goldenCtx.outStream() << joinModel.toString(true) << std::endl;
}

// Tests that a $project stage in a subpipeline for an $expr-join that does not modify or
// rename the join predicate field succeeds. The projection on non-join fields must be
// preserved in the foreign node's access path.
TEST_F(PipelineAnalyzerTest, SubpipelineExprJoinProjectNotModifyingJoinFieldOk) {
    unittest::GoldenTestContext goldenCtx(&goldenTestConfig);
    const auto query = R"([
        {$lookup: {from: "A", as: "x", let: {aa: "$a"}, pipeline: [
            {$match: {$expr: {$eq: ["$$aa", "$b"]}}},
            {$project: {_id: 0, c: 0}}
        ]}},
        {$unwind: "$x"}
    ])";

    auto pipeline = makePipeline(query, {"A"});
    markFieldsAsScalar(*pipeline, {"a", "b"}, {{"A", {"b", "c"}}});

    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);
    auto& joinModel = swJoinModel.getValue();
    goldenCtx.outStream() << joinModel.toString(true) << std::endl;
}

// Tests that an inclusion $project in the pipeline prefix that excludes the join predicate
// field (localField) causes model construction to bail.
TEST_F(PipelineAnalyzerTest, PrefixProjectInclusionExcludesJoinFieldBails) {
    const auto query = R"([
        {$project: {_id: 0, c: 1}},
        {$lookup: {from: "A", localField: "a", foreignField: "b", as: "x"}},
        {$unwind: "$x"}
    ])";

    auto pipeline = makePipeline(query, {"A"});
    markFieldsAsScalar(*pipeline, {"a", "b", "c"}, {{"A", {"b"}}});

    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    // We bail because the inclusion $project drops field "a" (the localField).
    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_NOT_OK(swJoinModel);
}

// Tests that two joins each having a $project in their subpipeline are correctly handled.
// The first join has a $match + $project, and the second has a $project-only subpipeline.
// Both projections must be preserved in their respective nodes' access paths.
TEST_F(PipelineAnalyzerTest, TwoJoinsEachWithSubpipelineProjectOk) {
    unittest::GoldenTestContext goldenCtx(&goldenTestConfig);
    const auto query = R"([
        {$lookup: {from: "A", localField: "a", foreignField: "b", as: "fromA", pipeline: [
            {$match: {c: {$gt: 0}}},
            {$project: {_id: 0}}
        ]}},
        {$unwind: "$fromA"},
        {$lookup: {from: "B", localField: "a", foreignField: "b", as: "fromB", pipeline: [
            {$project: {_id: 0, d: 0}}
        ]}},
        {$unwind: "$fromB"}
    ])";

    auto pipeline = makePipeline(query, {"A", "B"});
    markFieldsAsScalar(*pipeline, {"a"}, {{"A", {"b"}}, {"B", {"b"}}});

    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);
    auto& joinModel = swJoinModel.getValue();
    goldenCtx.outStream() << joinModel.toString(true) << std::endl;
}

TEST_F(PipelineAnalyzerTest, InvalidPipelinePrefixDetected) {
    const auto query = R"([
        {$match: {a: {$gt: 0}}},
        {$sort: {b: 1}},
        {$lookup: {from: "A", localField: "a", foreignField: "b", as: "fromA"}},
        {$unwind: "$fromA"},
        {$lookup: {from: "B", localField: "a", foreignField: "b", as: "fromB"}},
        {$unwind: "$fromB"}
    ])";

    auto pipeline = makePipeline(query, {"A", "B"});
    markFieldsAsScalar(*pipeline, {"a"}, {{"A", {"b"}}, {"B", {"b"}}});
    ASSERT_FALSE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));
}

TEST_F(PipelineAnalyzerTest, InvalidSubPipelineDetected) {
    const auto query = R"([
        {$lookup: {from: "A", localField: "a", foreignField: "b", as: "fromA", pipeline: [
            {$match: {a: {$gt: 0}}},
            {$sort: {b: 1}}
        ]}},
        {$unwind: "$fromA"},
        {$lookup: {from: "B", localField: "a", foreignField: "b", as: "fromB"}},
        {$unwind: "$fromB"}
    ])";

    auto pipeline = makePipeline(query, {"A", "B"});
    markFieldsAsScalar(*pipeline, {"a"}, {{"A", {"b"}}, {"B", {"b"}}});
    ASSERT_FALSE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));
}

TEST_F(PipelineAnalyzerTest, tooManyNodes) {
    static constexpr size_t numJoins = 5;
    auto pipeline = makePipelineOfSize(numJoins);
    markFieldsAsScalar(*pipeline, {"a"sv}, {{"A", {"b"sv}}});
    // Configure the buildParams that one $lookup/$unwind pair is forced to the suffix because the
    // maximum number of nodes is hit.
    AggModelBuildParams buildParams{
        .joinGraphBuildParams =
            JoinGraphBuildParams(/*maxNodes*/ numJoins, /*maxEdges*/ kHardMaxEdgesInJoin),
        .maxNumberNodesConsideredForImplicitEdges = kMaxNumberNodesConsideredForImplicitEdges};
    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, buildParams);
    ASSERT_OK(swJoinModel);
    // One $lookup with absorbed $unwind was left unoptimized.
    ASSERT_EQ(swJoinModel.getValue().getSuffix()->getSources().size(), 1);
}

TEST_F(PipelineAnalyzerTest, tooManyEdges) {
    static constexpr size_t numJoins = 5;
    auto pipeline = makePipelineOfSize(numJoins);
    markFieldsAsScalar(*pipeline, {"a"sv}, {{"A", {"b"sv}}});
    // Configure the buildParams that one $lookup/$unwind pair is forced to the suffix because the
    // maximum number of edges is hit.
    AggModelBuildParams buildParams{
        .joinGraphBuildParams =
            JoinGraphBuildParams(/*maxNodes*/ kHardMaxNodesInJoin, /*maxEdges*/ numJoins - 1),
        .maxNumberNodesConsideredForImplicitEdges = kMaxNumberNodesConsideredForImplicitEdges};
    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, buildParams);
    ASSERT_OK(swJoinModel);
    // One $lookup with absorbed $unwind was left unoptimized.
    ASSERT_EQ(swJoinModel.getValue().getSuffix()->getSources().size(), 1);
}

TEST_F(PipelineAnalyzerTest, SingleJoinCompoundPredicate) {
    unittest::GoldenTestContext goldenCtx(&goldenTestConfig);
    const auto query = R"([
    {
        $lookup: {
            from: "A",
            let: {foo: "$foo", bar: "$bar"},
            pipeline: [
                {
                    $match: {
                        $expr: {
                            $and: [
                                {$eq: ["$foo", "$$foo"]},
                                {$eq: ["$$bar", "$bar"]}
                            ]
                        }
                    }
                }
            ],
            as: "a"
        }
    },
    {$unwind: "$a"}
    ])";

    auto pipeline = makePipeline(query, {"A"});
    markFieldsAsScalar(*pipeline, {"foo"sv, "bar"sv}, {{"A", {"foo"sv, "bar"sv}}});

    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);
    auto& joinModel = swJoinModel.getValue();
    goldenCtx.outStream() << joinModel.toString(true) << std::endl;
}

TEST_F(PipelineAnalyzerTest, CompoundJoinKeyWithLocalForeignSyntax) {
    unittest::GoldenTestContext goldenCtx(&goldenTestConfig);
    // We expect this graph to add an implicit edge with both foo and bar predicates
    const auto query = R"([
    {
        $lookup: {
            from: "A",
            localField: "foo",
            foreignField: "foo",
            let: {bar: "$bar"},
            pipeline: [
                {
                    $match: {
                        $expr: {
                            $and: [
                                {$eq: ["$bar", "$$bar"]},
                                {$eq: ["$baz", 5]}
                            ]
                        }
                    }
                }
            ],
            as: "a"
        }
    },
    {$unwind: "$a"},
    {
        $lookup: {
            from: "B",
            localField: "foo",
            foreignField: "foo",
            let: {bar: "$bar"},
            pipeline: [
                {
                    $match: {
                        $expr: {
                            $and: [
                                {$eq: ["$bar", "$$bar"]},
                                {$eq: ["$baz", 6]}
                            ]
                        }
                    }
                }
            ],
            as: "b"
        }
    },
    {$unwind: "$b"}
    ])";

    auto pipeline = makePipeline(query, {"A", "B"});
    markFieldsAsScalar(
        *pipeline, {"foo"sv, "bar"sv}, {{"A", {"foo"sv, "bar"sv}}, {"B", {"foo"sv, "bar"sv}}});

    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);
    auto& joinModel = swJoinModel.getValue();
    goldenCtx.outStream() << joinModel.toString(true) << std::endl;
}

TEST_F(PipelineAnalyzerTest, DuplicateExprEqAndEqEdges) {
    unittest::GoldenTestContext goldenCtx(&goldenTestConfig);
    // This test joins on the "bar" field in several different ways, ensuring that we don't keep
    // both an expr and a regular eq predicate on the same edge, while at the same time adding
    // implicit edges as eqs in all cases where we can.
    const auto query = R"([
    {
        $lookup: {
            from: "A",
            localField: "bar",
            foreignField: "bar",
            as: "localForeign"
        }
    },
    {$unwind: "$localForeign"},
    {
        $lookup: {
            from: "B",
            let: {bar: "$bar"},
            pipeline: [
                {
                    $match: {
                        $expr: {$eq: ["$bar", "$$bar"]}
                    }
                }
            ],
            as: "correlatedSubpipeline"
        }
    },
    {$unwind: "$correlatedSubpipeline"},
    {
        $lookup: {
            from: "C",
            localField: "bar",
            foreignField: "bar",
            let: {bar: "$bar"},
            pipeline: [
                {
                    $match: {
                        $expr: {$eq: ["$bar", "$$bar"]}
                    }
                }
            ],
            as: "both"
        }
    },
    {$unwind: "$both"}
    ])";

    auto pipeline = makePipeline(query, {"A", "B", "C"});
    markFieldsAsScalar(
        *pipeline, {"bar"sv}, {{"A", {"bar"sv}}, {"B", {"bar"sv}}, {"C", {"bar"sv}}});

    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);
    auto& joinModel = swJoinModel.getValue();
    goldenCtx.outStream() << joinModel.toString(true) << std::endl;
}

TEST_F(PipelineAnalyzerTest, ExprOnlyImplicitEdges) {
    unittest::GoldenTestContext goldenCtx(&goldenTestConfig);
    const auto query = R"([
    {
        $lookup: {
            from: "A",
            let: {bar: "$bar"},
            pipeline: [
                {
                    $match: {
                        $expr: {$eq: ["$bar", "$$bar"]}
                    }
                }
            ],
            as: "a"
        }
    },
    {$unwind: "$a"},
    {
        $lookup: {
            from: "B",
            let: {bar: "$bar"},
            pipeline: [
                {
                    $match: {
                        $expr: {$eq: ["$bar", "$$bar"]}
                    }
                }
            ],
            as: "b"
        }
    },
    {$unwind: "$b"}
    ])";

    auto pipeline = makePipeline(query, {"A", "B"});
    markFieldsAsScalar(*pipeline, {"bar"sv}, {{"A", {"bar"sv}}, {"B", {"bar"sv}}});

    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);
    auto& joinModel = swJoinModel.getValue();
    goldenCtx.outStream() << joinModel.toString(true) << std::endl;
}

TEST_F(PipelineAnalyzerTest, PipelineIneligibleWithCorrelatedNonJoinPredicate) {
    const auto query = R"([
    {
        $lookup: {
            from: "A",
            localField: "foo",
            foreignField: "foo",
            let: {bar: "$bar"},
            pipeline: [
                {
                    $match: {
                        $expr: {$gt: ["$bar", "$$bar"]}
                    }
                }
            ],
            as: "a"
        }
    },
    {$unwind: "$a"}
    ])";

    auto pipeline = makePipeline(query, {"A", "B"});
    markFieldsAsScalar(*pipeline, {"foo"sv}, {{"A", {"foo"sv}}});

    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_NOT_OK(swJoinModel);
}

TEST_F(PipelineAnalyzerTest, PipelineIneligibleWithNonFieldPathVariable) {
    const auto query = R"([
    {
        $lookup: {
            from: "A",
            localField: "foo",
            foreignField: "foo",
            let: {bar: {$concat: ['$bar', '-suffix']}},
            pipeline: [
                {
                    $match: {
                        $expr: {$eq: ["$bar", "$$bar"]}
                    }
                }
            ],
            as: "a"
        }
    },
    {$unwind: "$a"}
    ])";

    auto pipeline = makePipeline(query, {"A", "B"});
    markFieldsAsScalar(*pipeline, {"foo"sv}, {{"A", {"foo"sv}}});

    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_NOT_OK(swJoinModel);
}

TEST_F(PipelineAnalyzerTest, NumericLocalFieldExprIneligibleJoinPredicate) {
    // Numeric component in the let-bound local field path.
    const auto query = R"([
    {
        $lookup: {
            from: "A",
            let: {x: "$a.0"},
            pipeline: [{$match: {$expr: {$eq: ["$b", "$$x"]}}}],
            as: "fromA"
        }
    },
    {$unwind: "$fromA"}
    ])";

    auto pipeline = makePipeline(query, {"A"});
    markFieldsAsScalar(*pipeline, {"a.0"sv}, {{"A", {"b"sv}}});
    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));
    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_NOT_OK(swJoinModel);
}

TEST_F(PipelineAnalyzerTest, NumericForeignFieldExprIneligibleJoinPredicate) {
    // Numeric component in the foreign field path inside $expr.
    const auto query = R"([
    {
        $lookup: {
            from: "A",
            let: {x: "$a"},
            pipeline: [{$match: {$expr: {$eq: ["$b.0", "$$x"]}}}],
            as: "fromA"
        }
    },
    {$unwind: "$fromA"}
    ])";

    auto pipeline = makePipeline(query, {"A"});
    markFieldsAsScalar(*pipeline, {"a"sv}, {{"A", {"b.0"sv}}});
    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));
    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_NOT_OK(swJoinModel);
}

TEST_F(PipelineAnalyzerTest, NumericMidPathExprIneligibleJoinPredicate) {
    // Numeric component in the middle of a dotted path in the let binding.
    const auto query = R"([
    {
        $lookup: {
            from: "A",
            let: {x: "$a.0.b"},
            pipeline: [{$match: {$expr: {$eq: ["$c", "$$x"]}}}],
            as: "fromA"
        }
    },
    {$unwind: "$fromA"}
    ])";

    auto pipeline = makePipeline(query, {"A"});
    markFieldsAsScalar(*pipeline, {"a.0.b"sv}, {{"A", {"c"sv}}});
    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));
    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_NOT_OK(swJoinModel);
}

TEST_F(PipelineAnalyzerTest, ImplicitEdgeInferenceSelfEdgeSkipped) {
    // Regression test: when both localField ("key") and a let-bound path ("cor.key.foo") from the
    // local collection join to the same foreign field, implicit edge inference transitively infers
    // local.key == local.cor.key.foo. This is a same-node equality, not a join edge, and must be
    // silently skipped rather than trigger a tassert in addEdge().
    const auto query = R"([
    {
        $lookup: {
            from: "base_other",
            localField: "key",
            foreignField: "key",
            let: {f: "$cor.key.foo"},
            pipeline: [{$match: {$expr: {$eq: ["$key", "$$f"]}}}],
            as: "lf"
        }
    },
    { $unwind: "$lf" }
    ])";

    auto pipeline = makePipeline(query, {"base_other"});
    markFieldsAsScalar(*pipeline, {"key"sv, "cor.key.foo"sv}, {{"base_other", {"key"sv}}});
    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));
    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);
    const auto& joinGraph = swJoinModel.getValue().getGraph();
    ASSERT_EQ(joinGraph.numNodes(), 2);
    ASSERT_EQ(joinGraph.numEdges(), 1);
}

TEST_F(PipelineAnalyzerTest, LeadingMatchAfterLimitPushdownBailsOut) {
    // Regression test: a $limit before a $match prevents the $match from being folded into the
    // base collection's CanonicalQuery (the $match runs *after* the limit, so it can't become a
    // find-layer filter). The limit is still pushed down, leaving the $match as the first stage
    // surfacing in the join-building loop -- before any $lookup has been absorbed. That $match
    // can't encode a join predicate (there is no foreign node yet), so we must bail out
    // gracefully rather than trip tassert 11116400.
    const auto query = R"([
        { $limit: 1 },
        { $match: { x: 1 } },
        {
            $lookup: {
                from: "B",
                localField: "x",
                foreignField: "y",
                as: "joined"
            }
        },
        { $unwind: "$joined" }
    ])";

    auto pipeline = makePipeline(query, {"B"});
    markFieldsAsScalar(*pipeline, {"x"sv}, {{"B", {"y"sv}}});
    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_NOT_OK(swJoinModel);
}

TEST_F(PipelineAnalyzerTest, EmbedPathShadowsResolvedPredicatePath) {
    // The first $lookup uses "a.x" as its localField, attributing that path to the base node.
    // A second $lookup whose "as" field is a prefix of, equal to, or a child of "a.x" must be
    // excluded from the join graph: reordering it past the first lookup would overwrite "a.x" in
    // the document and corrupt the first join predicate. The eligible prefix stops at 2 nodes.
    auto getNodeCount = [&](std::string_view secondAs) -> size_t {
        std::string q = std::string(R"([
            {$lookup: {from: "B", localField: "a.x", foreignField: "k", as: "matched"}},
            {$unwind: "$matched"},
            {$lookup: {from: "C", localField: "q", foreignField: "r", as: ")") +
            std::string(secondAs) + R"("}},
            {$unwind: "$)" +
            std::string(secondAs) + R"("}
        ])";
        auto pipeline = makePipeline(q, {"B", "C"});
        markFieldsAsScalar(*pipeline, {"a.x", "q"}, {{"B", {"k"}}, {"C", {"r"}}});
        ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));
        auto sw = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
        ASSERT_OK(sw);
        return sw.getValue().getGraph().numNodes();
    };

    // "a" is a prefix of the resolved "a.x" → shadow conflict → second lookup excluded (2 nodes).
    ASSERT_EQ(2u, getNodeCount("a"));
    // "a.x" exactly matches the resolved path → also excluded (2 nodes).
    ASSERT_EQ(2u, getNodeCount("a.x"));
    // "a.x.y" extends the resolved path → also excluded (2 nodes).
    ASSERT_EQ(2u, getNodeCount("a.x.y"));
    // "a.y" is a sibling with no overlap → second lookup included (3 nodes).
    ASSERT_EQ(3u, getNodeCount("a.y"));
    // "b" is unrelated → second lookup included (3 nodes).
    ASSERT_EQ(3u, getNodeCount("b"));
}

}  // namespace
}  // namespace mongo::join_ordering
