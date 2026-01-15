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
#include "mongo/db/query/compiler/optimizer/join/agg_join_model_fixture.h"
#include "mongo/unittest/golden_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo::join_ordering {
class AggJoinModelGoldenTest : public AggJoinModelFixture {
public:
    static constexpr size_t kMaxNumberNodesConsideredForImplicitEdges = 4;

    AggJoinModelGoldenTest() : _cfg{"src/mongo/db/test_output/query/compiler/optimizer/join"} {}

    StatusWith<AggJoinModel> runVariation(
        std::unique_ptr<Pipeline> pipeline,
        StringData variationName,
        boost::optional<AggModelBuildParams> buildParams = boost::none) {
        unittest::GoldenTestContext ctx(&_cfg);

        ctx.outStream() << "VARIATION " << variationName << std::endl;
        ctx.outStream() << "input " << toString(pipeline) << std::endl;

        auto joinModel = AggJoinModel::constructJoinModel(
            *pipeline, buildParams.get_value_or(defaultBuildParams));

        if (joinModel.isOK()) {
            ctx.outStream() << "output: " << joinModel.getValue().toString(/*pretty*/ true)
                            << std::endl;
        } else {
            ctx.outStream() << "output: " << joinModel.getStatus() << std::endl;
        }

        ctx.outStream() << std::endl;

        return joinModel;
    }

    size_t numPredicates(const JoinGraph& joinGraph) {
        size_t numPredicates = 0;
        for (const auto& edge : joinGraph.edges()) {
            numPredicates += edge.predicates.size();
        }
        return numPredicates;
    }

    unittest::GoldenTestConfig _cfg;
};

TEST_F(AggJoinModelGoldenTest, longPrefix) {
    const auto query = R"([
            {$match: {c: 1, h: 12}},
            {$sort: {e: 1}},
            {$project: {k: 0}},
            {$lookup: {from: "A", localField: "a", foreignField: "b", as: "fromA"}},
            {$unwind: "$fromA"},
            {$lookup: {from: "B", localField: "a", foreignField: "b", as: "fromB"}},
            {$unwind: "$fromB"}
        ])";
    auto pipeline = makePipeline(query, {"A", "B"});
    auto joinModel = runVariation(std::move(pipeline), "longPrefix");
    ASSERT_OK(joinModel);
}

TEST_F(AggJoinModelGoldenTest, veryLargePipeline) {
    auto pipeline = makePipelineOfSize(/*numJoins*/ kHardMaxNodesInJoin + 3);
    auto joinModel = runVariation(std::move(pipeline), "veryLargePipeline");
    ASSERT_OK(joinModel);
}

/**
 * The test case with three nodes: A.b = base.a = B.b;
 * one implicit precicate: A.b = B.b.
 */
TEST_F(AggJoinModelGoldenTest, addImplicitEdges_OneImplictEdge) {
    const auto query = R"([
            {$lookup: {from: "A", localField: "a", foreignField: "b", as: "fromA"}},
            {$unwind: "$fromA"},
            {$lookup: {from: "B", localField: "a", foreignField: "b", as: "fromB"}},
            {$unwind: "$fromB"}
        ])";
    auto pipeline = makePipeline(query, {"A", "B"});
    auto joinModel = runVariation(std::move(pipeline), "addImplicitEdges_OneImplictEdge");
    ASSERT_OK(joinModel);
    ASSERT_EQ(joinModel.getValue().graph.numNodes(), 3);
    ASSERT_EQ(joinModel.getValue().graph.numEdges(), 3);
}

/**
 * The test case with four nodes nodes: base.a = A.a = B.b = C.c;
 * three implicit predicates: base.a = B.b, base.a = C.c, A.a = C.c
 */
TEST_F(AggJoinModelGoldenTest, addImplicitEdges_MultipleImplictEdges) {
    const auto query = R"([
            {$lookup: {from: "A", localField: "a", foreignField: "a", as: "fromA"}},
            {$unwind: "$fromA"},
            {$lookup: {from: "B", localField: "fromA.a", foreignField: "b", as: "fromB"}},
            {$unwind: "$fromB"},
            {$lookup: {from: "C", localField: "fromB.b", foreignField: "c", as: "fromC"}},
            {$unwind: "$fromC"}
        ])";
    auto pipeline = makePipeline(query, {"A", "B", "C"});
    auto joinModel = runVariation(std::move(pipeline), "addImplicitEdges_MultipleImplictEdges");
    ASSERT_OK(joinModel);
    ASSERT_EQ(joinModel.getValue().graph.numNodes(), 4);
    ASSERT_EQ(joinModel.getValue().graph.numEdges(), 6);
}

/**
 * The test case with two connected components:
 * - base.a = A.a = B.b = C.c,
 * - C.d = D.d = E.e
 * implicit predicates:
 * - base.a = B.b, base.a = C.c, A.a = C.c
 * - C.d = E.e
 */
