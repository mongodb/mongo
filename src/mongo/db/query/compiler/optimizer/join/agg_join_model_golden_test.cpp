// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/optimizer/join/agg_join_model.h"
#include "mongo/db/query/compiler/optimizer/join/agg_join_model_fixture.h"
#include "mongo/unittest/golden_test.h"
#include "mongo/unittest/unittest.h"

#include <string_view>

namespace mongo::join_ordering {
using namespace std::literals::string_view_literals;
class AggJoinModelGoldenTest : public AggJoinModelFixture {
public:
    static constexpr size_t kMaxNumberNodesConsideredForImplicitEdges = 4;

    AggJoinModelGoldenTest() : _cfg{"src/mongo/db/test_output/query/compiler/optimizer/join"} {}

    StatusWith<AggJoinModel> runVariation(
        std::unique_ptr<Pipeline> pipeline,
        std::string_view variationName,
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
            {$project: {k: 0}},
            {$lookup: {from: "A", localField: "a", foreignField: "b", as: "fromA"}},
            {$unwind: "$fromA"},
            {$lookup: {from: "B", localField: "a", foreignField: "b", as: "fromB"}},
            {$unwind: "$fromB"}
        ])";
    auto pipeline = makePipeline(query, {"A", "B"});
    markFieldsAsScalar(*pipeline, {"a"sv}, {{"A", {"b"sv}}, {"B", {"b"sv}}});
    auto joinModel = runVariation(std::move(pipeline), "longPrefix");
    ASSERT_OK(joinModel);
}

TEST_F(AggJoinModelGoldenTest, veryLargePipeline) {
    auto pipeline = makePipelineOfSize(/*numJoins*/ kHardMaxNodesInJoin + 3);
    markFieldsAsScalar(*pipeline, {"a"sv}, {{"A", {"b"sv}}});
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
    markFieldsAsScalar(*pipeline, {"a"sv}, {{"A", {"b"sv}}, {"B", {"b"sv}}});
    auto joinModel = runVariation(std::move(pipeline), "addImplicitEdges_OneImplictEdge");
    ASSERT_OK(joinModel);
    ASSERT_EQ(joinModel.getValue().getGraph().numNodes(), 3);
    ASSERT_EQ(joinModel.getValue().getGraph().numEdges(), 3);
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
    markFieldsAsScalar(*pipeline, {"a"sv}, {{"A", {"a"sv}}, {"B", {"b"sv}}, {"C", {"c"sv}}});
    auto joinModel = runVariation(std::move(pipeline), "addImplicitEdges_MultipleImplictEdges");
    ASSERT_OK(joinModel);
    ASSERT_EQ(joinModel.getValue().getGraph().numNodes(), 4);
    ASSERT_EQ(joinModel.getValue().getGraph().numEdges(), 6);
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
    markFieldsAsScalar(
        *pipeline,
        {"a"sv},
        {{"A", {"a"sv}}, {"B", {"b"sv}}, {"C", {"c"sv, "d"sv}}, {"D", {"d"sv}}, {"E", {"e"sv}}});
    auto joinModel = runVariation(std::move(pipeline), "addImplicitEdges_TwoConnectedComponents");
    ASSERT_OK(joinModel);
    ASSERT_EQ(joinModel.getValue().getGraph().numNodes(), 6);
    ASSERT_EQ(joinModel.getValue().getGraph().numEdges(), 9);
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
    markFieldsAsScalar(*pipeline,
                       {"a"sv},
                       {{"A", {"a"sv, "b"sv}},
                        {"B", {"b"sv, "c"sv}},
                        {"C", {"c"sv, "d"sv}},
                        {"D", {"d"sv, "e"sv}},
                        {"E", {"e"sv}}});
    auto joinModel = runVariation(std::move(pipeline), "addImplicitEdges_NoImplicitEdges");
    ASSERT_OK(joinModel);
    ASSERT_EQ(joinModel.getValue().getGraph().numNodes(), 6);
    ASSERT_EQ(joinModel.getValue().getGraph().numEdges(), 5);
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
    markFieldsAsScalar(*pipeline,
                       {"s1"sv, "s2"sv, "s3"sv, "s4"sv},
                       {{"A", {"s1"sv, "a"sv, "d"sv}},
                        {"B", {"s2"sv, "a"sv, "b"sv}},
                        {"C", {"s3"sv, "b"sv, "c"sv}},
                        {"D", {"s4"sv, "c"sv, "d"sv}}});

    auto joinModel = runVariation(std::move(pipeline), "addEdgesFromExpr_predicatesAtEnd");
    ASSERT_OK(joinModel);
    ASSERT_EQ(joinModel.getValue().getGraph().numNodes(), 5);
    ASSERT_EQ(joinModel.getValue().getGraph().numEdges(), 8);
}

