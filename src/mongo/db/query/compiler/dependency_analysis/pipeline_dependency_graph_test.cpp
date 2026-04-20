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


#include "mongo/db/query/compiler/dependency_analysis/pipeline_dependency_graph.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/document_source_facet.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/db/query/compiler/dependency_analysis/pipeline_dependency_graph_test_util.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/tenant_id.h"
#include "mongo/dbtests/dbtests.h"  // IWYU pragma: keep
#include "mongo/unittest/unittest.h"

#include <memory>
#include <string>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::pipeline::dependency_graph {
namespace {

class PipelineDependencyGraphTest : public unittest::Test {
protected:
    void SetUp() override {
        pathArrayness = std::make_shared<PathArrayness>();
    }

    void setPipeline(const std::string& array) {
        pipeline = parsePipeline(array);
        pipeline->getContext()->setPathArrayness(pathArrayness);
        stages.assign(pipeline->getSources().begin(), pipeline->getSources().end());
        canPathBeArray = [this](StringData path) -> bool {
            return pipeline->getContext()->canMainCollPathBeArray(FieldPath(path));
        };
        graph = std::make_unique<DependencyGraph>(pipeline->getSources(), canPathBeArray);
    }

    /**
     * Runs the given assertions after rebuilding the graph from every stage.
     *
     * NOTE: We cannot directly compare equality of dependency graphs due to non-deterministic
     * iteration of fields, which leads to unstable FieldIDs. Instead, we verify specific properties
     * that should hold regardless of the field order.
     */
    template <typename F>
    void runTest(F&& func) {
        recomputeAndAssert(*graph, *pipeline, std::forward<F>(func));
    }

    std::unique_ptr<Pipeline> pipeline;
    std::shared_ptr<PathArrayness> pathArrayness;
    CanPathBeArray canPathBeArray;
    std::vector<boost::intrusive_ptr<DocumentSource>> stages;
    std::unique_ptr<DependencyGraph> graph;

private:
    BSONObj pipelineFromJsonArray(const std::string& array) const {
        return fromjson("{pipeline: " + array + "}");
    }
    std::unique_ptr<Pipeline> parsePipeline(const std::string& inputPipeJson) const {
        const BSONObj inputBson = pipelineFromJsonArray(inputPipeJson);

        ASSERT_EQUALS(inputBson["pipeline"].type(), BSONType::array);
        std::vector<BSONObj> rawPipeline;
        for (auto&& stageElem : inputBson["pipeline"].Array()) {
            ASSERT_EQUALS(stageElem.type(), BSONType::object);
            rawPipeline.push_back(stageElem.embeddedObject());
        }

        const StringData kDBName = "test";
        const NamespaceString kTestNss =
            NamespaceString::createNamespaceString_forTest(kDBName, "collection");

        auto additionalNs = std::vector<NamespaceString>(
            {NamespaceString::createNamespaceString_forTest("test.coll_b"_sd),
             NamespaceString::createNamespaceString_forTest("test.coll_c"_sd),
             NamespaceString::createNamespaceString_forTest("test2.coll_d"_sd)});

        ResolvedNamespaceMap resolvedNs;
        resolvedNs.insert_or_assign(kTestNss, {kTestNss, std::vector<BSONObj>{}});
        for (auto&& ns : additionalNs) {
            resolvedNs.insert_or_assign(ns, {ns, std::vector<BSONObj>{}});
        }

        AggregateCommandRequest request(kTestNss, rawPipeline);
        boost::intrusive_ptr<ExpressionContextForTest> ctx = new ExpressionContextForTest();
        ctx->setResolvedNamespaces(resolvedNs);

        return pipeline_factory::makePipeline(
            request.getPipeline(), ctx, pipeline_factory::kOptionsMinimal);
    }
};

TEST_F(PipelineDependencyGraphTest, SubPipelineLookupHasSubGraph) {
    setPipeline(R"([{$lookup: {
        from: "coll_b",
        localField: "foo",
        foreignField: "b_foo",
        as: "docs",
        let: {},
        pipeline: [
            {$match: {b_ssn: 1}},
            {$set: {extra: 1}}
        ]
    }}])");

    runTest([&] {
        // $lookup should have a sub-pipeline graph.
        auto* subGraph = graph->getSubpipelineGraph(stages[0].get());
        ASSERT_NOT_EQUALS(subGraph, nullptr);
    });
}

TEST_F(PipelineDependencyGraphTest, SubPipelineNullForRegularStage) {
    setPipeline("[{$set: { a: 'foo' }}, {$match: { a: 'foo' }}]");
    runTest([&] {
        // Regular stages should not have sub-pipeline graphs.
        ASSERT_EQUALS(graph->getSubpipelineGraph(stages[0].get()), nullptr);
        ASSERT_EQUALS(graph->getSubpipelineGraph(stages[1].get()), nullptr);
    });
}

TEST_F(PipelineDependencyGraphTest, SubPipelineGetDeclaringStageDelegatesToSubGraph) {
    setPipeline(R"([{$lookup: {
        from: "coll_b",
        localField: "foo",
        foreignField: "b_foo",
        as: "docs",
        let: {},
        pipeline: [{$set: {b_ssn: 2}}]
    }}])");

    runTest([&] {
        // 'docs' itself is declared by the $lookup stage.
        ASSERT_EQUALS(graph->getDeclaringStageIncludingSubpipelines(nullptr, "docs").srcStages,
                      stages);
        ASSERT_EQUALS(graph->getDeclaringStage(nullptr, "docs"), stages[0]);

        // 'docs.b_ssn' should resolve across the $lookup into the sub-pipeline's $set stage.
        auto* subGraph = graph->getSubpipelineGraph(stages[0].get());
        ASSERT_NOT_EQUALS(subGraph, nullptr);
        auto result = graph->getDeclaringStageIncludingSubpipelines(nullptr, "docs.b_ssn");

        // The declaring stage should come from the sub-pipeline (the $set).
        auto subDeclaringStage = subGraph->getDeclaringStage(nullptr, "b_ssn");
        ASSERT_EQUALS(result.srcStages.back(), subDeclaringStage);
        ASSERT_TRUE(result.fromSubpipeline);
    });
}

TEST_F(PipelineDependencyGraphTest,
       SubPipelineWithDottedAsPathGetDeclaringStageDelegatesToSubGraph) {
    setPipeline(R"([{$lookup: {
        from: "coll_b",
        localField: "foo",
        foreignField: "b_foo",
        as: "docs.x",
        let: {},
        pipeline: [{$set: {b_ssn: 2}}]
    }}])");

    runTest([&] {
        // 'docs.x' itself is declared by the $lookup stage.
        ASSERT_EQUALS(graph->getDeclaringStageIncludingSubpipelines(nullptr, "docs.x").srcStages,
                      stages);
        ASSERT_EQUALS(graph->getDeclaringStage(nullptr, "docs.x"), stages[0]);

        // 'docs.x.b_ssn' should resolve across the $lookup into the sub-pipeline's $set stage.
        auto* subGraph = graph->getSubpipelineGraph(stages[0].get());
        ASSERT_NOT_EQUALS(subGraph, nullptr);
        auto result = graph->getDeclaringStageIncludingSubpipelines(nullptr, "docs.x.b_ssn");

        // The declaring stage should come from the sub-pipeline (the $set).
        auto subDeclaringStage = subGraph->getDeclaringStage(nullptr, "b_ssn");
        ASSERT_EQUALS(result.srcStages.back(), subDeclaringStage);
        ASSERT_TRUE(result.fromSubpipeline);
    });
}

TEST_F(PipelineDependencyGraphTest, SubPipelineGetDeclaringStageUnknownSubField) {
    setPipeline(R"([{$lookup: {
        from: "coll_b",
        localField: "foo",
        foreignField: "b_foo",
        as: "docs",
        let: {},
        pipeline: [{$set: {b_ssn: 2}}]
    }}])");

    runTest([&] {
        // 'docs.unknown' - the sub-pipeline's $set does not define 'unknown',
        // so getDeclaringStage should return nullptr (comes from the sub-pipeline's input).
        auto result = graph->getDeclaringStageIncludingSubpipelines(nullptr, "docs.unknown");
        ASSERT_EQUALS(result.srcStages.back(), nullptr);
        ASSERT_TRUE(result.fromSubpipeline);
    });
}

