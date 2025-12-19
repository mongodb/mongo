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
#include "mongo/unittest/golden_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo::join_ordering {
namespace {
unittest::GoldenTestConfig goldenTestConfig{"src/mongo/db/test_output/query/join/agg_join_model"};

using PipelineAnalyzerTest = AggJoinModelFixture;

TEST_F(PipelineAnalyzerTest, PipelineIneligibleForJoinReorderingNoLocalForeignFields) {
    unittest::GoldenTestContext goldenCtx(&goldenTestConfig);
    const auto query = R"([
            {$lookup: {from: "B", as: "fromB", pipeline: [{$match: {a: 1}}]}},
            {$unwind: "$fromB"}
        ])";

    auto pipeline = makePipeline(query, {"A", "B"});

    // This pipeline is not eligible for join reordering due to a sub-pipeline.
    ASSERT_FALSE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));
}

TEST_F(PipelineAnalyzerTest, PipelinePrefixEligibleForJoinReorderingNoLocalForeignFields) {
    unittest::GoldenTestContext goldenCtx(&goldenTestConfig);
    const auto query = R"([
            {$lookup: {from: "A", localField: "a", foreignField: "b", as: "fromA"}},
            {$unwind: "$fromA"},
            {$lookup: {from: "B", as: "fromB", pipeline: [{$match: {a: 1}}]}},
            {$unwind: "$fromB"}
        ])";

    auto pipeline = makePipeline(query, {"A", "B"});

    // This pipeline's prefix is eligible for reordering.
    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);

    auto& joinModel = swJoinModel.getValue();
    goldenCtx.outStream() << joinModel.toString(true) << std::endl;
}

TEST_F(PipelineAnalyzerTest, PipelineEligibleForJoinReorderingSingleLookupUnwind) {
    unittest::GoldenTestContext goldenCtx(&goldenTestConfig);
    const auto query = R"([
            {$lookup: {from: "A", localField: "a", foreignField: "b", as: "fromA"}},
            {$unwind: "$fromA"}
        ])";

    auto pipeline = makePipeline(query, {"A"});
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

    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);

    const auto& joinModel = swJoinModel.getValue();
    ASSERT_EQ(joinModel.graph.numNodes(), 3);
    const auto* cq = joinModel.graph.accessPathAt((NodeId)1);
    ASSERT_EQ(cq->nss().coll(), "A");
    ASSERT_EQ("{ d: { $eq: 11 } }", cq->getPrimaryMatchExpression()->toString());
    goldenCtx.outStream() << joinModel.toString(true) << std::endl;
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

    // We don't detect ineligibility here.
    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    // But we fail to construct a model here, because $group isn't pushed into SBE.
    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_EQ(swJoinModel.getStatus(), ErrorCodes::QueryFeatureNotAllowed);
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
                       pipeline: [{$lookup: {from: "B", localField: "b", foreignField: "b", as: "innerB"}}]}
            },
            {$unwind: "$fromA"},
            {$lookup: {from: "B", localField: "a", foreignField: "b", as: "fromB"}},
            {$unwind: "$fromB"}
        ])";

    auto pipeline = makePipeline(query, {"A", "B"});

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
            {$sort: {e: 1}},
            {$project: {k: 0}},
            {$lookup: {from: "A", localField: "a", foreignField: "b", as: "fromA"}},
            {$unwind: "$fromA"},
            {$lookup: {from: "B", localField: "a", foreignField: "b", as: "fromB"}},
            {$unwind: "$fromB"}
        ])";

    auto pipeline = makePipeline(query, {"A", "B"});

    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);
    auto& joinModel = swJoinModel.getValue();
    goldenCtx.outStream() << joinModel.toString(true) << std::endl;
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

    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);
    auto& joinModel = swJoinModel.getValue();
    goldenCtx.outStream() << joinModel.toString(true) << std::endl;
}

TEST_F(PipelineAnalyzerTest, tooManyNodes) {
    static constexpr size_t numJoins = 5;
    auto pipeline = makePipelineOfSize(numJoins);
    // Configure the buildParams that one $lookup/$unwind pair is forced to the suffix because the
    // maximum number of nodes is hit.
    AggModelBuildParams buildParams{
        .joinGraphBuildParams =
            JoinGraphBuildParams(/*maxNodes*/ numJoins, /*maxEdges*/ kHardMaxEdgesInJoin),
        .maxNumberNodesConsideredForImplicitEdges = kMaxNumberNodesConsideredForImplicitEdges};
    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, buildParams);
    ASSERT_OK(swJoinModel);
    // One $lookup with absorbed $unwind was left unoptimized.
    ASSERT_EQ(swJoinModel.getValue().suffix->getSources().size(), 1);
}

TEST_F(PipelineAnalyzerTest, tooManyEdges) {
    static constexpr size_t numJoins = 5;
    auto pipeline = makePipelineOfSize(numJoins);
    // Configure the buildParams that one $lookup/$unwind pair is forced to the suffix because the
    // maximum number of edges is hit.
    AggModelBuildParams buildParams{
        .joinGraphBuildParams =
            JoinGraphBuildParams(/*maxNodes*/ kHardMaxNodesInJoin, /*maxEdges*/ numJoins - 1),
        .maxNumberNodesConsideredForImplicitEdges = kMaxNumberNodesConsideredForImplicitEdges};
    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, buildParams);
    ASSERT_OK(swJoinModel);
    // One $lookup with absorbed $unwind was left unoptimized.
    ASSERT_EQ(swJoinModel.getValue().suffix->getSources().size(), 1);
}

}  // namespace
}  // namespace mongo::join_ordering