TEST_F(AggJoinModelGoldenTest, addImplicitEdges_TwoConnectedComponents) {
    const auto query = R"([
            {$lookup: {from: "A", localField: "a", foreignField: "a", as: "fromA"}},
            {$unwind: "$fromA"},
            {$lookup: {from: "B", localField: "fromA.a", foreignField: "b", as: "fromB"}},
            {$unwind: "$fromB"},
            {$lookup: {from: "C", localField: "fromB.b", foreignField: "c", as: "fromC"}},
            {$unwind: "$fromC"},
            {$lookup: {from: "D", localField: "fromC.d", foreignField: "d", as: "fromD"}},
            {$unwind: "$fromD"},
            {$lookup: {from: "E", localField: "fromD.d", foreignField: "e", as: "fromE"}},
            {$unwind: "$fromE"}
        ])";
    auto pipeline = makePipeline(query, {"A", "B", "C", "D", "E"});
    auto joinModel = runVariation(std::move(pipeline), "addImplicitEdges_TwoConnectedComponents");
    ASSERT_OK(joinModel);
    ASSERT_EQ(joinModel.getValue().graph.numNodes(), 6);
    ASSERT_EQ(joinModel.getValue().graph.numEdges(), 9);
}

/**
 * The test case without connected components:
 * - base.a = A.a, A.b = B.b, B.c = C.c, C.d = D.d, D.e = E.e
 * implicit predicates: none
 */
TEST_F(AggJoinModelGoldenTest, addImplicitEdges_NoImplicitEdges) {
    const auto query = R"([
            {$lookup: {from: "A", localField: "a", foreignField: "a", as: "fromA"}},
            {$unwind: "$fromA"},
            {$lookup: {from: "B", localField: "fromA.b", foreignField: "b", as: "fromB"}},
            {$unwind: "$fromB"},
            {$lookup: {from: "C", localField: "fromB.c", foreignField: "c", as: "fromC"}},
            {$unwind: "$fromC"},
            {$lookup: {from: "D", localField: "fromC.d", foreignField: "d", as: "fromD"}},
            {$unwind: "$fromD"},
            {$lookup: {from: "E", localField: "fromD.e", foreignField: "e", as: "fromE"}},
            {$unwind: "$fromE"}
        ])";
    auto pipeline = makePipeline(query, {"A", "B", "C", "D", "E"});
    auto joinModel = runVariation(std::move(pipeline), "addImplicitEdges_NoImplicitEdges");
    ASSERT_OK(joinModel);
    ASSERT_EQ(joinModel.getValue().graph.numNodes(), 6);
    ASSERT_EQ(joinModel.getValue().graph.numEdges(), 5);
}

/**
 * local/foreignFields specify edges: base -- A, base -- B, base -- C, base -- D;
 * $expr predicates specify edges: A -- B, B -- B, C -- C, D -- D;
 * all $expr's are defined in one big $match at the end of the pipeline.
 */
TEST_F(AggJoinModelGoldenTest, addEdgesFromExpr_predicatesAtEnd) {
    const auto query = R"([
            {$lookup: {from: "A", localField: "s1", foreignField: "s1", as: "fromA"}},
            {$unwind: "$fromA"},
            {$lookup: {from: "B", localField: "s2", foreignField: "s2", as: "fromB"}},
            {$unwind: "$fromB"},
            {$lookup: {from: "C", localField: "s3", foreignField: "s3", as: "fromC"}},
            {$unwind: "$fromC"},
            {$lookup: {from: "D", localField: "s4", foreignField: "s4", as: "fromD"}},
            {$unwind: "$fromD"},
            {$match: {$and: [
                {$expr: {$eq: ["$fromA.a", "$fromB.a"]}},
                {$expr: {$eq: ["$fromB.b", "$fromC.b"]}},
                {$expr: {$eq: ["$fromC.c", "$fromD.c"]}},
                {$expr: {$eq: ["$fromD.d", "$fromA.d"]}}
                ]}
            }
        ])";
    auto pipeline = makePipeline(query, {"A", "B", "C", "D"});
    auto joinModel = runVariation(std::move(pipeline), "addEdgesFromExpr_predicatesAtEnd");
    ASSERT_OK(joinModel);
    ASSERT_EQ(joinModel.getValue().graph.numNodes(), 5);
    ASSERT_EQ(joinModel.getValue().graph.numEdges(), 8);
}

/**
 * local/foreignFields specify edges: base -- A, base -- B, base -- C, base -- D;
 * $expr predicates specify edges: A -- B, B -- B, C -- C, D -- D;
 * $expr's are defined in separate $match stages inside and at the end of the pipeline.
 */