TEST_F(PipelineDependencyGraphTest, NestedSubPipelineGetDeclaringStageSubField) {
    setPipeline(R"([{$lookup: {
        from: "coll_b",
        localField: "foo",
        foreignField: "b_foo",
        as: "docs",
        let: {},
        pipeline: [{$lookup: {
            from: "coll_c",
            localField: "b_foo",
            foreignField: "c_foo",
            as: "rocks",
            let: {},
            pipeline: [{$set: {c_ssn: 2}}]
        }}]
    }}])");

    runTest([&] {
        // 'docs.rocks.unknown' - the inner sub-pipeline's $set does not define 'unknown',
        // so getDeclaringStage should return nullptr (comes from the sub-pipeline's input).
        auto unknownResult =
            graph->getDeclaringStageIncludingSubpipelines(nullptr, "docs.rocks.unknown");
        ASSERT_EQUALS(unknownResult.srcStages.back(), nullptr);
        ASSERT_TRUE(unknownResult.fromSubpipeline);

        // 'docs.rocks.c_ssn' is declared by the innermost $set stage.
        auto knownResult =
            graph->getDeclaringStageIncludingSubpipelines(nullptr, "docs.rocks.c_ssn");
        // get the stage generating 'c_ssn' (subpipeline of the subpipeline)
        auto innerSetStage = stages[0]->getSubPipeline()->front()->getSubPipeline()->front();
        ASSERT_EQUALS(knownResult.srcStages.back(), innerSetStage);
        ASSERT_TRUE(knownResult.fromSubpipeline);
    });
}

TEST_F(PipelineDependencyGraphTest, SubPipelineGetDeclaringStageWithInclusionProjection) {
    setPipeline(R"([{$lookup: {  
        from: "coll_b",  
        localField: "foo",  
        foreignField: "b_foo",  
        as: "docs",  
        let: {},  
        pipeline: [{$project: {b_ssn: 1}}]  
    }}])");

    runTest([&] {
        auto resultSubPipelines =
            graph->getDeclaringStageIncludingSubpipelines(nullptr, "docs.b_ssn");
        ASSERT_EQUALS(resultSubPipelines.srcStages.back(), nullptr);
        ASSERT_TRUE(resultSubPipelines.fromSubpipeline);

        auto resultMainPipeline = graph->getDeclaringStage(nullptr, "docs.b_ssn");
        ASSERT_EQUALS(resultMainPipeline, stages[0]);
    });
}

TEST_F(PipelineDependencyGraphTest, SubPipelineCanPathBeArrayDelegatesToSubGraph) {
    setPipeline(R"([{$lookup: {
        from: "coll_b",
        localField: "foo",
        foreignField: "b_foo",
        as: "docs",
        let: {},
        pipeline: [{$set: {b_ssn: 42}}]
    }}])");

    runTest([&] {
        // 'docs.b_ssn' is set to a constant integer, which cannot be an array.
        auto* subGraph = graph->getSubpipelineGraph(stages[0].get());
        ASSERT_NOT_EQUALS(subGraph, nullptr);
        ASSERT_FALSE(subGraph->canPathBeArray(nullptr, "b_ssn"));
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "docs.b_ssn"));
    });
}

TEST_F(PipelineDependencyGraphTest, SubPipelineCanPathBeArrayDelegatesToSubSubGraph) {
    setPipeline(R"([{$lookup: {
        from: "coll_b",
        localField: "foo",
        foreignField: "b_foo",
        as: "docs",
        let: {},
        pipeline: [
            {$set: {b_ssn: 1}},
            {$lookup: {
                from: "coll_c",
                localField: "bar",
                foreignField: "c_bar",
                as: "inner_docs",
                let: {},
                pipeline: [{$set: {c_ssn: 99}}]
            }}
        ]
    }}])");

    runTest([&] {
        // 'docs.b_ssn' is set to a constant integer, which cannot be an array.
        auto* subGraph = graph->getSubpipelineGraph(stages[0].get());
        ASSERT_NOT_EQUALS(subGraph, nullptr);
        ASSERT_FALSE(subGraph->canPathBeArray(nullptr, "b_ssn"));
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "docs.b_ssn"));
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "docs.inner_docs.c_ssn"));
    });
}

TEST_F(PipelineDependencyGraphTest, SubPipelineLookupDottedPathDelegation) {
    setPipeline(R"([{$lookup: {
        from: "coll_b",
        localField: "foo",
        foreignField: "b_foo",
        as: "docs",
        let: {},
        pipeline: [
            {$set: {a: {b: 42}}}
        ]
    }},
    {$match: {"docs.a": 1}}])");

    runTest([&] {
        auto* matchStage = stages[1].get();
        // 'docs.a' should resolve through the $lookup into the subpipeline's $set.
        auto* subGraph = graph->getSubpipelineGraph(stages[0].get());
        ASSERT_NOT_EQUALS(subGraph, nullptr);
        auto subDeclStage = subGraph->getDeclaringStage(nullptr, "a");
        auto result = graph->getDeclaringStageIncludingSubpipelines(matchStage, "docs.a");
        ASSERT_EQUALS(result.srcStages.back(), subDeclStage);
        ASSERT_TRUE(result.fromSubpipeline);
    });
}

TEST_F(PipelineDependencyGraphTest, SubPipelineLookupInclusionProjection) {
    setPipeline(R"([{$lookup: {
        from: "coll_b",
        localField: "foo",
        foreignField: "b_foo",
        as: "docs",
        let: {},
        pipeline: [{$project: {b_ssn: 1}}]
    }}])");

    runTest([&] {
        auto* subGraph = graph->getSubpipelineGraph(stages[0].get());
        ASSERT_NOT_EQUALS(subGraph, nullptr);

        ASSERT_EQUALS(
            graph->getDeclaringStageIncludingSubpipelines(nullptr, "docs").srcStages.back(),
            stages[0]);

        // 'docs.b_ssn' crosses into the sub-pipeline. The inclusion projection preserves
        // b_ssn from the sub-pipeline's input, so it originates from the base collection.
        auto result = graph->getDeclaringStageIncludingSubpipelines(nullptr, "docs.b_ssn");
        ASSERT_EQUALS(result.srcStages.back(), nullptr);
        ASSERT_TRUE(result.fromSubpipeline);

        // 'docs.other' is excluded by the inclusion projection, so it's declared by the $project
        // (deleted).
        auto otherResult = graph->getDeclaringStageIncludingSubpipelines(nullptr, "docs.other");
        ASSERT_NOT_EQUALS(otherResult.srcStages.back(), nullptr);
        ASSERT_TRUE(otherResult.fromSubpipeline);

        // Within the sub-pipeline, b_ssn comes from the base collection (unknown arrayness).
        ASSERT_TRUE(subGraph->canPathBeArray(nullptr, "b_ssn"));
    });
}

TEST_F(PipelineDependencyGraphTest, AddFieldsUnionWithMatchDependencies) {
    // Pipeline:
    //   [0] $addFields: { s: "A" }
    //   [1] $unionWith: { coll: "coll_c", pipeline: [{ $addFields: { s: "B" } }] }
    //   [2] $match: { s: "A" }
    setPipeline(R"([
        {$addFields: {s: "A"}},
        {$unionWith: {
            coll: "coll_c",
            pipeline: [{$addFields: {s: "B"}}]
        }},
        {$match: {s: "A"}}
    ])");

    runTest([&] {
        // $unionWith should have a sub-pipeline with exactly one stage.
        auto* subGraph = graph->getSubpipelineGraph(stages[1].get());
        ASSERT_NOT_EQUALS(subGraph, nullptr);

        // $addFields and $match should not have sub-pipelines.
        ASSERT_EQUALS(graph->getSubpipelineGraph(stages[0].get()), nullptr);
        ASSERT_EQUALS(graph->getSubpipelineGraph(stages[2].get()), nullptr);

        // From $match (stages[2]), 's' is attributed to $unionWith (stages[1]) since
        // $unionWith replaces all paths with an exhaustive scope.
        ASSERT_EQUALS(
            graph->getDeclaringStageIncludingSubpipelines(stages[2].get(), "s").srcStages.back(),
            stages[1]);

        // Within the sub-pipeline, 's' is declared by the sub-pipeline's $addFields.
        auto subDeclStage =
            subGraph->getDeclaringStageIncludingSubpipelines(nullptr, "s").srcStages.back();
        ASSERT_NOT_EQUALS(subDeclStage, nullptr);

        // After $unionWith, 's' could come from either branch so canPathBeArray is true.
        ASSERT_TRUE(graph->canPathBeArray(stages[2].get(), "s"));

        // Within the sub-pipeline, 's' is set to constant string "B", so it cannot be an array.
        ASSERT_FALSE(subGraph->canPathBeArray(nullptr, "s"));
    });
}