TEST_F(AggJoinModelGoldenTest, addEdgesFromExpr_predicatesAtEndNonScalar) {
    // Repeat test above, but don't mark $expr fields as edges- then, we shouldn't add them, since
    // these fields are non-scalar.
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
    markFieldsAsScalar(*pipeline,
                       {"s1"sv, "s2"sv, "s3"sv, "s4"sv},
                       {{"A", {"s1"sv}}, {"B", {"s2"sv}}, {"C", {"s3"sv}}, {"D", {"s4"sv}}});
    auto joinModel = runVariation(std::move(pipeline), "addEdgesFromExpr_predicatesAtEndNonScalar");
    ASSERT_OK(joinModel);
    // $match gets pushed up by optimization, then renders remaining suffix ineligible!
    ASSERT_EQ(joinModel.getValue().getGraph().numNodes(), 3);
    ASSERT_EQ(joinModel.getValue().getGraph().numEdges(), 2);
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
    markFieldsAsScalar(*pipeline,
                       {"s1"sv, "s2"sv, "s3"sv, "s4"sv},
                       {{"A", {"s1"sv, "a"sv, "d"sv}},
                        {"B", {"s2"sv, "a"sv, "b"sv}},
                        {"C", {"s3"sv, "b"sv, "c"sv}},
                        {"D", {"s4"sv, "c"sv, "d"sv}}});
    auto joinModel = runVariation(std::move(pipeline), "addEdgesFromExpr_predicatesInBetween");
    ASSERT_OK(joinModel);
    ASSERT_EQ(joinModel.getValue().getGraph().numNodes(), 5);
    ASSERT_EQ(joinModel.getValue().getGraph().numEdges(), 8);
}

TEST_F(AggJoinModelGoldenTest, addEdgesFromExpr_predicatesInBetweenNonScalar) {
    // Same as above, but missing path arrayness.
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
    markFieldsAsScalar(*pipeline,
                       {"s1"sv, "s2"sv, "s3"sv, "s4"sv},
                       {{"A", {"s1"sv}}, {"B", {"s2"sv}}, {"C", {"s3"sv}}, {"D", {"s4"sv}}});
    auto joinModel =
        runVariation(std::move(pipeline), "addEdgesFromExpr_predicatesInBetweenNonScalar");
    ASSERT_OK(joinModel);
    // $match moves up, disqualifying 2 nodes.
    ASSERT_EQ(joinModel.getValue().getGraph().numNodes(), 3);
    ASSERT_EQ(joinModel.getValue().getGraph().numEdges(), 2);
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
    markFieldsAsScalar(
        *pipeline,
        {"s1"sv, "s2"sv, "s3"sv, "s4"sv},
        {{"A", {"s1"sv, "a"sv}}, {"B", {"s2"sv, "a"sv, "b"sv}}, {"C", {"s3"sv}}, {"D", {"s4"sv}}});
    auto joinModel = runVariation(std::move(pipeline), "addEdgesFromExpr_earlyEnd");
    ASSERT_OK(joinModel);
    ASSERT_EQ(joinModel.getValue().getGraph().numNodes(), 3);
    ASSERT_EQ(joinModel.getValue().getGraph().numEdges(), 3);
}

TEST_F(AggJoinModelGoldenTest, addEdgesFromExpr_earlyEndNumeric) {
    // Same as first, but with a numeric field path instead of a self-edge.
    const auto query = R"([
            {$lookup: {from: "A", localField: "s1", foreignField: "s1", as: "fromA"}},
            {$unwind: "$fromA"},
            {$lookup: {from: "B", localField: "s2", foreignField: "s2", as: "fromB"}},
            {$unwind: "$fromB"},
            {$match: {$and: [
                {$expr: {$eq: ["$fromA.a", "$fromB.a"]}},
                {$expr: {$eq: ["$fromA.c", "$fromB.c.0"]}}
            ]}},
            {$lookup: {from: "C", localField: "s3", foreignField: "s3", as: "fromC"}},
            {$unwind: "$fromC"},
            {$lookup: {from: "D", localField: "s4", foreignField: "s4", as: "fromD"}},
            {$unwind: "$fromD"}
        ])";
    auto pipeline = makePipeline(query, {"A", "B", "C", "D"});
    markFieldsAsScalar(
        *pipeline,
        {"s1"sv, "s2"sv, "s3"sv, "s4"sv},
        {{"A", {"s1"sv, "a"sv, "c"sv}},
         {"B", {"s2"sv, "a"sv, "c.0"sv}},  // Field 'c.0' is scalar, but not permitted.
         {"C", {"s3"sv}},
         {"D", {"s4"sv}}});
    auto joinModel = runVariation(std::move(pipeline), "addEdgesFromExpr_earlyEndNumeric");
    ASSERT_OK(joinModel);
    ASSERT_EQ(joinModel.getValue().getGraph().numNodes(), 3);
    ASSERT_EQ(joinModel.getValue().getGraph().numEdges(), 3);
}

