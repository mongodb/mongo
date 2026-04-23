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

TEST_F(PipelineAnalyzerTest, ConflictingLocalFields) {
    // Conflicting prefix in the local field

    auto query = R"([
        {$lookup: {from: "B", as: "a.b", localField: "x", foreignField: "y"}},
        {$unwind: "$a.b"},
        {$lookup: {from: "C", as: "c", localField: "a", foreignField: "z"}},
        {$unwind: "$c"}
    ])";

    auto pipeline = makePipeline(query, {"B", "C"});
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

    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_NOT_OK(swJoinModel);
}

}  // namespace
}  // namespace mongo::join_ordering