TEST_F(PipelineDependencyGraphTest, AddFieldsUnionWithMatchDependenciesWithAddingArrayValue) {
    // Pipeline:
    //   [0] $addFields: { s: ["A", "B"] }
    //   [1] $unionWith: { coll: "coll_c", pipeline: [{ $addFields: { s: "B" } }] }
    //   [2] $match: { s: "A" }
    setPipeline(R"([
        {$addFields: {s: ["A", "B"]}},
        {$unionWith: {
            coll: "coll_c",
            pipeline: [{$addFields: {s: "B"}}]
        }},
        {$match: {s: "A"}}
    ])");

    runTest([&] {
        // After $unionWith, 's' could come from either branch so canPathBeArray is true.
        ASSERT_TRUE(graph->canPathBeArray(stages[2].get(), "s"));
    });
}

TEST_F(PipelineDependencyGraphTest, SubPipelineCanPathBeArrayUnknownSubField) {
    setPipeline(R"([{$lookup: {
        from: "coll_b",
        localField: "foo",
        foreignField: "b_foo",
        as: "docs",
        let: {},
        pipeline: [{$set: {b_ssn: 2}}]
    }}])");

    runTest([&] {
        // 'docs.unknown' comes from the sub-pipeline's input collection, arrayness is unknown.
        ASSERT_TRUE(graph->canPathBeArray(nullptr, "docs.unknown"));
    });
}

TEST_F(PipelineDependencyGraphTest, SubPipelineGetDeclaringStageThenMatch) {
    setPipeline(R"([{$lookup: {
        from: "coll_b",
        localField: "foo",
        foreignField: "b_foo",
        as: "docs",
        let: {},
        pipeline: [{$set: {b_ssn: 2}}]
    }},
    {$match: {"docs.b_ssn": 1}}])");

    runTest([&] {
        auto* last = stages.back().get();
        // 'docs.b_ssn' visible from the $match stage should still resolve into the sub-pipeline.
        auto result = graph->getDeclaringStageIncludingSubpipelines(last, "docs.b_ssn");
        auto* subGraph = graph->getSubpipelineGraph(stages[0].get());
        ASSERT_NOT_EQUALS(subGraph, nullptr);
        auto subDeclaringStage = subGraph->getDeclaringStage(nullptr, "b_ssn");
        ASSERT_EQUALS(result.srcStages.back(), subDeclaringStage);
        ASSERT_TRUE(result.fromSubpipeline);
    });
}

TEST_F(PipelineDependencyGraphTest, SubPipelineUnionWithHasSubGraph) {
    setPipeline(R"([{$unionWith: {
        coll: "coll_c",
        pipeline: [{$set: {x: 1}}, {$set: {y: 2}}]
    }}])");

    runTest([&] {
        auto* subGraph = graph->getSubpipelineGraph(stages[0].get());
        ASSERT_NOT_EQUALS(subGraph, nullptr);
    });
}

TEST_F(PipelineDependencyGraphTest, SubPipelineUnionWithNullForRegularStage) {
    setPipeline(R"([{$unionWith: {
        coll: "coll_c",
        pipeline: [{$set: {x: 1}}]
    }},
    {$match: {x: 1}}])");

    runTest([&] {
        // $unionWith has a sub-pipeline graph.
        ASSERT_NOT_EQUALS(graph->getSubpipelineGraph(stages[0].get()), nullptr);
        // $match does not.
        ASSERT_EQUALS(graph->getSubpipelineGraph(stages[1].get()), nullptr);
    });
}

TEST_F(PipelineDependencyGraphTest, SubPipelineUnionWithDeclaringStage) {
    setPipeline(R"([{$unionWith: {
        coll: "coll_c",
        pipeline: [{$set: {x: 1}}]
    }},
    {$match: {x: 1}}])");

    runTest([&] {
        auto* matchStage = stages[1].get();
        // After $unionWith, any field is attributed to the $unionWith stage since it
        // replaces all paths (kAllPaths).
        auto declStage =
            graph->getDeclaringStageIncludingSubpipelines(matchStage, "x").srcStages.back();
        ASSERT_EQUALS(declStage, stages[0]);
    });
}

TEST_F(PipelineDependencyGraphTest, SubPipelineUnionWithDeclaringStageUnknownField) {
    setPipeline(R"([{$unionWith: {
        coll: "coll_c",
        pipeline: [{$set: {x: 1}}]
    }},
    {$match: {y: 1}}])");

    runTest([&] {
        auto* matchStage = stages[1].get();
        // Even fields NOT in the sub-pipeline are attributed to $unionWith since
        // it creates an exhaustive scope.
        auto declStage =
            graph->getDeclaringStageIncludingSubpipelines(matchStage, "y").srcStages.back();
        ASSERT_EQUALS(declStage, stages[0]);
    });
}

TEST_F(PipelineDependencyGraphTest, SubPipelineLookupEmptyPipeline) {
    setPipeline(R"([{$lookup: {
        from: "coll_b",
        localField: "foo",
        foreignField: "b_foo",
        as: "docs"
    }}])");

    runTest([&] {
        auto* subGraph = graph->getSubpipelineGraph(stages[0].get());
        ASSERT_NOT_EQUALS(subGraph, nullptr);
    });
}

TEST_F(PipelineDependencyGraphTest, SubPipelineMultipleStagesWithSubPipelines) {
    setPipeline(R"([{$lookup: {
        from: "coll_b",
        localField: "foo",
        foreignField: "b_foo",
        as: "docs",
        let: {},
        pipeline: [{$set: {extra: 1}}]
    }},
    {$unionWith: {
        coll: "coll_c",
        pipeline: [{$set: {x: 1}}]
    }},
    {$match: {x: 1}}])");

    runTest([&] {
        // Both $lookup and $unionWith should have subpipeline graphs.
        auto* lookupSubGraph = graph->getSubpipelineGraph(stages[0].get());
        ASSERT_NOT_EQUALS(lookupSubGraph, nullptr);

        auto* unionSubGraph = graph->getSubpipelineGraph(stages[1].get());
        ASSERT_NOT_EQUALS(unionSubGraph, nullptr);

        // $match should not have a subpipeline graph.
        ASSERT_EQUALS(graph->getSubpipelineGraph(stages[2].get()), nullptr);
    });
}

TEST_F(PipelineDependencyGraphTest, SubPipelineNestedLookup) {
    setPipeline(R"([{$lookup: {
        from: "coll_b",
        localField: "foo",
        foreignField: "b_foo",
        as: "docs",
        let: {},
        pipeline: [
            {$set: {extra: 1}},
            {$lookup: {
                from: "coll_c",
                localField: "bar",
                foreignField: "c_bar",
                as: "inner_docs",
                let: {},
                pipeline: [{$set: {deep: 99}}]
            }}
        ]
    }}])");

    runTest([&] {
        // Outer $lookup has a subpipeline graph.
        auto* outerSubGraph = graph->getSubpipelineGraph(stages[0].get());
        ASSERT_NOT_EQUALS(outerSubGraph, nullptr);

        // The inner $lookup (second stage of outer subpipeline) should also have
        // its own subpipeline graph via the outer subgraph.
        auto* outerSubStages = pipeline->getSources().front()->getSubPipeline();
        auto innerLookupIt = outerSubStages->begin();
        std::advance(innerLookupIt, 1);  // second stage = inner $lookup
        auto* innerSubGraph = outerSubGraph->getSubpipelineGraph(innerLookupIt->get());
        ASSERT_NOT_EQUALS(innerSubGraph, nullptr);
    });
}

TEST_F(PipelineDependencyGraphTest, SubPipelineUnionWithCanPathBeArray) {
    setPipeline(R"([{$unionWith: {
        coll: "coll_c",
        pipeline: [{$set: {x: 42}}]
    }},
    {$match: {x: 1}}])");

    runTest([&] {
        auto* matchStage = stages[1].get();
        // After $unionWith (exhaustive scope), canPathBeArray is conservatively true
        // because documents can come from either stream.
        ASSERT_TRUE(graph->canPathBeArray(matchStage, "x"));
        ASSERT_TRUE(graph->canPathBeArray(matchStage, "y"));
    });
}

TEST_F(PipelineDependencyGraphTest, SubPipelineUnionWithCanPathBeArrayBothNonArray) {
    setPipeline(R"([
        {$set: {x: 42}},
        {$unionWith: {
            coll: "coll_c",
            pipeline: [{$set: {x: 99}}]
        }},
        {$match: {x: 1}}
    ])");

    runTest([&] {
        auto* matchStage = stages[2].get();
        // Even though 'x' is a non-array constant in both the main pipeline ($set: {x: 42})
        // and the sub-pipeline ($set: {x: 99}), after $unionWith the field is conservatively
        // considered to potentially be an array because $unionWith creates an exhaustive scope.
        ASSERT_TRUE(graph->canPathBeArray(matchStage, "x"));

        // Within the sub-pipeline, x=99 (constant int), so canPathBeArray is false.
        auto* subGraph = graph->getSubpipelineGraph(stages[1].get());
        ASSERT_NOT_EQUALS(subGraph, nullptr);
        ASSERT_FALSE(subGraph->canPathBeArray(nullptr, "x"));
    });
}

