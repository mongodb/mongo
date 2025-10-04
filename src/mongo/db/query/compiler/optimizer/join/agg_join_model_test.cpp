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

#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/unittest/unittest.h"

namespace mongo::join_ordering {
std::vector<BSONObj> pipelineFromJsonArray(StringData jsonArray) {
    auto inputBson = fromjson("{pipeline: " + jsonArray + "}");
    ASSERT_EQUALS(inputBson["pipeline"].type(), BSONType::array);
    std::vector<BSONObj> rawPipeline;
    for (auto&& stageElem : inputBson["pipeline"].Array()) {
        ASSERT_EQUALS(stageElem.type(), BSONType::object);
        rawPipeline.push_back(stageElem.embeddedObject().getOwned());
    }
    return rawPipeline;
}

class PipelineAnalyzerTest : public AggregationContextFixture {
protected:
    auto makePipeline(StringData query, std::vector<StringData> collNames) {
        stdx::unordered_set<NamespaceString> secondaryNamespaces;
        for (auto&& collName : collNames) {
            secondaryNamespaces.insert(
                NamespaceString::createNamespaceString_forTest("test", collName));
        }
        auto expCtx = getExpCtx();
        expCtx->addResolvedNamespaces(secondaryNamespaces);

        const auto bsonStages = pipelineFromJsonArray(query);
        auto pipeline = Pipeline::parse(bsonStages, expCtx);
        pipeline->optimizePipeline();

        return pipeline;
    }
};

TEST_F(PipelineAnalyzerTest, CanOptimizeWithJoinReordering_noLocalForeignFeilds) {
    const auto query = R"([
            {$lookup: {from: "A", localField: "a", foreignField: "b", as: "fromA"}},
            {$unwind: "$fromA"},
            {$lookup: {from: "B", as: "fromB", pipeline: [{$match: {a: 1}}]}},
            {$unwind: "$fromB"}
        ])";

    auto pipeline = makePipeline(query, {"A", "B"});

    ASSERT_TRUE(AggJoinModel::canOptimizeWithJoinReordering(pipeline));
}

TEST_F(PipelineAnalyzerTest, CanOptimizeWithJoinReordering_singleLookupUnwind) {
    const auto query = R"([
            {$lookup: {from: "A", localField: "a", foreignField: "b", as: "fromA"}},
            {$unwind: "$fromA"}
        ])";

    auto pipeline = makePipeline(query, {"A"});
    ASSERT_TRUE(AggJoinModel::canOptimizeWithJoinReordering(pipeline));
}

TEST_F(PipelineAnalyzerTest, CanOptimizeWithJoinReordering_noUnwind) {
    const auto query = R"([
            {$lookup: {from: "A", localField: "a", foreignField: "b", as: "fromA"}},
            {$unwind: "$fromA"},
            {$lookup: {from: "B", localField: "a", foreignField: "b", as: "fromB"}}
        ])";

    auto pipeline = makePipeline(query, {"A", "B"});

    ASSERT_TRUE(AggJoinModel::canOptimizeWithJoinReordering(pipeline));
}

TEST_F(PipelineAnalyzerTest, CanOptimizeWithJoinReordering_nonAbsorbableUnwind) {
    const auto query = R"([
            {$lookup: {from: "A", localField: "a", foreignField: "b", as: "fromA"}},
            {$unwind: "$fromA"},
            {$lookup: {from: "B", localField: "a", foreignField: "b", as: "fromB"}},
            {$unwind: "$hello"}
        ])";

    auto pipeline = makePipeline(query, {"A", "B"});

    ASSERT_TRUE(AggJoinModel::canOptimizeWithJoinReordering(pipeline));
}

TEST_F(PipelineAnalyzerTest, TwoLookupUnwinds) {
    const auto query = R"([
            {$lookup: {from: "A", localField: "a", foreignField: "b", as: "fromA"}},
            {$unwind: "$fromA"},
            {$lookup: {from: "B", localField: "a", foreignField: "b", as: "fromB"}},
            {$unwind: "$fromB"}
        ])";

    auto pipeline = makePipeline(query, {"A", "B"});

    ASSERT_FALSE(AggJoinModel::canOptimizeWithJoinReordering(pipeline));

    AggJoinModel joinModel{std::move(pipeline)};
    ASSERT_EQ(joinModel.graph.numNodes(), 3);
    ASSERT_EQ(joinModel.graph.numEdges(), 2);
    ASSERT_EQ(joinModel.resolvedPaths.size(), 3);
}