TEST_F(AggJoinModelGoldenTest, addEdgesFromExpr_earlyEndNonScalar) {
    // Same as first, but without path arrayness.
    const auto query = R"([
            {$lookup: {from: "A", localField: "s1", foreignField: "s1", as: "fromA"}},
            {$unwind: "$fromA"},
            {$lookup: {from: "B", localField: "s2", foreignField: "s2", as: "fromB"}},
            {$unwind: "$fromB"},
            {$match: {$and: [
                {$expr: {$eq: ["$fromA.a", "$fromB.a"]}},
                {$expr: {$eq: ["$fromB.b", "$fromA.b"]}}
                ]}
            },
            {$lookup: {from: "C", localField: "s3", foreignField: "s3", as: "fromC"}},
            {$unwind: "$fromC"},
            {$lookup: {from: "D", localField: "s4", foreignField: "s4", as: "fromD"}},
            {$unwind: "$fromD"}
        ])";
    auto pipeline = makePipeline(query, {"A", "B", "C", "D"});
    markFieldsAsScalar(*pipeline,
                       {"s1"sv, "s2"sv, "s3"sv, "s4"sv},
                       {{"A", {"s1"sv, "a"sv}},  // No arrayness info for field 'b'.
                        {"B", {"s2"sv, "a"sv, "b"sv}},
                        {"C", {"s3"sv}},
                        {"D", {"s4"sv}}});
    auto joinModel = runVariation(std::move(pipeline), "addEdgesFromExpr_earlyEndNonScalar");
    ASSERT_OK(joinModel);
    ASSERT_EQ(joinModel.getValue().getGraph().numNodes(), 3);
    ASSERT_EQ(joinModel.getValue().getGraph().numEdges(), 3);
}
/**
 * Combined test of $expr and implicit edges.
 * Legend: '==' - local/foreignField edge; '--' - $expr edge.
 * Connected Component 1:  base.a == A.a -- D.a. 1 implicit predicate is expected.
 * Connected Component 2: A.b == B.b -- C.c == D.d. 2 Implicit predicates are expected.
 * Standalone predicate: B.s == C.s
 * Total: 5 nodes; 5 local/foreignFields predicates + 2 $expr predicates + 3 implicit predicates =
 * 10 total predicates.
 */
TEST_F(AggJoinModelGoldenTest, addEdgesFromExpr_addImplicitEdge) {
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
    markFieldsAsScalar(*pipeline,
                       {"a"sv},
                       {{"A", {"a"sv, "b"sv}},
                        {"B", {"b"sv, "s"sv}},
                        {"C", {"s"sv, "c"sv}},
                        {"D", {"a"sv, "d"sv}}});
    auto joinModel = runVariation(std::move(pipeline), "addEdgesFromExpr_addImplicitEdge");
    ASSERT_OK(joinModel);
    ASSERT_EQ(joinModel.getValue().getGraph().numNodes(), 5);
    ASSERT_EQ(joinModel.getValue().getGraph().numEdges(), 8);
    ASSERT_EQ(numPredicates(joinModel.getValue().getGraph()), 10);
}

TEST_F(AggJoinModelGoldenTest, addEdgesFromExpr_addImplicitEdgeNonScalar) {
    // Same as above but without arrayness.
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
    markFieldsAsScalar(
        *pipeline,
        {"a"sv},
        {{"A", {"a"sv, "b"sv}}, {"B", {"b"sv, "s"sv}}, {"C", {"s"sv, "c"sv}}, {"D", {"d"sv}}});
    auto joinModel = runVariation(std::move(pipeline), "addEdgesFromExpr_addImplicitEdgeNonScalar");
    ASSERT_OK(joinModel);
    ASSERT_EQ(joinModel.getValue().getGraph().numNodes(), 5);
    // Can't add potentially multikey edge "A.a" - "D.a".
    ASSERT_EQ(joinModel.getValue().getGraph().numEdges(), 7);
    ASSERT_EQ(numPredicates(joinModel.getValue().getGraph()), 8);
}