TEST_F(PipelineDependencyGraphTest, SubPipelineUnionWithSubGraphDeclaringStage) {
    setPipeline(R"([{$unionWith: {
        coll: "coll_c",
        pipeline: [{$set: {x: 1}}]
    }},
    {$match: {x: 1}}])");

    runTest([&] {
        // Although getDeclaringStage for the main pipeline returns $unionWith,
        // we can independently query the sub-pipeline graph.
        auto* subGraph = graph->getSubpipelineGraph(stages[0].get());
        ASSERT_NOT_EQUALS(subGraph, nullptr);

        auto subDeclStage =
            subGraph->getDeclaringStageIncludingSubpipelines(nullptr, "x").srcStages.back();
        ASSERT_NOT_EQUALS(subDeclStage, nullptr);
        // The sub-pipeline's $set declares "x".
    });
}

TEST_F(PipelineDependencyGraphTest, SubPipelineUnionWithSubGraphCanPathBeArray) {
    setPipeline(R"([{$unionWith: {
        coll: "coll_c",
        pipeline: [{$set: {x: 42}}]
    }},
    {$match: {x: 1}}])");

    runTest([&] {
        auto* subGraph = graph->getSubpipelineGraph(stages[0].get());
        ASSERT_NOT_EQUALS(subGraph, nullptr);

        // Within the sub-pipeline, x=42 (constant int), so canPathBeArray is false.
        ASSERT_FALSE(subGraph->canPathBeArray(nullptr, "x"));
        // Unknown fields in the sub-pipeline: conservative true.
        ASSERT_TRUE(subGraph->canPathBeArray(nullptr, "unknown_field"));
    });
}

TEST_F(PipelineDependencyGraphTest, SubPipelineUnionWithThenSet) {
    setPipeline(R"([{$unionWith: {
        coll: "coll_c",
        pipeline: [{$set: {x: 1}}]
    }},
    {$set: {x: 99}},
    {$match: {x: 1}}])");

    runTest([&] {
        auto* matchStage = stages[2].get();
        // $set after $unionWith overrides: field "x" is now declared by the $set.
        auto declStage =
            graph->getDeclaringStageIncludingSubpipelines(matchStage, "x").srcStages.back();
        ASSERT_EQUALS(declStage, stages[1]);

        // "y" not set by the outer $set, still attributed to $unionWith.
        auto declY =
            graph->getDeclaringStageIncludingSubpipelines(matchStage, "y").srcStages.back();
        ASSERT_EQUALS(declY, stages[0]);
    });
}

TEST_F(PipelineDependencyGraphTest, SubPipelineUnionWithThenSetCanPathBeArray) {
    setPipeline(R"([{$unionWith: {
        coll: "coll_c",
        pipeline: [{$set: {x: 1}}]
    }},
    {$set: {x: 42}},
    {$match: {x: 1}}])");

    runTest([&] {
        auto* matchStage = stages[2].get();
        // $set {x: 42} after $unionWith: x is a constant int, not an array.
        ASSERT_FALSE(graph->canPathBeArray(matchStage, "x"));
        // "y" still comes from the exhaustive scope, so conservatively true.
        ASSERT_TRUE(graph->canPathBeArray(matchStage, "y"));
    });
}

TEST_F(PipelineDependencyGraphTest,
       SubPipelineUnionWithExhaustiveScopesBothBranchesExcludeFieldCanPathBeArray) {
    // Both branches have inclusion projections that keep only 'x' — 'y' is truly absent
    // from the outer pipeline and from the $unionWith sub-pipeline.  Despite this, the
    // graph cannot reason across both branches of a $unionWith, so after the union it
    // still conservatively reports canPathBeArray(y) = true.
    setPipeline(R"([
        {$project: {x: 1, _id: 0}},
        {$unionWith: {
            coll: "coll_c",
            pipeline: [{$project: {x: 1, _id: 0}}]
        }},
        {$match: {y: 1}}
    ])");

    runTest([&] {
        pathArrayness->addPath("y", {}, true);
        // Before $unionWith: the outer $project {x: 1} makes 'y' truly absent.
        auto unionWith = stages[1].get();
        ASSERT_FALSE(graph->canPathBeArray(unionWith, "y"));

        // Inside the sub-pipeline: same $project also makes 'y' truly absent.
        auto* subGraph = graph->getSubpipelineGraph(unionWith);
        ASSERT_NOT_EQUALS(subGraph, nullptr);
        ASSERT_FALSE(subGraph->canPathBeArray(nullptr, "y"));

        // After $unionWith: the graph cannot cross-reference both branches, so 'y'
        // is conservatively assumed to be potentially present and array-typed.
        auto match = stages[2].get();
        ASSERT_TRUE(graph->canPathBeArray(match, "y"));
    });
}

TEST_F(PipelineDependencyGraphTest, SubPipelineWithEmptyPipeline) {
    setPipeline(R"([
        {$match: {my_id: 100}},
        {$lookup: {
            from: "coll_b", 
            localField: "b", 
            foreignField: "b",
            as: "B_data"
        }},
        {$unwind: "$B_data"},
        {$match: {"B_data.indicator": "Y"}},
        {$lookup: {
            from: "coll_c", 
            localField: "b", 
            foreignField: "b", 
            as: "C_data"
        }},
        {$unwind: "$C_data"},
        {
            $addFields: {
                zip: "$C_data.other_id.zip"
            }
    }])");

    runTest([&] { ASSERT_TRUE(graph->canPathBeArray(nullptr, "zip")); });
}

TEST_F(PipelineDependencyGraphTest, SimpleCase) {
    setPipeline(
        "[{$set: { a: 'foo' }},"
        "{$match: { a: 'foo' }}]");

    runTest([&] { ASSERT_EQUALS(graph->getDeclaringStage(nullptr, "a"), stages[0]); });
}

TEST_F(PipelineDependencyGraphTest, Shadowing) {
    setPipeline(
        "[{$set: { a: 'foo' }},"
        "{$set: { b: 'bar' }},"
        "{$set: { a: 'baz' }},"
        "{$match: { a: 'baz' }}]");

    runTest([&] { ASSERT_EQUALS(graph->getDeclaringStage(nullptr, "a"), stages[2]); });
}

TEST_F(PipelineDependencyGraphTest, Shadowing2) {
    setPipeline(
        "[{$set: { a: 'foo' }},"
        "{$set: { b: 'bar' }},"
        "{$match: { a: 'baz' }},"
        "{$set: { a: 'baz' }}]");

    runTest([&] {
        // Expected stage is the first one since $match comes before the last $set
        ASSERT_EQUALS(graph->getDeclaringStage(stages[2].get(), "a"), stages[0]);
    });
}

TEST_F(PipelineDependencyGraphTest, UnknownField) {
    setPipeline("[{$match: { a: 'baz' }}]");

    runTest([&] {
        // Return nullptr to indicate it comes from document.
        ASSERT_EQUALS(graph->getDeclaringStage(nullptr, "a"), nullptr);
    });
}

TEST_F(PipelineDependencyGraphTest, UnknownComplex) {
    setPipeline("[{$match: { 'a.b': 'baz' }}]");

    runTest([&] {
        // Return nullptr to indicate it comes from document.
        ASSERT_EQUALS(graph->getDeclaringStage(nullptr, "a.b"), nullptr);
    });
}

TEST_F(PipelineDependencyGraphTest, UnknownComplexPrefix) {
    setPipeline("[{$set: { 'a.b': 'baz' }}]");

    runTest([&] {
        // Return stages[0] to indicate it was modified by setting the prefix.
        ASSERT_EQUALS(graph->getDeclaringStage(nullptr, "a.b.c"), stages[0]);
    });
}

TEST_F(PipelineDependencyGraphTest, UnknownFieldAfterExhaustive) {
    setPipeline(
        "[{$replaceRoot: { newRoot: {} }},"
        "{$set: { b: 'bar' }},"
        "{$match: { a: 'baz' }}]");

    runTest([&] {
        // Return stage[0] to indicate it would be modified by $replaceRoot.
        ASSERT_EQUALS(graph->getDeclaringStage(nullptr, "a"), stages[0]);
    });
}