TEST_F(PipelineAnalyzerTest, MatchOnMainCollection) {
    const auto query = R"([
            {$match: {c: 1}},
            {$lookup: {from: "A", localField: "a", foreignField: "b", as: "fromA"}},
            {$unwind: "$fromA"},
            {$lookup: {from: "B", localField: "a", foreignField: "b", as: "fromB"}},
            {$unwind: "$fromB"}
        ])";

    auto pipeline = makePipeline(query, {"A", "B"});

    ASSERT_FALSE(AggJoinModel::canOptimizeWithJoinReordering(pipeline));

    AggJoinModel joinModel{std::move(pipeline)};
    ASSERT_EQ(joinModel.graph.numNodes(), 3);
    ASSERT_EQ(joinModel.graph.numEdges(), 2);
    ASSERT_EQ(joinModel.resolvedPaths.size(), 3);
    ASSERT_EQ(joinModel.graph.getNode(0).accessPath->getPrimaryMatchExpression()->debugString(),
              "c $eq 1\n");
}

TEST_F(PipelineAnalyzerTest, MatchInSubPipeline) {
    const auto query = R"([
            {$lookup: {from: "A", localField: "a", foreignField: "b", as: "fromA",
                       pipeline: [{$match: {d: 11}}] }
            },
            {$unwind: "$fromA"},
            {$lookup: {from: "B", localField: "a", foreignField: "b", as: "fromB"}},
            {$unwind: "$fromB"}
        ])";

    auto pipeline = makePipeline(query, {"A", "B"});

    ASSERT_FALSE(AggJoinModel::canOptimizeWithJoinReordering(pipeline));

    AggJoinModel joinModel{std::move(pipeline)};
    ASSERT_EQ(joinModel.graph.numNodes(), 3);
    ASSERT_EQ(joinModel.graph.numEdges(), 2);
    ASSERT_EQ(joinModel.resolvedPaths.size(), 3);
    ASSERT_EQ(joinModel.graph.getNode(1).accessPath->getPrimaryMatchExpression()->debugString(),
              "d $eq 11\n");
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

    ASSERT_FALSE(AggJoinModel::canOptimizeWithJoinReordering(pipeline));

    AggJoinModel joinModel{std::move(pipeline)};
    ASSERT_EQ(joinModel.graph.numNodes(), 1);
    ASSERT_EQ(joinModel.graph.numEdges(), 0);
    ASSERT_EQ(joinModel.resolvedPaths.size(), 0);
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

    ASSERT_FALSE(AggJoinModel::canOptimizeWithJoinReordering(pipeline));

    AggJoinModel joinModel{std::move(pipeline)};
    ASSERT_EQ(joinModel.graph.numNodes(), 1);
    ASSERT_EQ(joinModel.graph.numEdges(), 0);
    ASSERT_EQ(joinModel.resolvedPaths.size(), 0);
}

TEST_F(PipelineAnalyzerTest, LongPrefix) {
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

    ASSERT_FALSE(AggJoinModel::canOptimizeWithJoinReordering(pipeline));

    AggJoinModel joinModel{std::move(pipeline)};
    ASSERT_EQ(joinModel.graph.numNodes(), 3);
    ASSERT_EQ(joinModel.graph.numEdges(), 2);
    ASSERT_EQ(joinModel.resolvedPaths.size(), 3);
    const auto& baseCQ = joinModel.graph.getNode(0).accessPath;
    ASSERT_EQ(baseCQ->getPrimaryMatchExpression()->debugString(), "c $eq 1\n");
    ASSERT_TRUE(baseCQ->getSortPattern().has_value());
    ASSERT_EQ(baseCQ->getSortPattern()->front().fieldPath->fullPath(), std::string("e"));
    ASSERT_NE(baseCQ->getProj(), nullptr);
    ASSERT_TRUE(baseCQ->getProj()->isExclusionOnly());
}
}  // namespace mongo::join_ordering
