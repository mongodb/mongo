/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
namespace {
unittest::GoldenTestConfig goldenTestConfig{"src/mongo/db/test_output/query/join/agg_join_model"};
using RenamesTest = AggJoinModelFixture;
}  // namespace

TEST_F(RenamesTest, RenamePrefixLocalForeign) {
    unittest::GoldenTestContext goldenCtx(&goldenTestConfig);
    const auto query = R"([
            {$project: {renamedField: "$a"}},
            {$lookup: {from: "A", localField: "renamedField", foreignField: "b", as: "fromA"}},
            {$unwind: "$fromA"}
        ])";

    auto pipeline = makePipeline(query, {"A"});
    markFieldsAsScalar(*pipeline, {"a"}, {{"A", {"b"}}});
    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);

    auto& joinModel = swJoinModel.getValue();
    const auto& resolvedPaths = joinModel.getResolvedPaths();
    ASSERT_EQ(resolvedPaths.size(), 2);

    // Path "a" is renamed to "renamedField".
    ASSERT_EQ(resolvedPaths[0].nodeId, 0);
    ASSERT_EQ(resolvedPaths[0].underlyingFieldPath.fullPath(), "a");
    ASSERT(resolvedPaths[0].fieldPathAfterRenames.has_value());
    ASSERT_EQ(resolvedPaths[0].fieldPathAfterRenames->fullPath(), "renamedField");

    // Path "b" is simple, no rename.
    ASSERT_EQ(resolvedPaths[1].nodeId, 1);
    ASSERT_EQ(resolvedPaths[1].underlyingFieldPath.fullPath(), "b");
    ASSERT_FALSE(resolvedPaths[1].fieldPathAfterRenames);

    goldenCtx.outStream() << joinModel.toString(true) << std::endl;
}

TEST_F(RenamesTest, RenamePrefixMultiLocalForeignProjectDups) {
    unittest::GoldenTestContext goldenCtx(&goldenTestConfig);
    const auto query = R"([
            {$project: {renamedField: "$a", alsoRenamedField: "$a"}},
            {$lookup: {from: "A", localField: "alsoRenamedField", foreignField: "b", as: "fromA"}},
            {$unwind: "$fromA"}
        ])";

    auto pipeline = makePipeline(query, {"A"});
    markFieldsAsScalar(*pipeline, {"a"}, {{"A", {"b"}}});
    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);

    auto& joinModel = swJoinModel.getValue();
    const auto& resolvedPaths = joinModel.getResolvedPaths();
    ASSERT_EQ(resolvedPaths.size(), 2);

    // Path "a" is renamed to "alsoRenamedField".
    ASSERT_EQ(resolvedPaths[0].nodeId, 0);
    ASSERT_EQ(resolvedPaths[0].underlyingFieldPath.fullPath(), "a");
    ASSERT(resolvedPaths[0].fieldPathAfterRenames.has_value());
    ASSERT_EQ(resolvedPaths[0].fieldPathAfterRenames->fullPath(), "alsoRenamedField");

    // Path "b" is simple, no rename.
    ASSERT_EQ(resolvedPaths[1].nodeId, 1);
    ASSERT_EQ(resolvedPaths[1].underlyingFieldPath.fullPath(), "b");
    ASSERT_FALSE(resolvedPaths[1].fieldPathAfterRenames);

    goldenCtx.outStream() << joinModel.toString(true) << std::endl;
}