TEST_F(PipelineDependencyGraphTest, UnknownComplexAfterExhaustive) {
    setPipeline(
        "[{$replaceRoot: { newRoot: {} }},"
        "{$set: { b: 'bar' }},"
        "{$match: { 'a.b': 'baz' }}]");

    runTest([&] {
        // Return stage[0] to indicate it would be modified by $replaceRoot.
        ASSERT_EQUALS(graph->getDeclaringStage(nullptr, "a.b"), stages[0]);
    });
}

TEST_F(PipelineDependencyGraphTest, MatchMultiple) {
    setPipeline(
        "[{$set: { a: 'foo' }},"
        "{$set: { b: 'bar' }},"
        "{$match: { a: 'foo', b: 'bar' }}]");

    runTest([&] {
        // For field 'a', expect first stage.
        ASSERT_EQUALS(graph->getDeclaringStage(nullptr, "a"), stages[0]);
        // For field 'b', expect second stage.
        ASSERT_EQUALS(graph->getDeclaringStage(nullptr, "b"), stages[1]);
    });
}

TEST_F(PipelineDependencyGraphTest, MatchMultipleWithShadowing) {
    setPipeline(
        "[{$set: { a: 'foo', b: 'bar' }},"
        "{$set: { a: 'foo2' }},"
        "{$set: { b: 'bar2' }},"
        "{$match: { a: 'foo', b: 'bar' }}]");

    runTest([&] {
        // For field 'a', expect stage 2 (the shadowing stage)
        ASSERT_EQUALS(graph->getDeclaringStage(nullptr, "a"), stages[1]);
        // For field 'b', expect stage 3
        ASSERT_EQUALS(graph->getDeclaringStage(nullptr, "b"), stages[2]);
    });
}

TEST_F(PipelineDependencyGraphTest, MatchMultipleWithPartialShadowing) {
    setPipeline(
        "[{$set: { a: 'foo', b: 'bar' }},"
        "{$set: { b: 'foo2' }},"
        "{$match: { a: 'foo', b: 'foo2' }}]");

    runTest([&] {
        // For field 'a', expect stage 1 (no shadowing for 'a')
        ASSERT_EQUALS(graph->getDeclaringStage(nullptr, "a"), stages[0]);

        // For field 'b', expect stage 2 (shadowing stage)
        ASSERT_EQUALS(graph->getDeclaringStage(nullptr, "b"), stages[1]);
    });
}

TEST_F(PipelineDependencyGraphTest, FalseDependency) {
    setPipeline(
        "[{$set: { a: 'foo' }},"
        "{$set: { b: 'bar' }},"
        "{$set: { a: '$$REMOVE' }},"
        "{$match: { a: 'foo', b: 'bar' }}]");

    runTest([&] {
        // For field 'a', expect stage 3 (the $$REMOVE stage)
        ASSERT_EQUALS(graph->getDeclaringStage(nullptr, "a"), stages[2]);
    });
}

TEST_F(PipelineDependencyGraphTest, FalseDependencyFromInclusionProjection) {
    setPipeline(
        "[{$set: { a: 'foo' }},"
        "{$set: { b: 'bar' }},"
        "{$set: { c: 'baz' }},"
        "{$project: { b: 1, c: 1 }},"
        "{$match: { a: 'foo', b: 'bar' }}]");

    runTest([&] {
        // For field 'a', expect stage 4 (the $project that excludes 'a')
        ASSERT_EQUALS(graph->getDeclaringStage(nullptr, "a"), stages[3]);
    });
}

TEST_F(PipelineDependencyGraphTest, FalseDependencyFromInclusionProjectionWithUndefinedField) {
    setPipeline(
        "[{$set: { b: 'bar' }},"
        "{$set: { c: 'baz' }},"
        "{$project: { b: 1, c: 1 }},"
        "{$match: { a: 'foo', b: 'bar' }}]");

    runTest([&] {
        // For field 'a', expect stage 3 (the $project)
        ASSERT_EQUALS(graph->getDeclaringStage(nullptr, "a"), stages[2]);
    });
}

TEST_F(PipelineDependencyGraphTest, ComplexPathShadowing) {
    setPipeline(
        "[{$set: { d: 1 }},"
        "{$set: { 'd.b.c': 1 }},"
        "{$set: { 'd.b': 1, 'd.a': 1 }}]");

    runTest([&] {
        // Lookup from the end of the pipeline.
        ASSERT_EQUALS(graph->getDeclaringStage(nullptr, "d.b.c"), stages.back());
    });
}

TEST_F(PipelineDependencyGraphTest, ComplexPathInclusionProjection) {
    setPipeline(
        "[{$set: { 'a.b.c': 1 }},"
        "{$project: { 'a.b': 1 }},"
        "{$set: { 'c': 1 }}]");

    runTest(
        [&] { ASSERT_EQUALS(graph->getDeclaringStage(stages.back().get(), "a.b.c"), stages[0]); });
}

TEST_F(PipelineDependencyGraphTest, ComplexPathInclusionProjectionNonExistent) {
    setPipeline(
        "[{$set: { 'a.b.c': 1 }},"
        "{$project: { 'a.b.d': 1 }},"
        "{$set: { 'c': 1 }}]");

    runTest([&] {
        // The inclusion modified a (filtered its subfields).
        ASSERT_EQUALS(graph->getDeclaringStage(stages.back().get(), "a"), stages[1]);
        // The inclusion modified a.b (filtered its subfields).
        ASSERT_EQUALS(graph->getDeclaringStage(stages.back().get(), "a.b"), stages[1]);
        // Excluded by the inclusion.
        ASSERT_EQUALS(graph->getDeclaringStage(stages.back().get(), "a.b.c"), stages[1]);
        // Preserved from the base doc, never defined by any stage.
        ASSERT_EQUALS(graph->getDeclaringStage(stages.back().get(), "a.b.d"), nullptr);
        // Excluded by the inclusion.
        ASSERT_EQUALS(graph->getDeclaringStage(stages.back().get(), "a.b.c.e"), stages[1]);
    });
}

TEST_F(PipelineDependencyGraphTest, ComplexPathInclusionProjectionModifiedPath) {
    setPipeline(
        "[{$set: { 'a.b.c': 1 }},"
        "{$project: { 'a.b.c.d': 1 }},"
        "{$set: { 'c': 1 }}]");

    runTest([&] {
        // The inclusion modified a (filtered its subfields).
        ASSERT_EQUALS(graph->getDeclaringStage(stages.back().get(), "a"), stages[1]);
        // The inclusion modified a.b (filtered its subfields).
        ASSERT_EQUALS(graph->getDeclaringStage(stages.back().get(), "a.b"), stages[1]);
        // The inclusion modified a.b.c (filtered its subfields).
        ASSERT_EQUALS(graph->getDeclaringStage(stages.back().get(), "a.b.c"), stages[1]);
        // TODO(SERVER-119392): This is technically kept by the inclusion (prefix "a.b.c" was
        // defined by the $set), so the declaring field should be $set.
        ASSERT_EQUALS(graph->getDeclaringStage(stages.back().get(), "a.b.c.d"), stages[1]);
        // Excluded by the inclusion.
        ASSERT_EQUALS(graph->getDeclaringStage(stages.back().get(), "a.b.c.e"), stages[1]);
    });
}

TEST_F(PipelineDependencyGraphTest, InclusionBaseCollectionField) {
    setPipeline(
        "[{$set: { a: 1 }},"
        "{$project: { a: 1, b: 1 }},"
        "{$set: { c: 1 }}]");

    runTest([&] {
        auto* last = stages.back().get();
        // 'a' was set by stage 0, preserved by inclusion.
        ASSERT_EQUALS(graph->getDeclaringStage(last, "a"), stages[0]);
        // 'b' never defined — from base collection.
        ASSERT_EQUALS(graph->getDeclaringStage(last, "b"), nullptr);
        // 'd' not included — excluded by projection.
        ASSERT_EQUALS(graph->getDeclaringStage(last, "d"), stages[1]);
    });
}

TEST_F(PipelineDependencyGraphTest, InclusionAfterExhaustiveStage) {
    setPipeline(
        "[{$replaceRoot: { newRoot: '$x' }},"
        "{$project: { a: 1, b: 1 }},"
        "{$set: { c: 1 }}]");

    runTest([&] {
        auto* last = stages.back().get();
        // 'a' included but originates from $replaceRoot.
        ASSERT_EQUALS(graph->getDeclaringStage(last, "a"), stages[0]);
        // 'b' same — from $replaceRoot.
        ASSERT_EQUALS(graph->getDeclaringStage(last, "b"), stages[0]);
        // 'd' not included — excluded by projection.
        ASSERT_EQUALS(graph->getDeclaringStage(last, "d"), stages[1]);
    });
}