TEST_F(AggJoinModelGoldenTest, addEdgesFromExpr_predicatesInBetween) {
    const auto query = R"([
            {$lookup: {from: "A", localField: "s1", foreignField: "s1", as: "fromA"}},
            {$unwind: "$fromA"},
            {$lookup: {from: "B", localField: "s2", foreignField: "s2", as: "fromB"}},
            {$unwind: "$fromB"},
            {$match: {$expr: {$eq: ["$fromA.a", "$fromB.a"]}}},
            {$lookup: {from: "C", localField: "s3", foreignField: "s3", as: "fromC"}},
            {$unwind: "$fromC"},
            {$match: {$expr: {$eq: ["$fromB.b", "$fromC.b"]}}},
            {$lookup: {from: "D", localField: "s4", foreignField: "s4", as: "fromD"}},
            {$unwind: "$fromD"},
            {$match: {$expr: {$eq: ["$fromC.c", "$fromD.c"]}}},
            {$match: {$expr: {$eq: ["$fromD.d", "$fromA.d"]}}}
        ])";
    auto pipeline = makePipeline(query, {"A", "B", "C", "D"});
    auto joinModel = runVariation(std::move(pipeline), "addEdgesFromExpr_predicatesInBetween");
    ASSERT_OK(joinModel);
    ASSERT_EQ(joinModel.getValue().graph.numNodes(), 5);
    ASSERT_EQ(joinModel.getValue().graph.numEdges(), 8);
}

/**
 * local/foreignFields define two edges: base -- A, base -- B
 * $expr define one edge: B - C;
 * $expr specifies an edge between the same node, which cannot be absorbed, so we stop building
 * graph earlier.
 */
TEST_F(AggJoinModelGoldenTest, addEdgesFromExpr_earlyEnd) {
    const auto query = R"([
            {$lookup: {from: "A", localField: "s1", foreignField: "s1", as: "fromA"}},
            {$unwind: "$fromA"},
            {$lookup: {from: "B", localField: "s2", foreignField: "s2", as: "fromB"}},
            {$unwind: "$fromB"},
            {$match: {$and: [
                {$expr: {$eq: ["$fromA.a", "$fromB.a"]}},
                {$expr: {$eq: ["$fromB.b", "$fromB.s2"]}}
                ]}
            },
            {$lookup: {from: "C", localField: "s3", foreignField: "s3", as: "fromC"}},
            {$unwind: "$fromC"},
            {$lookup: {from: "D", localField: "s4", foreignField: "s4", as: "fromD"}},
            {$unwind: "$fromD"}
        ])";
    auto pipeline = makePipeline(query, {"A", "B", "C", "D"});
    auto joinModel = runVariation(std::move(pipeline), "addEdgesFromExpr_earlyEnd");
    ASSERT_OK(joinModel);
    ASSERT_EQ(joinModel.getValue().graph.numNodes(), 3);
    ASSERT_EQ(joinModel.getValue().graph.numEdges(), 3);
}

/**
 * Combined test of $expr and implicit edges.
 * Legend: '==' - local/foreignField edge; '--' - $expr edge.
 * Connected Component 1:  base.a == A.a -- D.a. 1 Implicit edge is expected.
 * Connected Component 2: A.b == B.b -- C.c == D.d. 2 Implicit edges are expected
 * Standalone edge: B.s == C.s
 * Total: 5 nodes; 5 local/foreignFields predicates + 2 $expr predicates + 3 implicit predicates =
 * 10 total predicates.
 */
TEST_F(AggJoinModelGoldenTest, addEdgesFromExpr_addImplicitEdge_subPipelineEdge) {
    const auto query = R"([
            {$lookup: {from: "A", localField: "a", foreignField: "a", as: "fromA"}},
            {$unwind: "$fromA"},
            {$lookup: {from: "B", localField: "fromA.b", foreignField: "b", as: "fromB"}},
            {$unwind: "$fromB"},
            {$lookup: {from: "C", localField: "fromB.s", foreignField: "s", as: "fromC"}},
            {$unwind: "$fromC"},
            {$lookup: {from: "D", localField: "fromC.c", foreignField: "d", as: "fromD"}},
            {$unwind: "$fromD"},
            {$match: {$expr: {$eq: ["$fromA.a", "$fromD.a"]}}},
            {$match: {$expr: {$eq: ["$fromB.b", "$fromC.c"]}}}
        ])";
    auto pipeline = makePipeline(query, {"A", "B", "C", "D"});
    auto joinModel = runVariation(std::move(pipeline), "addEdgesFromExpr_earlyEnd");
    ASSERT_OK(joinModel);
    ASSERT_EQ(joinModel.getValue().graph.numNodes(), 5);
    ASSERT_EQ(joinModel.getValue().graph.numEdges(), 8);
    ASSERT_EQ(numPredicates(joinModel.getValue().graph), 10);
}

}  // namespace mongo::join_ordering