TEST_F(RenamesTest, RenamePrefixMatchExpr) {
    unittest::GoldenTestContext goldenCtx(&goldenTestConfig);
    const auto query = R"([
            {$project: {renamedField: "$a"}},
            {$lookup: {from: "A", let: {rf: "$renamedField"}, as: "fromA", pipeline: [
                {$match: {$expr: {$eq: ["$$rf", "$b"]}}}
            ]}},
            {$unwind: "$fromA"}
        ])";

    auto pipeline = makePipeline(query, {"A"});
    markFieldsAsScalar(*pipeline, {"a"}, {{"A", {"b"}}});
    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);

    auto& joinModel = swJoinModel.getValue();
    const auto& resolvedPaths = joinModel.getResolvedPaths();
    ASSERT_EQ(resolvedPaths.size(), 2);

    // Path "a" is renamed to "renamedField".
    ASSERT_EQ(resolvedPaths[0].nodeId, 0);
    ASSERT_EQ(resolvedPaths[0].underlyingFieldPath.fullPath(), "a");
    ASSERT(resolvedPaths[0].fieldPathAfterRenames.has_value());
    ASSERT_EQ(resolvedPaths[0].fieldPathAfterRenames->fullPath(), "renamedField");

    // Path "b" is simple, no rename.
    ASSERT_EQ(resolvedPaths[1].nodeId, 1);
    ASSERT_EQ(resolvedPaths[1].underlyingFieldPath.fullPath(), "b");
    ASSERT_FALSE(resolvedPaths[1].fieldPathAfterRenames);

    goldenCtx.outStream() << joinModel.toString(true) << std::endl;
}

TEST_F(RenamesTest, RenameSubpipelineLocalForeign) {
    unittest::GoldenTestContext goldenCtx(&goldenTestConfig);
    const auto query = R"([
            {$lookup: {from: "A", localField: "a", foreignField: "b", as: "fromA", pipeline: [
                {$project: {renamedField: "$b"}}
            ]}},
            {$unwind: "$fromA"}
        ])";

    auto pipeline = makePipeline(query, {"A"});
    markFieldsAsScalar(*pipeline, {"a"}, {{"A", {"b"}}});
    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);

    const auto& joinModel = swJoinModel.getValue();
    const auto& resolvedPaths = joinModel.getResolvedPaths();
    ASSERT_EQ(resolvedPaths.size(), 2);

    // Path "a" is straightforward. Same before/after CQ.
    ASSERT_EQ(resolvedPaths[0].nodeId, 0);
    ASSERT_EQ(resolvedPaths[0].underlyingFieldPath.fullPath(), "a");
    ASSERT_FALSE(resolvedPaths[0].fieldPathAfterRenames.has_value());

    // Path "b" is renamed to renamedField, and we track that.
    ASSERT_EQ(resolvedPaths[1].nodeId, 1);
    ASSERT_EQ(resolvedPaths[1].underlyingFieldPath.fullPath(), "b");
    ASSERT(resolvedPaths[1].fieldPathAfterRenames.has_value());
    ASSERT_EQ(resolvedPaths[1].fieldPathAfterRenames->fullPath(), "renamedField");

    goldenCtx.outStream() << joinModel.toString(true) << std::endl;
}

TEST_F(RenamesTest, RenameSubpipelineMatchExpr) {
    unittest::GoldenTestContext goldenCtx(&goldenTestConfig);
    const auto query = R"([
            {$lookup: {from: "A", let: {lf: "$a"}, as: "fromA", pipeline: [
                {$match: {$expr: {$eq: ["$$lf", "$b"]}}},
                {$project: {renamedField: "$b"}}
            ]}},
            {$unwind: "$fromA"}
        ])";

    auto pipeline = makePipeline(query, {"A"});
    markFieldsAsScalar(*pipeline, {"a"}, {{"A", {"b"}}});
    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);

    const auto& joinModel = swJoinModel.getValue();
    const auto& resolvedPaths = joinModel.getResolvedPaths();
    ASSERT_EQ(resolvedPaths.size(), 2);

    // Path "a" is straightforward. Same before/after CQ.
    ASSERT_EQ(resolvedPaths[0].nodeId, 0);
    ASSERT_EQ(resolvedPaths[0].underlyingFieldPath.fullPath(), "a");
    ASSERT_FALSE(resolvedPaths[0].fieldPathAfterRenames.has_value());

    // Path "b" is renamed to renamedField, and we track that.
    ASSERT_EQ(resolvedPaths[1].nodeId, 1);
    ASSERT_EQ(resolvedPaths[1].underlyingFieldPath.fullPath(), "b");
    ASSERT(resolvedPaths[1].fieldPathAfterRenames.has_value());
    ASSERT_EQ(resolvedPaths[1].fieldPathAfterRenames->fullPath(), "renamedField");

    goldenCtx.outStream() << joinModel.toString(true) << std::endl;
}

