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

#include "mongo/db/query/compiler/optimizer/join/agg_join_model.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/query/compiler/optimizer/join/agg_join_model_fixture.h"
#include "mongo/db/query/compiler/parsers/matcher/parsed_match_expression_for_test.h"
#include "mongo/unittest/golden_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo::join_ordering {
namespace {
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
    RAIIServerParameterControllerForTest knob{"internalInferSingleTablePredicates", true};
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
    markFieldsAsScalar(*pipeline, {"a"_sd}, {{"B", {"a"_sd}}});
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
    RAIIServerParameterControllerForTest knob{"internalInferSingleTablePredicates", true};
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
    markFieldsAsScalar(*pipeline, {"a"_sd}, {{"B", {"b"_sd}}});
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
    RAIIServerParameterControllerForTest knob{"internalInferSingleTablePredicates", true};
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
    markFieldsAsScalar(*pipeline, {"a"_sd}, {{"B", {"b"_sd}}});
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
    RAIIServerParameterControllerForTest knob{"internalInferSingleTablePredicates", true};
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
    markFieldsAsScalar(*pipeline, {"a"_sd, "b"_sd}, {{"B", {"a"_sd, "b"_sd}}, {"C", {"a"_sd}}});
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
    RAIIServerParameterControllerForTest knob{"internalInferSingleTablePredicates", true};
    /**
     * For joins B.b = A.a, B.x = A.x and STPs B.x ∈ {100,200,300} and B.p = 10 and B.m = 500, we
     * propagate A.x ∈ {100,200,300}.
     *
     * For joins C.c = joinedB.b, C.x = joinedB.x, C.n = B.m and STP C.q = 42, we propagate C.x ∈
     * {100,200,300} and C.n = 500.
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
                                { $or: [
                                    { $eq: ["$x", 100] },
                                    { $eq: ["$x", 200] },
                                    { $eq: ["$x", 300] }
                                ] },
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
    markFieldsAsScalar(*pipeline,
                       {"a"_sd, "x"_sd},
                       {{"B", {"b"_sd, "x"_sd, "m"_sd}},
                        {"C", {"c"_sd, "x"_sd, "n"_sd}},
                        {"D", {"d"_sd, "o"_sd}}});
    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));
    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);
    auto& joinModel = swJoinModel.getValue();
    auto& joinGraph = joinModel.getGraph();
    ASSERT_EQ(joinGraph.numNodes(), 4);

    auto* baseCollCQ = joinGraph.accessPathAt((NodeId)0);
    ASSERT_EQ(baseCollCQ->nss().coll(), "pipeline_test");
    auto expectedChildren =
        "{ $and: [ { $or: [ { x: { $_internalExprEq: 100 } }, { x: { $_internalExprEq: 200 } }, { "
        "x: { $_internalExprEq: 300 } } ] }, { y: { $eq: 5 } }, { $expr: { $or: [ { $eq: [ \"$x\", "
        "{ $const: 100 } ] }, { $eq: [ \"$x\", { $const: 200 } ] }, { $eq: [ \"$x\", { $const: 300 "
        "} ] } ] } } ] }";
    ASSERT_EQ(expectedChildren, baseCollCQ->getPrimaryMatchExpression()->toString());

    auto* bCollCQ = joinGraph.accessPathAt((NodeId)1);
    ASSERT_EQ(bCollCQ->nss().coll(), "B");
    expectedChildren =
        "{ $and: [ { $or: [ { x: { $_internalExprEq: 100 } }, { x: { $_internalExprEq: 200 } }, { "
        "x: { $_internalExprEq: 300 } } ] }, { $expr: { $and: [ { $or: [ { $eq: [ \"$x\", { "
        "$const: 100 } ] }, { $eq: [ \"$x\", { $const: 200 } ] }, { $eq: [ \"$x\", { $const: 300 } "
        "] } ] }, { $eq: [ \"$p\", { $const: 10 } ] }, { $eq: [ \"$m\", { $const: 500 } ] } ] } }, "
        "{ m: { $_internalExprEq: 500 } }, { p: { $_internalExprEq: 10 } } ] }";
    ASSERT_EQ(expectedChildren, bCollCQ->getPrimaryMatchExpression()->toString());

    auto* cCollCQ = joinGraph.accessPathAt((NodeId)2);
    ASSERT_EQ(cCollCQ->nss().coll(), "C");
    std::vector<std::string> expectedChildrenVec = {
        "{ $expr: { $eq: [ \"$n\", { $const: 500 } ] } }",
        "{ $expr: { $eq: [ \"$q\", { $const: 42 } ] } }",
        "{ $expr: { $or: [ { $eq: [ \"$x\", { $const: 100 } ] }, { $eq: [ \"$x\", { $const: 200 } "
        "] }, { $eq: [ \"$x\", { $const: 300 } ] } ] } }",
        "{ $or: [ { x: { $_internalExprEq: 100 } }, { x: { $_internalExprEq: 200 } }, { x: { "
        "$_internalExprEq: 300 } } ] }",
        "{ n: { $_internalExprEq: 500 } }",
        "{ q: { $_internalExprEq: 42 } }"};

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

TEST_F(PipelineAnalyzerTest, PreserveEqExprSemantics) {
    RAIIServerParameterControllerForTest knob{"internalInferSingleTablePredicates", true};
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
    markFieldsAsScalar(*pipeline, {"a"_sd, "c"_sd}, {{"B", {"b"_sd}}, {"C", {"c"_sd}}});
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
    RAIIServerParameterControllerForTest knob{"internalInferSingleTablePredicates", true};
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
    markFieldsAsScalar(*pipeline,
                       {"x"_sd, "z"_sd},
                       {{"B", {"x"_sd, "y"_sd}},
                        {"C", {"y"_sd, "z"_sd, "x"_sd, "w"_sd}},
                        {"D", {"w"_sd, "p"_sd}}});
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
    markFieldsAsScalar(*pipeline, {"a"_sd}, {{"A", {"b"_sd}}});

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
    markFieldsAsScalar(*pipeline, {"a"_sd}, {{"A", {"b"_sd}}});
    // This pipeline is eligible for reordering.
    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);

    auto& joinModel = swJoinModel.getValue();
    goldenCtx.outStream() << joinModel.toString(true) << std::endl;
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
    markFieldsAsScalar(*pipeline, {"a"_sd}, {{"A", {"b"_sd}}, {"B", {"b"_sd}}});

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
    markFieldsAsScalar(*pipeline, {"a"_sd}, {{"A", {"b"_sd}}, {"B", {"b"_sd}}});

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
    markFieldsAsScalar(*pipeline, {"a"_sd}, {{"A", {"b"_sd}}, {"B", {"b"_sd}}});

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
    markFieldsAsScalar(*pipeline, {"a"_sd}, {{"A", {"b"_sd}}});

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
    markFieldsAsScalar(*pipeline, {"a"_sd}, {{"A", {"b"_sd}}});

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
    markFieldsAsScalar(*pipeline, {"a"_sd}, {{"A", {"b"_sd}}});

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
    markFieldsAsScalar(*pipeline, {"a.0"_sd}, {{"A", {"b"_sd}}});
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
    markFieldsAsScalar(*pipeline, {"a"_sd}, {{"A", {"b.0"_sd}}});
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
    markFieldsAsScalar(*pipeline, {"a.0.b"_sd}, {{"A", {"c"_sd}}});
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
    markFieldsAsScalar(*pipeline, {"a"_sd}, {{"A", {"b"_sd}}});

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
    markFieldsAsScalar(*pipeline, {"a"_sd}, {{"A", {"b"_sd}}});

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
    markFieldsAsScalar(*pipeline, {"a"_sd}, {{"A", {"b"_sd}}});

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
    markFieldsAsScalar(*pipeline, {"a"_sd}, {{"A", {"b"_sd}}});

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
    markFieldsAsScalar(*pipeline, {"a"_sd}, {{"A", {"b"_sd}}});

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
    markFieldsAsScalar(*pipeline, {"a"_sd}, {{"A", {"b"_sd}}});

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
    markFieldsAsScalar(*pipeline, {"a"_sd}, {{"A", {"b"_sd}}});

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
    markFieldsAsScalar(*pipeline, {"a"_sd}, {{"A", {"b"_sd}}});

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
    markFieldsAsScalar(*pipeline, {"a"_sd}, {{"A", {"b"_sd}}});

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
    markFieldsAsScalar(*pipeline, {"a"_sd}, {{"A", {"b"_sd}}});

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
    markFieldsAsScalar(*pipeline, {"a"_sd}, {{"A", {"b"_sd}}});

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
    markFieldsAsScalar(*pipeline, {"a"_sd}, {{"A", {"b"_sd}}});

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
    markFieldsAsScalar(*pipeline, {"a"_sd}, {{"A", {"b"_sd}}});

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
    markFieldsAsScalar(*pipeline, {"a"_sd}, {{"A", {"b"_sd}}});

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
    markFieldsAsScalar(*pipeline, {"a"_sd}, {{"A", {"b"_sd}}});

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
    markFieldsAsScalar(*pipeline, {"a"_sd}, {{"A", {"b"_sd}}});

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
    markFieldsAsScalar(*pipeline, {"a"_sd}, {{"A", {"b"_sd}}, {"B", {"c"_sd}}});

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
    markFieldsAsScalar(*pipeline, {"a"_sd}, {{"A", {"b"_sd}}, {"B", {"c"_sd}}});

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
    markFieldsAsScalar(*pipeline, {"a"_sd}, {{"A", {"b"_sd}}});

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
    markFieldsAsScalar(*pipeline, {"a"_sd}, {{"A", {"b"_sd}}, {"B", {"c"_sd}}});

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
    markFieldsAsScalar(*pipeline, {"a"_sd}, {{"A", {"b"_sd, "c"_sd}}, {"B", {"d"_sd}}});

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
    markFieldsAsScalar(*pipeline, {"a"_sd}, {{"A", {"b"_sd}}, {"B", {"b"_sd}}});

    // We don't detect ineligibility here.
    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    // But we fail to construct a model here, because $group isn't pushed into SBE.
    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_EQ(swJoinModel.getStatus(), ErrorCodes::QueryFeatureNotAllowed);
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
    markFieldsAsScalar(*pipeline, {"x"_sd, "a"_sd}, {{"B", {"y"_sd}}, {"C", {"z"_sd}}});
    // We don't detect ineligibility of local path fields here.
    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));
    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_EQ(swJoinModel.getStatus(), ErrorCodes::BadValue);
    ASSERT_EQ(swJoinModel.getStatus().reason(), "Local path could not be resolved");
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
        *pipeline, {"x"_sd, "foo"_sd, "bar"_sd}, {{"B", {"y"_sd}}, {"A", {"foo"_sd, "bar"_sd}}});
    // We don't detect ineligibility of local path fields here.
    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));
    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_EQ(swJoinModel.getStatus(), ErrorCodes::BadValue);
    ASSERT_EQ(swJoinModel.getStatus().reason(), "Local path could not be resolved");
}

TEST_F(PipelineAnalyzerTest, CompatibleAsFields) {
    auto query = R"([
            {$lookup: {from: "B", as: "x.y", localField: "x.c", foreignField: "c"}},
            {$unwind: "$x.y"},
            {$lookup: {from: "C", as: "x.z", localField: "x.y.d", foreignField: "d"}},
            {$unwind: "$x.z"}
        ])";
    auto pipeline = makePipeline(query, {"B", "C"});
    markFieldsAsScalar(*pipeline, {"x.c"_sd}, {{"B", {"c"_sd, "d"_sd}}, {"C", {"d"_sd}}});

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
    markFieldsAsScalar(*pipeline, {"a"_sd}, {{"A", {"b"_sd}}});

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
    markFieldsAsScalar(*pipeline, {"a"_sd}, {{"A", {"b"_sd}}, {"B", {"b"_sd}}});

    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_NOT_OK(swJoinModel);

    // Ensure we haven't modified our pipeline.
    auto serializedPipeline = pipeline->serializeToBson();
    BSONObjBuilder bob;
    {
        BSONArrayBuilder bar(bob.subarrayStart("pipeline"));
        for (auto&& stage : serializedPipeline) {
            bar.append(stage);
        }
    }
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT,
        R"({
            "pipeline": [
                {
                    "$lookup": {
                        "from": "A",
                        "as": "fromA",
                        "localField": "a",
                        "foreignField": "b",
                        "let": {},
                        "pipeline": [
                            {
                                "$group": {
                                    "_id": "$key",
                                    "a": {
                                        "$avg": "$c"
                                    },
                                    "$willBeMerged": false
                                }
                            }
                        ]
                    }
                },
                {
                    "$unwind": {
                        "path": "$fromA"
                    }
                },
                {
                    "$lookup": {
                        "from": "B",
                        "as": "fromB",
                        "localField": "a",
                        "foreignField": "b"
                    }
                },
                {
                    "$unwind": {
                        "path": "$fromB"
                    }
                }
            ]
        })",
        bob.obj());
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
    markFieldsAsScalar(*pipeline, {"a"_sd}, {{"A", {"b"_sd}}, {"B", {"b"_sd}}});

    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_NOT_OK(swJoinModel);

    // Ensure we haven't modified our pipeline.
    auto serializedPipeline = pipeline->serializeToBson();
    BSONObjBuilder bob;
    {
        BSONArrayBuilder bar(bob.subarrayStart("pipeline"));
        for (auto&& stage : serializedPipeline) {
            bar.append(stage);
        }
    }
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT,
        R"({
            "pipeline": [
                {
                    "$lookup": {
                        "from": "A",
                        "as": "fromA",
                        "localField": "a",
                        "foreignField": "b",
                        "let": {},
                        "pipeline": [
                            {
                                "$lookup": {
                                    "from": "B",
                                    "as": "innerB",
                                    "localField": "b",
                                    "foreignField": "b"
                                }
                            }
                        ]
                    }
                },
                {
                    "$unwind": {
                        "path": "$fromA"
                    }
                },
                {
                    "$lookup": {
                        "from": "B",
                        "as": "fromB",
                        "localField": "a",
                        "foreignField": "b"
                    }
                },
                {
                    "$unwind": {
                        "path": "$fromB"
                    }
                }
            ]
        })",
        bob.obj());
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
    markFieldsAsScalar(*pipeline, {"a"_sd}, {{"A", {"b"_sd}}, {"B", {"b"_sd}}});

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
    markFieldsAsScalar(*pipeline, {"a"_sd}, {{"A", {"b"_sd}}, {"B", {"b"_sd}}});
    // This is not where we examine the pipeline for a $sort stage.
    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));
    auto status = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams).getStatus();
    ASSERT_NOT_OK(status);
    ASSERT_EQ(status.code(), ErrorCodes::BadValue);
    ASSERT_STRING_CONTAINS(status.reason(), "Sort stage found in pipeline");

    const auto sortSubPipelineQuery = R"([
        {$lookup: {
            from: "A",
            localField: "a",
            foreignField: "b",
            as: "fromA",
            pipeline: [{$sort: {b: -1}}]
        }},
        {$unwind: "$fromA"}
    ])";
    pipeline = makePipeline(sortSubPipelineQuery, {"A", "B"});
    // We check $lookup subpipeline here, so this method will flag the $sort in the subpipeline.
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
    markFieldsAsScalar(*pipeline, {"a"_sd, "b"_sd}, {{"A", {"b"_sd}}, {"B", {"b"_sd}}});

    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);
    auto& joinModel = swJoinModel.getValue();
    goldenCtx.outStream() << joinModel.toString(true) << std::endl;
}

TEST_F(PipelineAnalyzerTest, tooManyNodes) {
    static constexpr size_t numJoins = 5;
    auto pipeline = makePipelineOfSize(numJoins);
    markFieldsAsScalar(*pipeline, {"a"_sd}, {{"A", {"b"_sd}}});
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
    markFieldsAsScalar(*pipeline, {"a"_sd}, {{"A", {"b"_sd}}});
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
    markFieldsAsScalar(*pipeline, {"foo"_sd, "bar"_sd}, {{"A", {"foo"_sd, "bar"_sd}}});

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
    markFieldsAsScalar(*pipeline,
                       {"foo"_sd, "bar"_sd},
                       {{"A", {"foo"_sd, "bar"_sd}}, {"B", {"foo"_sd, "bar"_sd}}});

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
        *pipeline, {"bar"_sd}, {{"A", {"bar"_sd}}, {"B", {"bar"_sd}}, {"C", {"bar"_sd}}});

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
    markFieldsAsScalar(*pipeline, {"bar"_sd}, {{"A", {"bar"_sd}}, {"B", {"bar"_sd}}});

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
    markFieldsAsScalar(*pipeline, {"foo"_sd}, {{"A", {"foo"_sd}}});

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
    markFieldsAsScalar(*pipeline, {"foo"_sd}, {{"A", {"foo"_sd}}});

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
    markFieldsAsScalar(*pipeline, {"a.0"_sd}, {{"A", {"b"_sd}}});
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
    markFieldsAsScalar(*pipeline, {"a"_sd}, {{"A", {"b.0"_sd}}});
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
    markFieldsAsScalar(*pipeline, {"a.0.b"_sd}, {{"A", {"c"_sd}}});
    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));
    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_NOT_OK(swJoinModel);
}

}  // namespace
}  // namespace mongo::join_ordering