TEST_F(PipelineDependencyGraphTest, InclusionDottedBaseCollectionMultipleSubfields) {
    setPipeline(
        "[{$project: { 'a.b': 1, 'a.c': 1 }},"
        "{$match: { 'a.b': 1, 'a.c': 1, 'a.d': 1 }}]");

    runTest([&] {
        auto* last = stages.back().get();
        // 'a' modified by inclusion (by excluding subfields).
        ASSERT_EQUALS(graph->getDeclaringStage(last, "a"), stages[0]);
        // 'a.b' and 'a.c' included from base collection.
        ASSERT_EQUALS(graph->getDeclaringStage(last, "a.b"), nullptr);
        ASSERT_EQUALS(graph->getDeclaringStage(last, "a.c"), nullptr);
        // 'a.d' excluded.
        ASSERT_EQUALS(graph->getDeclaringStage(last, "a.d"), stages[0]);
    });
}

TEST_F(PipelineDependencyGraphTest, InclusionLongDottedBaseCollectionMultipleSubfields) {
    setPipeline(
        "[{$project: { 'a.b.c': 1, 'a.b.d': 1 }},"
        "{$match: { 'a.b': 1, 'a.b.c': 1, 'a.b.d': 1 }}]");

    runTest([&] {
        auto* last = stages.back().get();
        // 'a' modified by inclusion (by excluding subfields).
        ASSERT_EQUALS(graph->getDeclaringStage(last, "a"), stages[0]);
        // 'a.b' modified by inclusion (by excluding subfields).
        ASSERT_EQUALS(graph->getDeclaringStage(last, "a.b"), stages[0]);
        // 'a.b.c' and 'a.b.d' included from base collection.
        ASSERT_EQUALS(graph->getDeclaringStage(last, "a.b.d"), nullptr);
        ASSERT_EQUALS(graph->getDeclaringStage(last, "a.b.c"), nullptr);
        // 'a.d' excluded.
        ASSERT_EQUALS(graph->getDeclaringStage(last, "a.d"), stages[0]);
    });
}

TEST_F(PipelineDependencyGraphTest, ChainedInclusionProjections) {
    setPipeline(
        "[{$set: { a: 1, b: 1, c: 1 }},"
        "{$project: { a: 1, b: 1 }},"
        "{$project: { a: 1 }},"
        "{$match: { a: 1 }}]");

    runTest([&] {
        auto* last = stages.back().get();
        // 'a' preserved through both inclusions, defined by from $set.
        ASSERT_EQUALS(graph->getDeclaringStage(last, "a"), stages[0]);
        // 'b' excluded by second projection.
        ASSERT_EQUALS(graph->getDeclaringStage(last, "b"), stages[2]);
        // 'c' excluded by both projections, but most recently by the second projection.
        // One could argue that the second $project didn't change 'c' since it was already excluded
        // by the first one, but for simplicity we don't consider that when building the graph. This
        // is consistent with dependency tracking for exclusion projections (see
        // 'ChainedExclusionProjections').
        ASSERT_EQUALS(graph->getDeclaringStage(last, "c"), stages[2]);
    });
}

TEST_F(PipelineDependencyGraphTest, ChainedExclusionProjections) {
    setPipeline(
        "[{$set: { a: 1, b: 1, c: 1 }},"
        "{$project: { c: 0 }},"
        "{$project: { b: 0, c: 0 }},"
        "{$match: { a: 1 }}]");

    runTest([&] {
        auto* last = stages.back().get();
        // 'a' preserved through both inclusions, defined by from $set.
        ASSERT_EQUALS(graph->getDeclaringStage(last, "a"), stages[0]);
        // 'b' excluded by second projection.
        ASSERT_EQUALS(graph->getDeclaringStage(last, "b"), stages[2]);
        // 'c' excluded by both projections, but most recently by the second projection.
        ASSERT_EQUALS(graph->getDeclaringStage(last, "c"), stages[2]);
    });
}

TEST_F(PipelineDependencyGraphTest, InclusionDottedAfterExhaustive) {
    setPipeline(
        "[{$replaceRoot: { newRoot: '$x' }},"
        "{$project: { 'a.b': 1 }},"
        "{$match: { 'a.b': 1, 'a.c': 1 }}]");

    runTest([&] {
        auto* last = stages.back().get();
        // 'a' modified by inclusion.
        ASSERT_EQUALS(graph->getDeclaringStage(last, "a"), stages[1]);
        // 'a.b' included — originates from $replaceRoot.
        ASSERT_EQUALS(graph->getDeclaringStage(last, "a.b"), stages[0]);
        // 'a.c' not included — excluded by projection.
        ASSERT_EQUALS(graph->getDeclaringStage(last, "a.c"), stages[1]);
    });
}

TEST_F(PipelineDependencyGraphTest, SetFieldThenIncludeDottedPath) {
    setPipeline(
        "[{$set: { a: 1 }},"
        "{$project: { 'a.b': 1 }},"
        "{$match: { 'a.b': 1, 'a.c': 1 }}]");

    runTest([&] {
        auto* last = stages.back().get();
        // 'a' modified by inclusion (by filtering subfields).
        ASSERT_EQUALS(graph->getDeclaringStage(last, "a"), stages[1]);
        // TODO(SERVER-119392): 'a.b' preserved by projection, originates from $set (we currently
        // report it as being declared by the inclusion projection).
        ASSERT_EQUALS(graph->getDeclaringStage(last, "a.b"), stages[1]);
        // 'a.c' excluded by projection.
        ASSERT_EQUALS(graph->getDeclaringStage(last, "a.c"), stages[1]);
    });
}

TEST_F(PipelineDependencyGraphTest, ComplexPathsMultiple) {
    setPipeline(
        "[{$set: { a: 1, b: 1, 'c.c': 1 }},"
        "{$set: { 'a.a': 1, 'a.b': 1, 'b.b.b': 1, c: 1 }},"
        "{$set: { 'a.b.a': 1, 'b.b': 1, 'b.a': 1 }}]");

    runTest([&] {
        // Lookup from the end of the pipeline.
        DocumentSource* ds = nullptr;
        ASSERT_EQUALS(graph->getDeclaringStage(ds, "a"), stages[2]);
        ASSERT_EQUALS(graph->getDeclaringStage(ds, "b"), stages[2]);
        ASSERT_EQUALS(graph->getDeclaringStage(ds, "c"), stages[1]);
        ASSERT_EQUALS(graph->getDeclaringStage(ds, "c.c"), stages[1]);
        ASSERT_EQUALS(graph->getDeclaringStage(ds, "a.b"), stages[2]);
        ASSERT_EQUALS(graph->getDeclaringStage(ds, "a.a"), stages[1]);
        ASSERT_EQUALS(graph->getDeclaringStage(ds, "b.b.b"), stages[2]);
        ASSERT_EQUALS(graph->getDeclaringStage(ds, "b.b"), stages[2]);
        ASSERT_EQUALS(graph->getDeclaringStage(ds, "b.a"), stages[2]);
        ASSERT_EQUALS(graph->getDeclaringStage(ds, "a.b.a"), stages[2]);
    });
}

TEST_F(PipelineDependencyGraphTest, CanPathBeArrayWithoutArraynessInfo) {
    setPipeline("[{$set: { a: '$b' }}]");
    runTest([&] {
        // Lookup from the end of the pipeline.
        auto* ds = stages.back().get();
        ASSERT_TRUE(graph->canPathBeArray(ds, "b"));
        ASSERT_TRUE(graph->canPathBeArray(ds, "a"));
        ASSERT_TRUE(graph->canPathBeArray(ds, "a.a"));
        ASSERT_TRUE(graph->canPathBeArray(ds, "a.unknown"));
        ASSERT_TRUE(graph->canPathBeArray(ds, "unknown"));
    });
}

TEST_F(PipelineDependencyGraphTest, CanPathBeArrayWhenMissing) {
    setPipeline("[{$replaceWith: {}}]");
    runTest([&] {
        // Lookup from the end of the pipeline.
        auto* ds = stages.back().get();
        // TODO(SERVER-119392): These should be trivially non-array with constant propagation.
        ASSERT_TRUE(graph->canPathBeArray(ds, "a"));
        ASSERT_TRUE(graph->canPathBeArray(ds, "a.a"));
    });
}