TEST_F(RenamesTest, RenameSubpipelineMatchExprProjectDups) {
    unittest::GoldenTestContext goldenCtx(&goldenTestConfig);
    const auto query = R"([
            {$lookup: {from: "A", let: {lf: "$a"}, as: "fromA", pipeline: [
                {$match: {$expr: {$eq: ["$$lf", "$b"]}}},
                {$project: {renamedField: "$b", renamedAlso: "$b", renameRename: "$b"}}
            ]}},
            {$unwind: "$fromA"}
        ])";

    auto pipeline = makePipeline(query, {"A"});
    markFieldsAsScalar(*pipeline, {"a"}, {{"A", {"b"}}});
    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);

    const auto& joinModel = swJoinModel.getValue();
    const auto& resolvedPaths = joinModel.getResolvedPaths();
    ASSERT_EQ(resolvedPaths.size(), 2);

    // Path "a" is straightforward. Same before/after CQ.
    ASSERT_EQ(resolvedPaths[0].nodeId, 0);
    ASSERT_EQ(resolvedPaths[0].underlyingFieldPath.fullPath(), "a");
    ASSERT_FALSE(resolvedPaths[0].fieldPathAfterRenames.has_value());

    // Path "b" is renamed twice, but we pick 'renameRename', and we track that.
    ASSERT_EQ(resolvedPaths[1].nodeId, 1);
    ASSERT_EQ(resolvedPaths[1].underlyingFieldPath.fullPath(), "b");
    ASSERT(resolvedPaths[1].fieldPathAfterRenames.has_value());
    ASSERT_EQ(resolvedPaths[1].fieldPathAfterRenames->fullPath(), "renameRename");

    goldenCtx.outStream() << joinModel.toString(true) << std::endl;
}

TEST_F(RenamesTest, RenameSubpipelineMatchExprSwapped) {
    // TODO SERVER-130580: we should be able to produce a join graph for this pipeline also- it
    // should produce an equivalent plan to the above.
    const auto query = R"([
            {$lookup: {from: "A", let: {lf: "$a"}, as: "fromA", pipeline: [
                {$project: {renamedField: "$b"}},
                {$match: {$expr: {$eq: ["$$lf", "$b"]}}}
            ]}},
            {$unwind: "$fromA"}
        ])";

    auto pipeline = makePipeline(query, {"A"});
    markFieldsAsScalar(*pipeline, {"a"}, {{"A", {"b"}}});
    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_NOT_OK(swJoinModel);
}

TEST_F(RenamesTest, TrailingMatchAfterRename) {
    // TODO SERVER-130580: we should be able to produce a join graph for this pipeline also- it
    // should produce an equivalent plan to the above.
    const auto query = R"([
            {$lookup: {from: "A", as: "fromA", pipeline: []}},
            {$unwind: "$fromA"},
            {$project: {renamedField: "$fromA.b"}},
            {$match: {$expr: {$eq: ["$a", "$renamedField"]}}}
        ])";

    auto pipeline = makePipeline(query, {"A"});
    markFieldsAsScalar(*pipeline, {"a"}, {{"A", {"b"}}});
    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_NOT_OK(swJoinModel);
}

