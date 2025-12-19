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

    void runVariation(std::unique_ptr<Pipeline> pipeline,
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
    runVariation(std::move(pipeline), "longPrefix");
}

TEST_F(AggJoinModelGoldenTest, veryLargePipeline) {
    auto pipeline = makePipelineOfSize(/*numJoins*/ kHardMaxNodesInJoin + 3);
    runVariation(std::move(pipeline), "veryLargePipeline");
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
    runVariation(std::move(pipeline), "addImplicitEdges_OneImplictEdge");
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
    runVariation(std::move(pipeline), "addImplicitEdges_MultipleImplictEdges");
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
    runVariation(std::move(pipeline), "addImplicitEdges_TwoConnectedComponents");
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
    runVariation(std::move(pipeline), "addImplicitEdges_NoImplicitEdges");
}
}  // namespace mongo::join_ordering