TEST_F(PipelineDependencyGraphTest, CanPathBeArrayWhenKnown) {
    setPipeline("[{$set: { 'a.b': 1, 'a.c': [], 'a.d': {$add: [2, 2]} }}]");
    runTest([&] {
        // Lookup from the end of the pipeline.
        auto* ds = stages.back().get();
        ASSERT_TRUE(graph->canPathBeArray(ds, "a"));
        ASSERT_TRUE(graph->canPathBeArray(ds, "a.a"));
        ASSERT_TRUE(graph->canPathBeArray(ds, "a.b"));
        ASSERT_TRUE(graph->canPathBeArray(ds, "a.c"));
        ASSERT_TRUE(graph->canPathBeArray(ds, "a.d"));
        ASSERT_TRUE(graph->canPathBeArray(ds, "a.unknown"));
    });
}

TEST_F(PipelineDependencyGraphTest, CanRenamedPathBeArray) {
    pathArrayness->addPath("b", {}, true);
    setPipeline("[{$set: { a: '$b' }}]");
    runTest([&] {
        // Lookup from the end of the pipeline.
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "b"));
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "a"));
        ASSERT_TRUE(graph->canPathBeArray(nullptr, "a.a"));
        ASSERT_TRUE(graph->canPathBeArray(nullptr, "a.unknown"));
        ASSERT_TRUE(graph->canPathBeArray(nullptr, "unknown"));
    });
}

TEST_F(PipelineDependencyGraphTest, CanRenamedChainBeArray) {
    pathArrayness->addPath("a.x", {}, true);
    setPipeline(
        "[{$set: { b: '$a' }}, "
        " {$set: { c: '$b' }}, "
        " {$set: { d: '$c' }}]");
    runTest([&] {
        // Lookup from the end of the pipeline.
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "a"));
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "b"));
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "c"));
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "d"));
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "d.x"));
    });
}

TEST_F(PipelineDependencyGraphTest, CanRenamedWithProjectBeArray) {
    pathArrayness->addPath("a", {}, true);
    setPipeline("[{$set: {}}, {$group: { _id: '$a' }}]");
    runTest([&] {
        // Lookup from the end of the pipeline.
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "_id"));
        // 'a' is not defined after the $group but may exist as an implicitly created field (e.g.
        // accumulator), so we conservatively assume it can be an array.
        ASSERT_TRUE(graph->canPathBeArray(nullptr, "a"));
        ASSERT_FALSE(graph->canPathBeArray(stages.front().get(), "a"));
    });
}

TEST_F(PipelineDependencyGraphTest, CanGroupAccumulatorFieldBeArray) {
    pathArrayness->addPath("c", {}, true);
    setPipeline("[{$set: {}}, {$group: { _id: '$a', b: { $avg: '$c' } }}, {$match: {b: 1}}]");
    runTest([&] {
        auto* ds = stages.back().get();
        // '_id' is a rename from 'a' which has no arrayness info, so it can be array.
        ASSERT_TRUE(graph->canPathBeArray(ds, "_id"));
        // 'b' is an accumulator field created by $group: it exists but is not reported in
        // getModifiedPaths(). The graph attributes it to the $group scope.
        ASSERT_TRUE(graph->canPathBeArray(ds, "b"));
        // getModifiedPaths() for $group doesn't report accumulated fields so we can't assume that
        // unknown fields are definitely missing.
        ASSERT_TRUE(graph->canPathBeArray(ds, "unknown"));
    });
}

TEST_F(PipelineDependencyGraphTest, CanMissingBaseFieldBeArray) {
    setPipeline("[{$project: { a: 1 }}, {$set: { 'b.c': 1 }}, {$match: {b: 1}}]");
    runTest([&] {
        auto* ds = stages.back().get();
        // 'b' is missing after the inclusion projection.
        ASSERT_FALSE(graph->canPathBeArray(ds, "b"));
        ASSERT_FALSE(graph->canPathBeArray(ds, "b.c"));
    });
}

TEST_F(PipelineDependencyGraphTest, CanDottedRenamedPathsBeArray) {
    pathArrayness->addPath("x.y.z", {}, true);
    setPipeline("[{$set: { a: '$x', b: '$x.y', 'c.d': '$x', 'e.f': '$x.y.z', 'g': '$x.y.z' }}]");
    runTest([&] {
        // Lookup from the end of the pipeline.
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "a"));
        ASSERT_TRUE(graph->canPathBeArray(nullptr, "a.unknown"));
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "b"));
        ASSERT_TRUE(graph->canPathBeArray(nullptr, "b.unknown"));
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "b.z"));
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "g"));
        ASSERT_TRUE(graph->canPathBeArray(nullptr, "c"));
        ASSERT_TRUE(graph->canPathBeArray(nullptr, "c.d"));
        ASSERT_TRUE(graph->canPathBeArray(nullptr, "c.unknown"));
        ASSERT_TRUE(graph->canPathBeArray(nullptr, "e"));
        ASSERT_TRUE(graph->canPathBeArray(nullptr, "e.f"));
        ASSERT_TRUE(graph->canPathBeArray(nullptr, "e.unknown"));
    });
}

TEST_F(PipelineDependencyGraphTest, CanRedefinedBaseFieldBeArray) {
    setPipeline(
        "[{$set: { a: 1, b: 1, 'c.c': 1 }},"
        "{$set: { 'a.a': 1, 'a.b': 1, 'b.b.b': 1, c: 1 }},"
        "{$set: { 'a.b.a': 1, 'b.b': 1, 'b.a': 1 }}]");
    runTest([&] {
        // Lookup from the end of the pipeline.
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "a"));
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "a.a"));
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "a.b"));
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "a.b.a"));
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "b"));
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "b.b"));
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "b.a"));
        // TODO(SERVER-119392): This should pass.
        // ASSERT_FALSE(graph->canPathBeArray(nullptr, "b.b.b"));
    });
}

TEST_F(PipelineDependencyGraphTest, CanBeArraysLooksThroughDottedRenamedPaths) {
    pathArrayness->addPath("x.y.z", {}, true);
    setPipeline("[{$set: { a: '$x', b: '$x.y', 'c.d': '$x', 'e.f': '$x.y.z' }}]");
    runTest([&] {
        // Lookup from the end of the pipeline.
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "a.y.z"));
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "a.y"));
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "b.z"));
        ASSERT_TRUE(graph->canPathBeArray(nullptr, "a.unknown"));
    });
}

TEST_F(PipelineDependencyGraphTest, CanAliasChainWithDottedRenameBeArray) {
    pathArrayness->addPath("a.x.y", {}, true);
    setPipeline(
        "[{$set: { b: '$a' }}, "
        " {$set: { c: '$b.x' }}]");
    runTest([&] {
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "c"));
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "c.y"));
    });
}

TEST_F(PipelineDependencyGraphTest, CanOverwrittenAliasBeArray) {
    pathArrayness->addPath("x", {}, true);
    setPipeline(
        "[{$set: { a: '$x' }}, "
        " {$set: { a: 1 }}]");
    runTest([&] {
        auto* ds = stages.back().get();
        // After overwrite, 'a' is 1 (constant), not an alias.
        ASSERT_FALSE(graph->canPathBeArray(ds, "a"));
        // a.y is shadowed by constant, no alias.
        ASSERT_TRUE(graph->canPathBeArray(ds, "a.y"));
    });
}

TEST_F(PipelineDependencyGraphTest, CanBaseCollectionFieldInPrefixBeArray) {
    pathArrayness->addPath("a.b", {}, true);
    setPipeline("[{$set: { 'a.c': '$a.b' }}]");
    runTest([&] {
        // "a" is a collection field known to be non-array from path arrayness.
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "a"));
        // "a.c" is renamed from "a.b" which is non-array, and prefix "a" is also non-array.
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "a.c"));
    });
}

TEST_F(PipelineDependencyGraphTest, DottedNewPathBaseFieldMetadataUsesFullCollectionPath) {
    pathArrayness->addPath("a.b.c", {}, true);
    setPipeline("[{$set: { 'a.b.c': 1 }}]");
    runTest([&] {
        auto* ds = stages.back().get();
        ASSERT_FALSE(graph->canPathBeArray(ds, "a"));
        // The intermediate "b" must check "a.b" (indexed, non-array), not just "b" (unknown).
        ASSERT_FALSE(graph->canPathBeArray(ds, "a.b"));
    });
}

TEST_F(PipelineDependencyGraphTest, CanPathBeArrayChecksShadowingFieldArrayness) {
    pathArrayness->addPath("x.y", {}, true);
    // c can be array (a is not indexed). c.y is shadowed by c: even though c's alias
    // resolves to x.y (non-array), c itself can be array so c.y must be too.
    setPipeline(
        "[{$set: { 'a.b': '$x' }}, "
        " {$set: { c: '$a.b' }}]");
    runTest([&] {
        auto* ds = stages.back().get();
        ASSERT_TRUE(graph->canPathBeArray(ds, "c.y"));
    });
}