TEST_F(RenamesTest, RenamePrefixTrailingMatch) {
    unittest::GoldenTestContext goldenCtx(&goldenTestConfig);
    const auto query = R"([
            {$project: {renamedField: "$a"}},
            {$lookup: {from: "A", as: "fromA", pipeline: []}},
            {$unwind: "$fromA"},
            {$match: {$expr: {$eq: ["$renamedField", "$fromA.b"]}}}
        ])";

    auto pipeline = makePipeline(query, {"A"});
    markFieldsAsScalar(*pipeline, {"a"}, {{"A", {"b"}}});
    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);

    auto& joinModel = swJoinModel.getValue();
    const auto& resolvedPaths = joinModel.getResolvedPaths();
    ASSERT_EQ(resolvedPaths.size(), 2);

    // Path "a" is renamed to "renamedField".
    ASSERT_EQ(resolvedPaths[0].nodeId, 0);
    ASSERT_EQ(resolvedPaths[0].underlyingFieldPath.fullPath(), "a");
    ASSERT(resolvedPaths[0].fieldPathAfterRenames.has_value());
    ASSERT_EQ(resolvedPaths[0].fieldPathAfterRenames->fullPath(), "renamedField");

    // Path "b" is simple, no rename.
    ASSERT_EQ(resolvedPaths[1].nodeId, 1);
    ASSERT_EQ(resolvedPaths[1].underlyingFieldPath.fullPath(), "b");
    ASSERT_FALSE(resolvedPaths[1].fieldPathAfterRenames);

    goldenCtx.outStream() << joinModel.toString(true) << std::endl;
}

TEST_F(RenamesTest, RenameSubpipelineTrailingMatch) {
    unittest::GoldenTestContext goldenCtx(&goldenTestConfig);
    const auto query = R"([
            {$lookup: {from: "A", as: "fromA", pipeline: [
                {$project: {renamedField: "$b"}}
            ]}},
            {$unwind: "$fromA"},
            {$match: {$expr: {$eq: ["$a", "$fromA.renamedField"]}}}
        ])";

    auto pipeline = makePipeline(query, {"A"});
    markFieldsAsScalar(*pipeline, {"a"}, {{"A", {"b"}}});
    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);

    const auto& joinModel = swJoinModel.getValue();
    const auto& resolvedPaths = joinModel.getResolvedPaths();
    ASSERT_EQ(resolvedPaths.size(), 2);

    // Path "a" is straightforward. Same before/after CQ.
    ASSERT_EQ(resolvedPaths[0].nodeId, 0);
    ASSERT_EQ(resolvedPaths[0].underlyingFieldPath.fullPath(), "a");
    ASSERT_FALSE(resolvedPaths[0].fieldPathAfterRenames.has_value());

    // Path "b" is renamed to renamedField, and we track that.
    ASSERT_EQ(resolvedPaths[1].nodeId, 1);
    ASSERT_EQ(resolvedPaths[1].underlyingFieldPath.fullPath(), "b");
    ASSERT(resolvedPaths[1].fieldPathAfterRenames.has_value());
    ASSERT_EQ(resolvedPaths[1].fieldPathAfterRenames->fullPath(), "renamedField");

    goldenCtx.outStream() << joinModel.toString(true) << std::endl;
}

TEST_F(RenamesTest, RenameSubpipelineTrailingMatchProjectDups) {
    unittest::GoldenTestContext goldenCtx(&goldenTestConfig);
    const auto query = R"([
            {$lookup: {from: "A", as: "fromA", pipeline: [
                {$project: {renamedField: "$b", renamedAlso: "$b", "xRename": "$b", "yRename": "$b"}}
            ]}},
            {$unwind: "$fromA"},
            {$match: {$expr: {$eq: ["$a", "$fromA.renamedField"]}}}
        ])";

    auto pipeline = makePipeline(query, {"A"});
    markFieldsAsScalar(*pipeline, {"a"}, {{"A", {"b"}}});
    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);

    const auto& joinModel = swJoinModel.getValue();
    const auto& resolvedPaths = joinModel.getResolvedPaths();
    ASSERT_EQ(resolvedPaths.size(), 2);

    // Path "a" is straightforward. Same before/after CQ.
    ASSERT_EQ(resolvedPaths[0].nodeId, 0);
    ASSERT_EQ(resolvedPaths[0].underlyingFieldPath.fullPath(), "a");
    ASSERT_FALSE(resolvedPaths[0].fieldPathAfterRenames.has_value());

    // Path "b" is renamed to renamedField, and we track that.
    ASSERT_EQ(resolvedPaths[1].nodeId, 1);
    ASSERT_EQ(resolvedPaths[1].underlyingFieldPath.fullPath(), "b");
    ASSERT(resolvedPaths[1].fieldPathAfterRenames.has_value());
    ASSERT_EQ(resolvedPaths[1].fieldPathAfterRenames->fullPath(), "renamedField");

    goldenCtx.outStream() << joinModel.toString(true) << std::endl;
}