/**
 * Combined test of $expr and implicit edges.
 * Legend: '==' - local/foreignField edge; '**' - subpipeline $expr edge.
 * Connected Component 1:  base.a == A.a ** D.a. 1 implicit predicate is expected.
 * Connected Component 2: A.b == B.b ** C.c == D.d. 2 Implicit predicates are expected.
 * Standalone predicate: B.s == C.s
 * Total: 5 nodes; 5 local/foreignFields predicates + 2 subpieline predicates + 3 implicit
 * predicates = 10 total predicates.
 */
TEST_F(AggJoinModelGoldenTest, subPipelineEdge_addImplicitEdge) {
    const auto query = R"([
            {$lookup: { from: "A", localField: "a", foreignField: "a", as: "fromA"} },
            {$unwind: "$fromA"},
            {$lookup: { from: "B", localField: "fromA.b", foreignField: "b", as: "fromB"} },
            {$unwind: "$fromB"},
            {$lookup: { from: "C",
                        localField: "fromB.s",
                        foreignField: "s",
                        as: "fromC",
                        let: {bb: "$fromB.b"},
                        pipeline: [ {$match: {$expr: {$eq: ["$$bb", "$c"]}}} ]
                      } 
            },
            {$unwind: "$fromC"},
            {$lookup: { from: "D",
                        localField: "fromC.c",
                        foreignField: "d",
                        as: "fromD",
                        let: {aa: "$fromA.a"},
                        pipeline: [ {$match: {$expr: {$eq: ["$a", "$$aa"]}}} ]
                      }
            },
            {$unwind: "$fromD"}
        ])";
    auto pipeline = makePipeline(query, {"A", "B", "C", "D"});
    markFieldsAsScalar(*pipeline,
                       {"a"sv},
                       {{"A", {"a"sv, "b"sv}},
                        {"B", {"b"sv, "s"sv}},
                        {"C", {"s"sv, "c"sv}},
                        {"D", {"d"sv, "a"sv}}});
    auto joinModel = runVariation(std::move(pipeline), "subPipelineEdge_addImplicitEdge");
    ASSERT_OK(joinModel);
    ASSERT_EQ(joinModel.getValue().getGraph().numNodes(), 5);
    ASSERT_EQ(joinModel.getValue().getGraph().numEdges(), 8);
    ASSERT_EQ(numPredicates(joinModel.getValue().getGraph()), 10);
}

/**
 * Combined test of $expr, subpipeline $expr, and implicit edges.
 * Legend: '==' - local/foreignField edge; '--' - $expr edge, '**' - subpipelines $expr.
 * Connected Component:  base.a == A.a -- B.a ** C.a. 3 implicit predicates are expected.
 * Standalone predicates: A.b == B.b, B.c == C.c
 * Total: 4 nodes; 3 local/foreignField predicates + 1 $expr predicate + 1 subpipeline predicate + 3
 * implicit predicates = 8 total predicates.
 */
TEST_F(AggJoinModelGoldenTest, addEdgesFromExpr_subPipelineEdge_addImplicitEdge) {
    const auto query = R"([
            {$lookup: { from: "A", localField: "a", foreignField: "a", as: "fromA"} },
            {$unwind: "$fromA"},
            {$lookup: { from: "B", localField: "fromA.b", foreignField: "b", as: "fromB"} },
            {$unwind: "$fromB"},
            {$lookup: { from: "C",
                        localField: "fromB.c",
                        foreignField: "c",
                        as: "fromC",
                        let: {ba: "$fromB.a"},
                        pipeline: [ {$match: {$expr: {$eq: ["$$ba", "$a"]}}} ]
                      }
            },
            {$unwind: "$fromC"},
            {$match: {$expr: {$eq: ["$fromA.a", "$fromB.a"]}}}
        ])";
    auto pipeline = makePipeline(query, {"A", "B", "C"});
    markFieldsAsScalar(
        *pipeline,
        {"a"sv},
        {{"A", {"a"sv, "b"sv}}, {"B", {"b"sv, "c"sv, "a"sv}}, {"C", {"c"sv, "a"sv}}});
    auto joinModel =
        runVariation(std::move(pipeline), "addEdgesFromExpr_subPipelineEdge_addImplicitEdge");
    ASSERT_OK(joinModel);
    ASSERT_EQ(joinModel.getValue().getGraph().numNodes(), 4);
    ASSERT_EQ(joinModel.getValue().getGraph().numEdges(), 6);
    ASSERT_EQ(numPredicates(joinModel.getValue().getGraph()), 8);
}

}  // namespace mongo::join_ordering