TEST_F(PipelineDependencyGraphTest, ShadowedRenameChecksShadowingFieldArrayness) {
    pathArrayness->addPath("x.y", {}, true);
    // Same setup: c can be array (a is not indexed). d renames c.y, so d can be array too.
    setPipeline(
        "[{$set: { 'a.b': '$x' }}, "
        " {$set: { c: '$a.b' }}, "
        " {$set: { d: '$c.y' }}]");
    runTest([&] {
        auto* ds = stages.back().get();
        ASSERT_TRUE(graph->canPathBeArray(ds, "d"));
    });
}

TEST_F(PipelineDependencyGraphTest, ReplaceRootAttributesAllFields) {
    setPipeline(
        "[{$set: { a: 1 }},"
        "{$replaceRoot: { newRoot: '$a' }},"
        "{$set: { c: 1 }}]");

    runTest([&] {
        // All pre-existing fields are attributed to $replaceRoot.
        ASSERT_EQUALS(graph->getDeclaringStage(nullptr, "a"), stages[1]);
        ASSERT_EQUALS(graph->getDeclaringStage(nullptr, "b"), stages[1]);
        // 'c' set after $replaceRoot.
        ASSERT_EQUALS(graph->getDeclaringStage(nullptr, "c"), stages[2]);
    });
}


TEST_F(PipelineDependencyGraphTest, ReplaceRootShadowsPriorDefinitions) {
    setPipeline(
        "[{$set: { a: 1, b: 1 }},"
        "{$replaceRoot: { newRoot: {} }},"
        "{$match: { a: 1, b: 1 }}]");

    runTest([&] {
        auto* last = stages.back().get();
        // Both fields are attributed to $replaceRoot, not the $set.
        ASSERT_EQUALS(graph->getDeclaringStage(last, "a"), stages[1]);
        ASSERT_EQUALS(graph->getDeclaringStage(last, "b"), stages[1]);
    });
}

TEST_F(PipelineDependencyGraphTest, ReplaceRootThenSetThenLookup) {
    setPipeline(
        "[{$replaceRoot: { newRoot: '$x' }},"
        "{$set: { a: 1 }},"
        "{$match: { a: 1, b: 1 }}]");

    runTest([&] {
        auto* last = stages.back().get();
        // 'a' redefined after $replaceRoot.
        ASSERT_EQUALS(graph->getDeclaringStage(last, "a"), stages[1]);
        // 'b' not redefined — still attributed to $replaceRoot.
        ASSERT_EQUALS(graph->getDeclaringStage(last, "b"), stages[0]);
    });
}

TEST_F(PipelineDependencyGraphTest, ChainedReplaceRoots) {
    setPipeline(
        "[{$set: { a: 1 }},"
        "{$replaceRoot: { newRoot: '$a' }},"
        "{$replaceRoot: { newRoot: '$b' }},"
        "{$match: { a: 1 }}]");

    runTest([&] {
        auto* last = stages.back().get();
        // Second $replaceRoot is the last exhaustive stage.
        ASSERT_EQUALS(graph->getDeclaringStage(last, "a"), stages[2]);
        ASSERT_EQUALS(graph->getDeclaringStage(last, "b"), stages[2]);
    });
}

TEST_F(PipelineDependencyGraphTest, GroupSimpleKey) {
    setPipeline(
        "[{$set: { x: 1 }},"
        "{$group: { _id: '$x', count: { $sum: 1 } }},"
        "{$match: { _id: 1, count: 1, a: 1 }}]");

    runTest([&] {
        auto* last = stages.back().get();
        // _id declared by $group.
        ASSERT_EQUALS(graph->getDeclaringStage(last, "_id"), stages[1]);
        // 'count' is also declared by $group.
        ASSERT_EQUALS(graph->getDeclaringStage(last, "count"), stages[1]);
        // 'a' from the base document is made missing by $group.
        ASSERT_EQUALS(graph->getDeclaringStage(last, "a"), stages[1]);
        // 'x' is also made missing by $group.
        ASSERT_EQUALS(graph->getDeclaringStage(last, "x"), stages[1]);
    });
}

TEST_F(PipelineDependencyGraphTest, GroupKeyFromBaseDocument) {
    setPipeline(
        "[{$group: { _id: '$x' }},"
        "{$match: { _id: 1 }}]");

    runTest([&] {
        auto* last = stages.back().get();
        // _id declared by $group.
        ASSERT_EQUALS(graph->getDeclaringStage(last, "_id"), stages[0]);
    });
}

TEST_F(PipelineDependencyGraphTest, GroupCompoundKey) {
    setPipeline(
        "[{$set: { x: 1, y: 1 }},"
        "{$group: { _id: { a: '$x', b: '$y' } }},"
        "{$match: { '_id.a': 1, '_id.b': 1, '_id.c': 1 }}]");

    runTest([&] {
        auto* last = stages.back().get();
        // _id.a declared by $group.
        ASSERT_EQUALS(graph->getDeclaringStage(last, "_id.a"), stages[1]);
        // _id.b declared by $group.
        ASSERT_EQUALS(graph->getDeclaringStage(last, "_id.b"), stages[1]);
        // _id declared by group.
        ASSERT_EQUALS(graph->getDeclaringStage(last, "_id"), stages[1]);
        // _id.c is not a group key field — attributed to $group via missing sentinel.
        ASSERT_EQUALS(graph->getDeclaringStage(last, "_id.c"), stages[1]);
    });
}

TEST_F(PipelineDependencyGraphTest, GroupDottedKeyNoRename) {
    setPipeline(
        "[{$set: { x: 1 }},"
        "{$group: { _id: '$x.y' }},"
        "{$match: { _id: 1 }}]");

    runTest([&] {
        auto* last = stages.back().get();
        // _id declared by $group.
        ASSERT_EQUALS(graph->getDeclaringStage(last, "_id"), stages[1]);
        // Everything else is made missing by $group.
        ASSERT_EQUALS(graph->getDeclaringStage(last, "x"), stages[1]);
        ASSERT_EQUALS(graph->getDeclaringStage(last, "x.y"), stages[1]);
    });
}

TEST_F(PipelineDependencyGraphTest, GroupNullKey) {
    setPipeline(
        "[{$group: { _id: null, total: { $sum: 1 } }},"
        "{$set: { a: 1 }},"
        "{$match: { _id: 1, total: 1, a: 1, b: 1 }}]");

    runTest([&] {
        auto* last = stages.back().get();
        // _id is declared by $group.
        ASSERT_EQUALS(graph->getDeclaringStage(last, "_id"), stages[0]);
        // 'total' is declared by $group.
        ASSERT_EQUALS(graph->getDeclaringStage(last, "total"), stages[0]);
        // 'a' set after $group.
        ASSERT_EQUALS(graph->getDeclaringStage(last, "a"), stages[1]);
        // 'b' is made missing by $group.
        ASSERT_EQUALS(graph->getDeclaringStage(last, "b"), stages[0]);
    });
}

TEST_F(PipelineDependencyGraphTest, GroupThenInclusion) {
    setPipeline(
        "[{$group: { _id: {foo: {bar: 1}}, count: { $sum: 1 } }},"
        "{$project: { _id: 1 }},"
        "{$match: { _id: 1, count: 1 }}]");

    runTest([&] {
        auto* last = stages.back().get();
        // _id preserved through inclusion, most recently declared by $group.
        ASSERT_EQUALS(graph->getDeclaringStage(last, "_id"), stages[0]);
        // 'count' excluded by inclusion projection.
        ASSERT_EQUALS(graph->getDeclaringStage(last, "count"), stages[1]);
        // Any arbitrary field last excluded by the inclusion projection.
        ASSERT_EQUALS(graph->getDeclaringStage(last, "foo"), stages[1]);
    });
}

TEST_F(PipelineDependencyGraphTest, SetThenGroupThenSetThenMatch) {
    setPipeline(
        "[{$set: { x: 1 }},"
        "{$group: { _id: '$x' }},"
        "{$set: { a: 1 }},"
        "{$match: { _id: 1, a: 1, b: 1 }}]");

    runTest([&] {
        auto* last = stages.back().get();
        // _id declared by $group.
        ASSERT_EQUALS(graph->getDeclaringStage(last, "_id"), stages[1]);
        // 'a' set after $group.
        ASSERT_EQUALS(graph->getDeclaringStage(last, "a"), stages[2]);
        // 'b' made missing by $group.
        ASSERT_EQUALS(graph->getDeclaringStage(last, "b"), stages[1]);
    });
}

}  // namespace
}  // namespace mongo::pipeline::dependency_graph