TEST_F(RenamesTest, RenameAllTypesCycle) {
    unittest::GoldenTestContext goldenCtx(&goldenTestConfig);
    // Cycle is formed due to predicates BASE.a --- A.b --- B.c --- C.d
    // Fields BASE.a, B.c, and C.d are renamed. Suffix $project has no effect.
    const auto query = R"([
            {$project: {renamedField: "$a"}},
            {$lookup: {from: "A", localField: "renamedField", foreignField: "b", as: "fromA"}},
            {$unwind: "$fromA"},
            {$lookup: {from: "B", localField: "fromA.b", foreignField: "c", as: "fromB", pipeline: [
                {$project: {cRenamed: "$c"}}
            ]}},
            {$unwind: "$fromB"},
            {$lookup: {from: "C", as: "fromC", pipeline: [
                {$project: {dRenamed: "$d"}}
            ]}},
            {$unwind: "$fromC"},
            {$match: {$expr: {$eq: ["$fromC.dRenamed", "$fromB.cRenamed"]}}},
            {$project: {renameAgain: "$fromC.dRenamed"}}
        ])";

    auto pipeline = makePipeline(query, {"A", "B", "C", "D"});
    markFieldsAsScalar(*pipeline, {"a"}, {{"A", {"b"}}, {"B", {"c"}}, {"C", {"d"}}});
    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);

    auto& joinModel = swJoinModel.getValue();
    const auto& resolvedPaths = joinModel.getResolvedPaths();
    ASSERT_EQ(resolvedPaths.size(), 4);

    // Path BASE.a is renamed to "renamedField".
    ASSERT_EQ(resolvedPaths[0].nodeId, 0);
    ASSERT_EQ(resolvedPaths[0].underlyingFieldPath.fullPath(), "a");
    ASSERT(resolvedPaths[0].fieldPathAfterRenames.has_value());
    ASSERT_EQ(resolvedPaths[0].fieldPathAfterRenames->fullPath(), "renamedField");

    // Path A.b is simple, no rename.
    ASSERT_EQ(resolvedPaths[1].nodeId, 1);
    ASSERT_EQ(resolvedPaths[1].underlyingFieldPath.fullPath(), "b");
    ASSERT_FALSE(resolvedPaths[1].fieldPathAfterRenames);

    // Path B.c is renamed to "cRenamed".
    ASSERT_EQ(resolvedPaths[2].nodeId, 2);
    ASSERT_EQ(resolvedPaths[2].underlyingFieldPath.fullPath(), "c");
    ASSERT(resolvedPaths[2].fieldPathAfterRenames.has_value());
    ASSERT_EQ(resolvedPaths[2].fieldPathAfterRenames->fullPath(), "cRenamed");

    // Path C.d is renamed to "dRenamed".
    ASSERT_EQ(resolvedPaths[3].nodeId, 3);
    ASSERT_EQ(resolvedPaths[3].underlyingFieldPath.fullPath(), "d");
    ASSERT(resolvedPaths[3].fieldPathAfterRenames.has_value());
    ASSERT_EQ(resolvedPaths[3].fieldPathAfterRenames->fullPath(), "dRenamed");

    goldenCtx.outStream() << joinModel.toString(true) << std::endl;
}

TEST_F(RenamesTest, RenameMultipleCycle) {
    unittest::GoldenTestContext goldenCtx(&goldenTestConfig);
    // Cycle is formed due to predicates BASE.a --- A.b --- B.c --- C.d
    // Fields BASE.a, B.c, and C.d are renamed. Suffix $project has no effect.
    // It shouldn't matter which rename we use, as long as all renames resolve to same base coll
    // path, i.e. optimizer should treat dRenamed/dRenamed2 as the same path.
    const auto query = R"([
            {$project: {renamedField: "$a"}},
            {$lookup: {from: "A", localField: "renamedField", foreignField: "b", as: "fromA"}},
            {$unwind: "$fromA"},
            {$lookup: {from: "B", as: "fromB", pipeline: [
                {$project: {cRenamed: "$c", cRenamed2: "$c"}}
            ]}},
            {$unwind: "$fromB"},
            {$lookup: {from: "C", as: "fromC", pipeline: [
                {$project: {dRenamed: "$d", dRenamed2: "$d"}}
            ]}},
            {$unwind: "$fromC"},
            {$match: {$expr: {$eq: ["$fromC.dRenamed2", "$fromB.cRenamed"]}}},
            {$match: {$expr: {$eq: ["$fromC.dRenamed", "$renamedField"]}}},
            {$match: {$expr: {$eq: ["$fromA.x", "$fromC.dRenamed2"]}}},
            {$project: {renameAgain: "$fromC.dRenamed"}}
        ])";

    auto pipeline = makePipeline(query, {"A", "B", "C", "D"});
    markFieldsAsScalar(*pipeline, {"a"}, {{"A", {"b", "x"}}, {"B", {"c"}}, {"C", {"d"}}});
    ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));

    auto swJoinModel = AggJoinModel::constructJoinModel(*pipeline, defaultBuildParams);
    ASSERT_OK(swJoinModel);

    auto& joinModel = swJoinModel.getValue();
    const auto& resolvedPaths = joinModel.getResolvedPaths();
    ASSERT_EQ(resolvedPaths.size(), 5);

    // Path BASE.a is renamed to "renamedField".
    ASSERT_EQ(resolvedPaths[0].nodeId, 0);
    ASSERT_EQ(resolvedPaths[0].underlyingFieldPath.fullPath(), "a");
    ASSERT(resolvedPaths[0].fieldPathAfterRenames.has_value());
    ASSERT_EQ(resolvedPaths[0].fieldPathAfterRenames->fullPath(), "renamedField");

    // Path A.b is simple, no rename.
    ASSERT_EQ(resolvedPaths[1].nodeId, 1);
    ASSERT_EQ(resolvedPaths[1].underlyingFieldPath.fullPath(), "b");
    ASSERT_FALSE(resolvedPaths[1].fieldPathAfterRenames);

    // Path C.d is renamed to "dRenamed" & "dRenamed2". Note: we track the last one referenced.
    ASSERT_EQ(resolvedPaths[2].nodeId, 3);
    ASSERT_EQ(resolvedPaths[2].underlyingFieldPath.fullPath(), "d");
    ASSERT(resolvedPaths[2].fieldPathAfterRenames.has_value());
    ASSERT_EQ(resolvedPaths[2].fieldPathAfterRenames->fullPath(), "dRenamed2");

    // Path B.c is similar.
    ASSERT_EQ(resolvedPaths[3].nodeId, 2);
    ASSERT_EQ(resolvedPaths[3].underlyingFieldPath.fullPath(), "c");
    ASSERT(resolvedPaths[3].fieldPathAfterRenames.has_value());
    ASSERT_EQ(resolvedPaths[3].fieldPathAfterRenames->fullPath(), "cRenamed");

    // Path A.x has no renames.
    ASSERT_EQ(resolvedPaths[4].nodeId, 1);
    ASSERT_EQ(resolvedPaths[4].underlyingFieldPath.fullPath(), "x");
    ASSERT_FALSE(resolvedPaths[4].fieldPathAfterRenames.has_value());

    goldenCtx.outStream() << joinModel.toString(true) << std::endl;
}
}  // namespace mongo::join_ordering
