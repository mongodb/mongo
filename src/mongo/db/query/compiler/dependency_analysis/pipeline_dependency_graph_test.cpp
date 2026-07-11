// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/query/compiler/dependency_analysis/pipeline_dependency_graph.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/document_source_facet.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/document_source_unwind.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/optimization/optimize.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/db/query/compiler/dependency_analysis/pipeline_dependency_graph_test_util.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/tenant_id.h"
#include "mongo/dbtests/dbtests.h"  // IWYU pragma: keep
#include "mongo/unittest/unittest.h"

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/strings/str_split.h"
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

using namespace std::literals::string_view_literals;

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

using namespace std::literals::string_view_literals;
namespace mongo::pipeline::dependency_graph {
namespace {

class PipelineDependencyGraphTest : public unittest::Test {
protected:
    void SetUp() override {
        pathArrayness = std::make_shared<PathArrayness>();
    }

    void setPipeline(std::unique_ptr<Pipeline> p) {
        pipeline = std::move(p);
        pipeline->getContext()->setPathArraynessForNss(pipeline->getContext()->getNamespaceString(),
                                                       pathArrayness);
        stages.assign(pipeline->getSources().begin(), pipeline->getSources().end());
        canPathBeArray = [this](std::string_view path) -> bool {
            return pipeline->getContext()->canPathBeArrayForNss(
                FieldRef(path), pipeline->getContext()->getNamespaceString());
        };
        graph = std::make_unique<DependencyGraph>(pipeline->getSources(), canPathBeArray);
    }

    void setPipeline(const std::string& array) {
        setPipeline(parsePipeline(array));
    }

    void setOptimizedPipeline(const std::string& array) {
        auto p = parsePipeline(array);
        pipeline_optimization::optimizePipeline(*p);
        setPipeline(std::move(p));
    }

    /**
     * Asserts that 'graph->getDeadFields()' produces the given (stage, path) pairs.
     */
    void assertDeadFieldsEq(std::vector<std::pair<const DocumentSource*, std::string>> expected) {
        auto dead = graph->getDeadFields();
        std::vector<std::pair<const DocumentSource*, std::string>> actual;
        actual.reserve(dead.size());
        for (const auto& df : dead) {
            actual.emplace_back(df.stage.get(), df.path.fullPath());
        }
        std::sort(actual.begin(), actual.end());
        std::sort(expected.begin(), expected.end());
        ASSERT_EQ(actual, expected);
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

        const std::string_view kDBName = "test";
        const NamespaceString kTestNss =
            NamespaceString::createNamespaceString_forTest(kDBName, "collection");

        auto additionalNs = std::vector<NamespaceString>(
            {NamespaceString::createNamespaceString_forTest("test.coll_b"sv),
             NamespaceString::createNamespaceString_forTest("test.coll_c"sv),
             NamespaceString::createNamespaceString_forTest("test2.coll_d"sv)});

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
        ASSERT_EQUALS(
            graph->getPrevModifyingStageIncludingSubpipelines_forTest(nullptr, "docs").srcStages,
            stages);
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "docs"), stages[0]);

        // 'docs.b_ssn' should resolve across the $lookup into the sub-pipeline's $set stage.
        auto* subGraph = graph->getSubpipelineGraph(stages[0].get());
        ASSERT_NOT_EQUALS(subGraph, nullptr);
        auto result =
            graph->getPrevModifyingStageIncludingSubpipelines_forTest(nullptr, "docs.b_ssn");

        // The declaring stage should come from the sub-pipeline (the $set).
        auto subDeclaringStage = subGraph->getPrevModifyingStage(nullptr, "b_ssn");
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
        ASSERT_EQUALS(
            graph->getPrevModifyingStageIncludingSubpipelines_forTest(nullptr, "docs.x").srcStages,
            stages);
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "docs.x"), stages[0]);

        // 'docs.x.b_ssn' should resolve across the $lookup into the sub-pipeline's $set stage.
        auto* subGraph = graph->getSubpipelineGraph(stages[0].get());
        ASSERT_NOT_EQUALS(subGraph, nullptr);
        auto result =
            graph->getPrevModifyingStageIncludingSubpipelines_forTest(nullptr, "docs.x.b_ssn");

        // The declaring stage should come from the sub-pipeline (the $set).
        auto subDeclaringStage = subGraph->getPrevModifyingStage(nullptr, "b_ssn");
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
        // so getPrevModifyingStage() should return nullptr (comes from the sub-pipeline's input).
        auto result =
            graph->getPrevModifyingStageIncludingSubpipelines_forTest(nullptr, "docs.unknown");
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
        // so getPrevModifyingStage should return nullptr (comes from the sub-pipeline's input).
        auto unknownResult = graph->getPrevModifyingStageIncludingSubpipelines_forTest(
            nullptr, "docs.rocks.unknown");
        ASSERT_EQUALS(unknownResult.srcStages.back(), nullptr);
        ASSERT_TRUE(unknownResult.fromSubpipeline);

        // 'docs.rocks.c_ssn' is declared by the innermost $set stage.
        auto knownResult =
            graph->getPrevModifyingStageIncludingSubpipelines_forTest(nullptr, "docs.rocks.c_ssn");
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
            graph->getPrevModifyingStageIncludingSubpipelines_forTest(nullptr, "docs.b_ssn");
        ASSERT_EQUALS(resultSubPipelines.srcStages.back(), nullptr);
        ASSERT_TRUE(resultSubPipelines.fromSubpipeline);

        auto resultMainPipeline = graph->getPrevModifyingStage(nullptr, "docs.b_ssn");
        ASSERT_EQUALS(resultMainPipeline, stages[0]);
    });
}

TEST_F(PipelineDependencyGraphTest, SubPipelineCanPathBeArrayDelegatesToSubGraph) {
    setOptimizedPipeline(R"([
        {$lookup: {
            from: "coll_b",
            as: "docs",
            pipeline: [{$set: {b_ssn: 42}}]
        }},
        {$unwind: "$docs"}
    ])");

    runTest([&] {
        // 'docs.b_ssn' is set to a constant integer, which cannot be an array.
        auto* subGraph = graph->getSubpipelineGraph(stages[0].get());
        ASSERT_NOT_EQUALS(subGraph, nullptr);
        ASSERT_FALSE(subGraph->canPathBeArray(nullptr, "b_ssn"));
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "docs.b_ssn"));
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
        auto subDeclStage = subGraph->getPrevModifyingStage(nullptr, "a");
        auto result =
            graph->getPrevModifyingStageIncludingSubpipelines_forTest(matchStage, "docs.a");
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

        ASSERT_EQUALS(graph->getPrevModifyingStageIncludingSubpipelines_forTest(nullptr, "docs")
                          .srcStages.back(),
                      stages[0]);

        // 'docs.b_ssn' crosses into the sub-pipeline. The inclusion projection preserves
        // b_ssn from the sub-pipeline's input, so it originates from the base collection.
        auto result =
            graph->getPrevModifyingStageIncludingSubpipelines_forTest(nullptr, "docs.b_ssn");
        ASSERT_EQUALS(result.srcStages.back(), nullptr);
        ASSERT_TRUE(result.fromSubpipeline);

        // 'docs.other' is excluded by the inclusion projection, so it's declared by the $project
        // (deleted).
        auto otherResult =
            graph->getPrevModifyingStageIncludingSubpipelines_forTest(nullptr, "docs.other");
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
            graph->getPrevModifyingStageIncludingSubpipelines_forTest(stages[2].get(), "s")
                .srcStages.back(),
            stages[1]);

        // Within the sub-pipeline, 's' is declared by the sub-pipeline's $addFields.
        auto subDeclStage =
            subGraph->getPrevModifyingStageIncludingSubpipelines_forTest(nullptr, "s")
                .srcStages.back();
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

TEST_F(PipelineDependencyGraphTest, LookupWithoutAbsorbedUnwindAsFieldIsArray) {
    // Without an absorbed $unwind, the 'as' field is an array of lookup results.
    setPipeline(R"([
        {$lookup: {
            from: "coll_b",
            as: "docs",
            pipeline: []
        }}
    ])");

    runTest([&] { ASSERT_TRUE(graph->canPathBeArray(nullptr, "docs")); });
}

TEST_F(PipelineDependencyGraphTest, LookupWithAbsorbedUnwindAsFieldIsNotArray) {
    // After $lookup+$unwind, the 'as' field holds a single document per output row, not an array.
    setOptimizedPipeline(R"([
        {$lookup: {
            from: "coll_b",
            as: "docs",
            pipeline: []
        }},
        {$unwind: "$docs"}
    ])");

    runTest([&] { ASSERT_FALSE(graph->canPathBeArray(nullptr, "docs")); });
}

TEST_F(PipelineDependencyGraphTest, LookupWithAbsorbedUnwindArrayIndexIsNotArray) {
    // The includeArrayIndex field is a numeric position, never an array.
    setOptimizedPipeline(R"([
        {$lookup: {
            from: "coll_b",
            as: "docs",
            pipeline: []
        }},
        {$unwind: {path: "$docs", includeArrayIndex: "docsIdx"}}
    ])");

    runTest([&] {
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "docs"));
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "docsIdx"));
    });
}

TEST_F(PipelineDependencyGraphTest, SubPipelineCanPathBeArrayUnknownSubField) {
    setOptimizedPipeline(R"([
        {$lookup: {
            from: "coll_b",
            as: "docs",
            pipeline: [{$set: {b_ssn: 2}}]
        }},
        {$unwind: "$docs"}
    ])");

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
        auto result = graph->getPrevModifyingStageIncludingSubpipelines_forTest(last, "docs.b_ssn");
        auto* subGraph = graph->getSubpipelineGraph(stages[0].get());
        ASSERT_NOT_EQUALS(subGraph, nullptr);
        auto subDeclaringStage = subGraph->getPrevModifyingStage(nullptr, "b_ssn");
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
        auto declStage = graph->getPrevModifyingStageIncludingSubpipelines_forTest(matchStage, "x")
                             .srcStages.back();
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
        auto declStage = graph->getPrevModifyingStageIncludingSubpipelines_forTest(matchStage, "y")
                             .srcStages.back();
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
        // Although getPrevModifyingStage() for the main pipeline returns $unionWith,
        // we can independently query the sub-pipeline graph.
        auto* subGraph = graph->getSubpipelineGraph(stages[0].get());
        ASSERT_NOT_EQUALS(subGraph, nullptr);

        auto subDeclStage =
            subGraph->getPrevModifyingStageIncludingSubpipelines_forTest(nullptr, "x")
                .srcStages.back();
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
        auto declStage = graph->getPrevModifyingStageIncludingSubpipelines_forTest(matchStage, "x")
                             .srcStages.back();
        ASSERT_EQUALS(declStage, stages[1]);

        // "y" not set by the outer $set, still attributed to $unionWith.
        auto declY = graph->getPrevModifyingStageIncludingSubpipelines_forTest(matchStage, "y")
                         .srcStages.back();
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

TEST_F(PipelineDependencyGraphTest, SubPipelineUsesSecondaryCollPathArrayness) {
    // The sub-pipeline DependencyGraph should query the PathArrayness of the subpipeline.
    setOptimizedPipeline(R"([
        {$lookup: {
            from: "coll_b",
            as: "docs",
            pipeline: [{$match: {b_foo: 1}}]
        }},
        {$unwind: "$docs"}
    ])");

    // Get the sub-pipeline's ExpressionContext (the $lookup's _fromExpCtx). The sub-pipeline
    // has one stage ($match) whose ExpCtx is the from-collection context.
    auto* subPipeline = stages[0]->getSubPipeline();
    ASSERT_NOT_EQUALS(subPipeline, nullptr);
    ASSERT_FALSE(subPipeline->empty());

    // Set up a PathArrayness for coll_b that marks 'b_non_array' as provably not an array.
    auto secondaryPathArrayness = std::make_shared<PathArrayness>();
    secondaryPathArrayness->addPath(
        FieldPath("b_non_array"), {} /*multikeyPath*/, true /*isFullRebuild*/);

    // Inject the main ExpCtx using the sub-pipeline NSS as the key.
    auto subExpCtx = subPipeline->front()->getExpCtx();
    pipeline->getContext()->setPathArraynessForNss(subExpCtx->getNamespaceString(),
                                                   secondaryPathArrayness);

    runTest([&] {
        // This is refering to 'b_non_array' originating from the base collection.
        ASSERT_TRUE(graph->canPathBeArray(nullptr, "b_non_array"));

        // This refers to the 'docs.b_non_array' originating from the secondary collection which
        // cannot be an array.
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "docs.b_non_array"));

        // An unindexed field not in the PathArrayness trie is conservatively true.
        ASSERT_TRUE(graph->canPathBeArray(nullptr, "b_unknown"));
    });
}

TEST_F(PipelineDependencyGraphTest, SubPipelineEmptyCanPathBeArray) {
    // $lookup with a truly empty sub-pipeline. Inject PathArrayness for the secondary
    // collection to verify it propagates even when there are no sub-pipeline stages.
    setOptimizedPipeline(R"([
        {$lookup: {
            from: "coll_b",
            localField: "foo",
            foreignField: "b_foo",
            as: "docs"
        }},
        {$unwind: "$docs"}
    ])");

    // Set up a PathArrayness for coll_b that marks 'b_non_array' as provably not an array.
    auto secondaryPathArrayness = std::make_shared<PathArrayness>();
    secondaryPathArrayness->addPath(
        FieldPath("b_non_array"), {} /*multikeyPath*/, true /*isFullRebuild*/);

    // Use getSubpipelineExpCtx() to obtain the from-collection's ExpCtx
    // even though the sub-pipeline is empty.
    auto subExpCtx = stages[0]->getSubpipelineExpCtx();
    ASSERT_NOT_EQUALS(subExpCtx, nullptr);
    // Inject the main ExpCtx using the sub-pipeline NSS as the key.
    pipeline->getContext()->setPathArraynessForNss(subExpCtx->getNamespaceString(),
                                                   secondaryPathArrayness);

    runTest([&] {
        // The sub-pipeline graph should exist even though the pipeline is empty.
        auto* subGraph = graph->getSubpipelineGraph(stages[0].get());
        ASSERT_NOT_EQUALS(subGraph, nullptr);

        // 'docs.b_non_array' crosses into the empty sub-pipeline and resolves via
        // coll_b's PathArrayness — provably not an array.
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "docs.b_non_array"));

        // An unindexed field in coll_b is conservatively true.
        ASSERT_TRUE(graph->canPathBeArray(nullptr, "docs.b_unknown"));

        // Base-collection field is unrelated to the sub-pipeline — conservatively true.
        ASSERT_TRUE(graph->canPathBeArray(nullptr, "base_field"));
    });
}

TEST_F(PipelineDependencyGraphTest, SimpleCase) {
    setPipeline(
        "[{$set: { a: 'foo' }},"
        "{$match: { a: 'foo' }}]");

    runTest([&] { ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "a"), stages[0]); });
}

TEST_F(PipelineDependencyGraphTest, Shadowing) {
    setPipeline(
        "[{$set: { a: 'foo' }},"
        "{$set: { b: 'bar' }},"
        "{$set: { a: 'baz' }},"
        "{$match: { a: 'baz' }}]");

    runTest([&] { ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "a"), stages[2]); });
}

TEST_F(PipelineDependencyGraphTest, Shadowing2) {
    setPipeline(
        "[{$set: { a: 'foo' }},"
        "{$set: { b: 'bar' }},"
        "{$match: { a: 'baz' }},"
        "{$set: { a: 'baz' }}]");

    runTest([&] {
        // Expected stage is the first one since $match comes before the last $set
        ASSERT_EQUALS(graph->getPrevModifyingStage(stages[2].get(), "a"), stages[0]);
    });
}

TEST_F(PipelineDependencyGraphTest, UnknownField) {
    setPipeline("[{$match: { a: 'baz' }}]");

    runTest([&] {
        // Return nullptr to indicate it comes from document.
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "a"), nullptr);
    });
}

TEST_F(PipelineDependencyGraphTest, UnknownComplex) {
    setPipeline("[{$match: { 'a.b': 'baz' }}]");

    runTest([&] {
        // Return nullptr to indicate it comes from document.
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "a.b"), nullptr);
    });
}

TEST_F(PipelineDependencyGraphTest, UnknownComplexPrefix) {
    setPipeline("[{$set: { 'a.b': 'baz' }}]");

    runTest([&] {
        // Return stages[0] to indicate it was modified by setting the prefix.
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "a.b.c"), stages[0]);
    });
}

TEST_F(PipelineDependencyGraphTest, UnknownFieldAfterExhaustive) {
    setPipeline(
        "[{$replaceRoot: { newRoot: {} }},"
        "{$set: { b: 'bar' }},"
        "{$match: { a: 'baz' }}]");

    runTest([&] {
        // Return stage[0] to indicate it would be modified by $replaceRoot.
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "a"), stages[0]);
    });
}

TEST_F(PipelineDependencyGraphTest, UnknownComplexAfterExhaustive) {
    setPipeline(
        "[{$replaceRoot: { newRoot: {} }},"
        "{$set: { b: 'bar' }},"
        "{$match: { 'a.b': 'baz' }}]");

    runTest([&] {
        // Return stage[0] to indicate it would be modified by $replaceRoot.
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "a.b"), stages[0]);
    });
}

TEST_F(PipelineDependencyGraphTest, MatchMultiple) {
    setPipeline(
        "[{$set: { a: 'foo' }},"
        "{$set: { b: 'bar' }},"
        "{$match: { a: 'foo', b: 'bar' }}]");

    runTest([&] {
        // For field 'a', expect first stage.
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "a"), stages[0]);
        // For field 'b', expect second stage.
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "b"), stages[1]);
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
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "a"), stages[1]);
        // For field 'b', expect stage 3
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "b"), stages[2]);
    });
}

TEST_F(PipelineDependencyGraphTest, MatchMultipleWithPartialShadowing) {
    setPipeline(
        "[{$set: { a: 'foo', b: 'bar' }},"
        "{$set: { b: 'foo2' }},"
        "{$match: { a: 'foo', b: 'foo2' }}]");

    runTest([&] {
        // For field 'a', expect stage 1 (no shadowing for 'a')
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "a"), stages[0]);

        // For field 'b', expect stage 2 (shadowing stage)
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "b"), stages[1]);
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
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "a"), stages[2]);
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
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "a"), stages[3]);
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
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "a"), stages[2]);
    });
}

TEST_F(PipelineDependencyGraphTest, ComplexPathShadowing) {
    setPipeline(
        "[{$set: { d: 1 }},"
        "{$set: { 'd.b.c': 1 }},"
        "{$set: { 'd.b': 1, 'd.a': 1 }}]");

    runTest([&] {
        // Lookup from the end of the pipeline.
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "d.b.c"), stages.back());
    });
}

TEST_F(PipelineDependencyGraphTest, ComplexPathInclusionProjection) {
    setPipeline(
        "[{$set: { 'a.b.c': 1 }},"
        "{$project: { 'a.b': 1 }},"
        "{$set: { 'c': 1 }}]");

    runTest([&] {
        ASSERT_EQUALS(graph->getPrevModifyingStage(stages.back().get(), "a.b.c"), stages[0]);
    });
}

TEST_F(PipelineDependencyGraphTest, ComplexPathInclusionProjectionNonExistent) {
    setPipeline(
        "[{$set: { 'a.b.c': 1 }},"
        "{$project: { 'a.b.d': 1 }},"
        "{$set: { 'c': 1 }}]");

    runTest([&] {
        // The inclusion modified a (filtered its subfields).
        ASSERT_EQUALS(graph->getPrevModifyingStage(stages.back().get(), "a"), stages[1]);
        // The inclusion modified a.b (filtered its subfields).
        ASSERT_EQUALS(graph->getPrevModifyingStage(stages.back().get(), "a.b"), stages[1]);
        // Excluded by the inclusion.
        ASSERT_EQUALS(graph->getPrevModifyingStage(stages.back().get(), "a.b.c"), stages[1]);
        // Preserved from the base doc, never defined by any stage.
        ASSERT_EQUALS(graph->getPrevModifyingStage(stages.back().get(), "a.b.d"), nullptr);
        // Excluded by the inclusion.
        ASSERT_EQUALS(graph->getPrevModifyingStage(stages.back().get(), "a.b.c.e"), stages[1]);
    });
}

TEST_F(PipelineDependencyGraphTest, ComplexPathInclusionProjectionModifiedPath) {
    setPipeline(
        "[{$set: { 'a.b.c': 1 }},"
        "{$project: { 'a.b.c.d': 1 }},"
        "{$set: { 'c': 1 }}]");

    runTest([&] {
        // The inclusion modified a (filtered its subfields).
        ASSERT_EQUALS(graph->getPrevModifyingStage(stages.back().get(), "a"), stages[1]);
        // The inclusion modified a.b (filtered its subfields).
        ASSERT_EQUALS(graph->getPrevModifyingStage(stages.back().get(), "a.b"), stages[1]);
        // The inclusion modified a.b.c (filtered its subfields).
        ASSERT_EQUALS(graph->getPrevModifyingStage(stages.back().get(), "a.b.c"), stages[1]);
        // TODO(SERVER-129134): This is technically kept by the inclusion (prefix "a.b.c" was
        // defined by the $set), so the declaring field should be $set.
        ASSERT_EQUALS(graph->getPrevModifyingStage(stages.back().get(), "a.b.c.d"), stages[1]);
        // Excluded by the inclusion.
        ASSERT_EQUALS(graph->getPrevModifyingStage(stages.back().get(), "a.b.c.e"), stages[1]);
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
        ASSERT_EQUALS(graph->getPrevModifyingStage(last, "a"), stages[0]);
        // 'b' never defined — from base collection.
        ASSERT_EQUALS(graph->getPrevModifyingStage(last, "b"), nullptr);
        // 'd' not included — excluded by projection.
        ASSERT_EQUALS(graph->getPrevModifyingStage(last, "d"), stages[1]);
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
        ASSERT_EQUALS(graph->getPrevModifyingStage(last, "a"), stages[0]);
        // 'b' same — from $replaceRoot.
        ASSERT_EQUALS(graph->getPrevModifyingStage(last, "b"), stages[0]);
        // 'd' not included — excluded by projection.
        ASSERT_EQUALS(graph->getPrevModifyingStage(last, "d"), stages[1]);
    });
}

TEST_F(PipelineDependencyGraphTest, InclusionDottedBaseCollectionMultipleSubfields) {
    setPipeline(
        "[{$project: { 'a.b': 1, 'a.c': 1 }},"
        "{$match: { 'a.b': 1, 'a.c': 1, 'a.d': 1 }}]");

    runTest([&] {
        auto* last = stages.back().get();
        // 'a' modified by inclusion (by excluding subfields).
        ASSERT_EQUALS(graph->getPrevModifyingStage(last, "a"), stages[0]);
        // 'a.b' and 'a.c' included from base collection.
        ASSERT_EQUALS(graph->getPrevModifyingStage(last, "a.b"), nullptr);
        ASSERT_EQUALS(graph->getPrevModifyingStage(last, "a.c"), nullptr);
        // 'a.d' excluded.
        ASSERT_EQUALS(graph->getPrevModifyingStage(last, "a.d"), stages[0]);
    });
}

TEST_F(PipelineDependencyGraphTest, InclusionLongDottedBaseCollectionMultipleSubfields) {
    setPipeline(
        "[{$project: { 'a.b.c': 1, 'a.b.d': 1 }},"
        "{$match: { 'a.b': 1, 'a.b.c': 1, 'a.b.d': 1 }}]");

    runTest([&] {
        auto* last = stages.back().get();
        // 'a' modified by inclusion (by excluding subfields).
        ASSERT_EQUALS(graph->getPrevModifyingStage(last, "a"), stages[0]);
        // 'a.b' modified by inclusion (by excluding subfields).
        ASSERT_EQUALS(graph->getPrevModifyingStage(last, "a.b"), stages[0]);
        // 'a.b.c' and 'a.b.d' included from base collection.
        ASSERT_EQUALS(graph->getPrevModifyingStage(last, "a.b.d"), nullptr);
        ASSERT_EQUALS(graph->getPrevModifyingStage(last, "a.b.c"), nullptr);
        // 'a.d' excluded.
        ASSERT_EQUALS(graph->getPrevModifyingStage(last, "a.d"), stages[0]);
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
        ASSERT_EQUALS(graph->getPrevModifyingStage(last, "a"), stages[0]);
        // 'b' excluded by second projection.
        ASSERT_EQUALS(graph->getPrevModifyingStage(last, "b"), stages[2]);
        // 'c' excluded by both projections, but most recently by the second projection.
        // One could argue that the second $project didn't change 'c' since it was already excluded
        // by the first one, but for simplicity we don't consider that when building the graph. This
        // is consistent with dependency tracking for exclusion projections (see
        // 'ChainedExclusionProjections').
        ASSERT_EQUALS(graph->getPrevModifyingStage(last, "c"), stages[2]);
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
        ASSERT_EQUALS(graph->getPrevModifyingStage(last, "a"), stages[0]);
        // 'b' excluded by second projection.
        ASSERT_EQUALS(graph->getPrevModifyingStage(last, "b"), stages[2]);
        // 'c' excluded by both projections, but most recently by the second projection.
        ASSERT_EQUALS(graph->getPrevModifyingStage(last, "c"), stages[2]);
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
        ASSERT_EQUALS(graph->getPrevModifyingStage(last, "a"), stages[1]);
        // 'a.b' included — originates from $replaceRoot.
        ASSERT_EQUALS(graph->getPrevModifyingStage(last, "a.b"), stages[0]);
        // 'a.c' not included — excluded by projection.
        ASSERT_EQUALS(graph->getPrevModifyingStage(last, "a.c"), stages[1]);
    });
}

TEST_F(PipelineDependencyGraphTest, InclusionAndModificationOfSubfields) {
    setPipeline(
        "[{$set: { 'a.b.c': 1 }},"
        " {$project: { 'a.str': 'value', 'a.b.c': 1 }}]");
    runTest([&] {
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "a"), stages[1]);
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "a.b"), stages[1]);
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "a.b.c"), stages[0]);
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "a.str"), stages[1]);
    });
}

TEST_F(PipelineDependencyGraphTest, ModificationOfSubfieldDropsPriorSiblings) {
    setPipeline(
        "[{$set: { 'a.b.c': 1 }},"
        " {$project: { 'a.str': 'value' }}]");
    runTest([&] {
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "a"), stages[1]);
        // 'a.b' is not included by the projection, so it does not pass through from $set.
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "a.b"), stages[1]);
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "a.b.c"), stages[1]);
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "a.str"), stages[1]);
        // The constant from $set must not survive the projection.
        ASSERT_FALSE(graph->getConstant(nullptr, "a.b.c").has_value());
    });
}

TEST_F(PipelineDependencyGraphTest, ModificationOfNestedSubfieldDropsPriorSiblings) {
    setPipeline(
        "[{$set: { 'a.b.c': 1, 'a.b.d': 2 }},"
        " {$project: { 'a.b.str': 'value', 'a.b.c': 1 }}]");
    runTest([&] {
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "a"), stages[1]);
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "a.b"), stages[1]);
        // 'a.b.c' is included, so it passes through from $set.
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "a.b.c"), stages[0]);
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "a.b.str"), stages[1]);
        // 'a.b.d' is not included, so it does not pass through from $set.
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "a.b.d"), stages[1]);
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
        ASSERT_EQUALS(graph->getPrevModifyingStage(last, "a"), stages[1]);
        // TODO(SERVER-129134): 'a.b' preserved by projection, originates from $set (we currently
        // report it as being declared by the inclusion projection).
        ASSERT_EQUALS(graph->getPrevModifyingStage(last, "a.b"), stages[1]);
        // 'a.c' excluded by projection.
        ASSERT_EQUALS(graph->getPrevModifyingStage(last, "a.c"), stages[1]);
    });
}

TEST_F(PipelineDependencyGraphTest, DottedPathAfterBaseField) {
    setPipeline(
        "[{$set: { a: 1 }},"
        "{$set: { 'a.a': 1 }},"
        "{$set: { 'a.b.a': 1 }}]");

    runTest([&] {
        // The a.x seen from the first stage is whatever a.x comes from the base document.
        ASSERT_EQUALS(graph->getPrevModifyingStage(stages[0].get(), "a.x"), nullptr);
        // The a.x seen from the second stage is non-existent erased by the first stage.
        ASSERT_EQUALS(graph->getPrevModifyingStage(stages[1].get(), "a.x"), stages[0]);
        // The a.x seen from the second stage is still the non-existent erased by the first stage.
        ASSERT_EQUALS(graph->getPrevModifyingStage(stages[2].get(), "a.x"), stages[1]);
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "a.x"), stages[1]);
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
        ASSERT_EQUALS(graph->getPrevModifyingStage(ds, "a"), stages[2]);
        ASSERT_EQUALS(graph->getPrevModifyingStage(ds, "b"), stages[2]);
        ASSERT_EQUALS(graph->getPrevModifyingStage(ds, "c"), stages[1]);
        ASSERT_EQUALS(graph->getPrevModifyingStage(ds, "c.c"), stages[1]);
        ASSERT_EQUALS(graph->getPrevModifyingStage(ds, "a.b"), stages[2]);
        ASSERT_EQUALS(graph->getPrevModifyingStage(ds, "a.a"), stages[1]);
        ASSERT_EQUALS(graph->getPrevModifyingStage(ds, "b.b.b"), stages[2]);
        ASSERT_EQUALS(graph->getPrevModifyingStage(ds, "b.b"), stages[2]);
        ASSERT_EQUALS(graph->getPrevModifyingStage(ds, "b.a"), stages[2]);
        ASSERT_EQUALS(graph->getPrevModifyingStage(ds, "a.b.a"), stages[2]);
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
        // TODO(SERVER-129132): With constant propagation for $replaceRoot these should be
        // non-array.
        ASSERT_TRUE(graph->canPathBeArray(nullptr, "a"));
        ASSERT_TRUE(graph->canPathBeArray(nullptr, "a.a"));
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

TEST_F(PipelineDependencyGraphTest, CanInclusionProjectionDottedPrefixInheritAccumulatorArrayness) {
    setPipeline(
        "[{$group: {_id: null, m: {$minN: {input: '$a', n: 1}}}},"
        " {$project: {'m.m2': '$a'}}]");
    runTest([&] {
        // The $minN accumulator output 'm' is always an array, and the dotted inclusion projection
        // '{m.m2: ...}' must preserve that arrayness.
        ASSERT_TRUE(graph->canPathBeArray(nullptr, "m"));
    });
}

TEST_F(PipelineDependencyGraphTest, CanRenamedAccumulatorResultBeArray) {
    // $minN produces an array field 'm'. We move it to a new field 'n' via a rename.
    setPipeline(
        "[{$group: {_id: null, m: {$minN: {input: '$a', n: 1}}}},"
        " {$set: {n: '$m'}}]");
    runTest([&] {
        // 'm' is the $minN accumulator result, which is an array.
        ASSERT_TRUE(graph->canPathBeArray(nullptr, "m"));
        // 'n' is a rename of 'm', so it inherits the accumulator's arrayness and can be an array.
        ASSERT_TRUE(graph->canPathBeArray(nullptr, "n"));
    });
}

TEST_F(PipelineDependencyGraphTest, CanInclusionProjectionDottedPrefixInheritDeclaredArrayness) {
    setPipeline(
        "[{$addFields: {m: {$literal: [1, 2]}}},"
        " {$project: {'m.m2': '$a'}}]");
    runTest([&] {
        // 'm' has a declared array value that must be preserved across the dotted inclusion
        // projection.
        ASSERT_TRUE(graph->canPathBeArray(nullptr, "m"));
    });
}

TEST_F(PipelineDependencyGraphTest,
       CanInclusionProjectionComputedDottedPrefixInheritDeclaredArrayness) {
    // Like the rename variant above, but the dotted projection uses a computed expression
    // ('{m.m2: {$add: [2, 2]}}') so it exercises the non-RenamePath (ModifyPath) code path. The
    // declared array value of the prefix 'm' must still be preserved.
    setPipeline(
        "[{$addFields: {m: {$literal: [1, 2]}}},"
        " {$project: {'m.m2': {$add: [2, 2]}}}]");
    runTest([&] { ASSERT_TRUE(graph->canPathBeArray(nullptr, "m")); });
}

TEST_F(PipelineDependencyGraphTest, InclusionProjectionDottedPrefixScalarStaysNonArray) {
    setPipeline(
        "[{$addFields: {m: {$literal: 5}}},"
        " {$project: {'m.m2': '$a'}}]");
    runTest([&] {
        // A scalar prefix must not be reported as able to be an array by the dotted inclusion
        // projection.
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "m"));
    });
}

TEST_F(PipelineDependencyGraphTest, InclusionProjectionComputedDottedPrefixScalarStaysNonArray) {
    // Like the rename variant above, but the dotted projection uses a computed expression so it
    // exercises the non-RenamePath (ModifyPath) code path. A scalar prefix must not be reported as
    // able to be an array.
    setPipeline(
        "[{$addFields: {m: {$literal: 5}}},"
        " {$project: {'m.m2': {$add: [2, 2]}}}]");
    runTest([&] { ASSERT_FALSE(graph->canPathBeArray(nullptr, "m")); });
}

TEST_F(PipelineDependencyGraphTest,
       CanInclusionPreservePathDottedPrefixInheritAccumulatorArrayness) {
    // A pure (non-computed) dotted inclusion '{m.x: 1}' is described as a PreservePath and handled
    // by includeField(), a different code path than computed dotted projections. It must still
    // preserve the prior arrayness of the prefix 'm' produced by the $minN accumulator.
    setPipeline(
        "[{$group: {_id: null, m: {$minN: {input: '$a', n: 1}}}},"
        " {$project: {'m.x': 1}}]");
    runTest([&] { ASSERT_TRUE(graph->canPathBeArray(nullptr, "m")); });
}

TEST_F(PipelineDependencyGraphTest, CanInclusionPreservePathDottedPrefixInheritArrayLeafArrayness) {
    // 'm' is a concrete array leaf from the previous stage. A pure dotted inclusion '{m.x: 1}'
    // shadows the leaf and must still preserve 'm's array-ness across the fresh inclusion root.
    setPipeline(
        "[{$addFields: {m: {$literal: [1, 2]}}},"
        " {$project: {'m.x': 1}}]");
    runTest([&] { ASSERT_TRUE(graph->canPathBeArray(nullptr, "m")); });
}

TEST_F(PipelineDependencyGraphTest, GroupCompoundKeyDoesNotPreservePrefixArrayness) {
    // '_id' is itself a concrete array [1, 2] from the $set stage, so its prefix arrayness is true.
    // The $group compound key rewrites '_id' as a freshly constructed object '{a: "$x"}': the
    // rename x -> _id.a *builds* the '_id' prefix rather than array-traversing it, so the
    // reconstructed '_id' is a plain object and must NOT inherit the prior array-ness of '_id'.
    setPipeline(
        "[{$set: {_id: {$literal: [1, 2]}}},"
        " {$group: {_id: {a: '$x'}}}]");
    runTest([&] {
        // Before the $group, '_id' is the array [1, 2].
        ASSERT_TRUE(graph->canPathBeArray(stages[1].get(), "_id"));
        // After the $group, the reconstructed '_id' is a plain object, not an array: the rename
        // does not preserve the prior array-ness of '_id'.
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "_id"));
        // '_id' and its compound-key subfield '_id.a' are declared by $group, not carried over
        // from $set.
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "_id"), stages[1]);
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "_id.a"), stages[1]);
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
        // 'b.b.b' is shadowed by scalar 'b.b = 1'; walking its constant yields missing.
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "b.b.b"));
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
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "a"), stages[1]);
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "b"), stages[1]);
        // 'c' set after $replaceRoot.
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "c"), stages[2]);
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
        ASSERT_EQUALS(graph->getPrevModifyingStage(last, "a"), stages[1]);
        ASSERT_EQUALS(graph->getPrevModifyingStage(last, "b"), stages[1]);
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
        ASSERT_EQUALS(graph->getPrevModifyingStage(last, "a"), stages[1]);
        // 'b' not redefined — still attributed to $replaceRoot.
        ASSERT_EQUALS(graph->getPrevModifyingStage(last, "b"), stages[0]);
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
        ASSERT_EQUALS(graph->getPrevModifyingStage(last, "a"), stages[2]);
        ASSERT_EQUALS(graph->getPrevModifyingStage(last, "b"), stages[2]);
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
        ASSERT_EQUALS(graph->getPrevModifyingStage(last, "_id"), stages[1]);
        // 'count' is also declared by $group.
        ASSERT_EQUALS(graph->getPrevModifyingStage(last, "count"), stages[1]);
        // 'a' from the base document is made missing by $group.
        ASSERT_EQUALS(graph->getPrevModifyingStage(last, "a"), stages[1]);
        // 'x' is also made missing by $group.
        ASSERT_EQUALS(graph->getPrevModifyingStage(last, "x"), stages[1]);
    });
}

TEST_F(PipelineDependencyGraphTest, GroupKeyFromBaseDocument) {
    setPipeline(
        "[{$group: { _id: '$x' }},"
        "{$match: { _id: 1 }}]");

    runTest([&] {
        auto* last = stages.back().get();
        // _id declared by $group.
        ASSERT_EQUALS(graph->getPrevModifyingStage(last, "_id"), stages[0]);
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
        ASSERT_EQUALS(graph->getPrevModifyingStage(last, "_id.a"), stages[1]);
        // _id.b declared by $group.
        ASSERT_EQUALS(graph->getPrevModifyingStage(last, "_id.b"), stages[1]);
        // _id declared by group.
        ASSERT_EQUALS(graph->getPrevModifyingStage(last, "_id"), stages[1]);
        // _id.c is not a group key field — attributed to $group via missing sentinel.
        ASSERT_EQUALS(graph->getPrevModifyingStage(last, "_id.c"), stages[1]);
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
        ASSERT_EQUALS(graph->getPrevModifyingStage(last, "_id"), stages[1]);
        // Everything else is made missing by $group.
        ASSERT_EQUALS(graph->getPrevModifyingStage(last, "x"), stages[1]);
        ASSERT_EQUALS(graph->getPrevModifyingStage(last, "x.y"), stages[1]);
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
        ASSERT_EQUALS(graph->getPrevModifyingStage(last, "_id"), stages[0]);
        // 'total' is declared by $group.
        ASSERT_EQUALS(graph->getPrevModifyingStage(last, "total"), stages[0]);
        // 'a' set after $group.
        ASSERT_EQUALS(graph->getPrevModifyingStage(last, "a"), stages[1]);
        // 'b' is made missing by $group.
        ASSERT_EQUALS(graph->getPrevModifyingStage(last, "b"), stages[0]);
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
        ASSERT_EQUALS(graph->getPrevModifyingStage(last, "_id"), stages[0]);
        // 'count' excluded by inclusion projection.
        ASSERT_EQUALS(graph->getPrevModifyingStage(last, "count"), stages[1]);
        // Any arbitrary field last excluded by the inclusion projection.
        ASSERT_EQUALS(graph->getPrevModifyingStage(last, "foo"), stages[1]);
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
        ASSERT_EQUALS(graph->getPrevModifyingStage(last, "_id"), stages[1]);
        // 'a' set after $group.
        ASSERT_EQUALS(graph->getPrevModifyingStage(last, "a"), stages[2]);
        // 'b' made missing by $group.
        ASSERT_EQUALS(graph->getPrevModifyingStage(last, "b"), stages[1]);
    });
}

TEST_F(PipelineDependencyGraphTest, TruncateWithSwappedStages) {
    setPipeline(
        "[{$set: {a: 1}},"
        "{$set: {b: 1}},"
        "{$set: {c: 1}},"
        "{$set: {d: 1}},"
        "{$set: {e: 1}}]");
    auto& container = pipeline->getSources();

    // We used to leave stale entries in _dsToStageId which would cause a crash in the sequence
    // below, thinking incorrectly that B is still a position 2.
    graph->resize(std::next(container.begin(), 3));
    // Move B to the end: [A, C, D, E, B].
    container.splice(container.end(), container, std::next(container.begin()));
    // Rebuild graph over [A, C, D].
    graph->resize(container.begin());
    graph->resize(std::next(container.begin(), 3));

    // Must grow to size 5, not truncate because of stale B.
    ASSERT_DOES_NOT_THROW(graph->resize(container.end()));
}

TEST_F(PipelineDependencyGraphTest, ArrayLeafSiblingRedeclared) {
    pathArrayness->addPath("a.b", {}, true);
    setPipeline(
        "[{$set: {a: [{b: 99}]}},"
        " {$set: {'a.c': 1}}]");

    runTest([&] {
        ASSERT_FALSE(graph->canPathBeArray(stages[0].get(), "a"));
        ASSERT_FALSE(graph->canPathBeArray(stages[0].get(), "a.b"));
        ASSERT_TRUE(graph->canPathBeArray(nullptr, "a"));
    });
}

// $lookup writes its 'as' field directly. When the prefix component 'a' is an array, $lookup
// replaces it with an object, destroying any subfields that existed before (e.g. 'a.c').
TEST_F(PipelineDependencyGraphTest, ModifyPathLookupDottedAsMayDestroySibling) {
    setPipeline(R"([{$lookup: {from: "coll_b", as: "a.b", pipeline: []}}])");

    runTest([&] {
        // 'a.c' may have been destroyed if 'a' was an array, so we attribute it to the lookup.
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "a.c"), stages[0]);
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "a.b"), stages[0]);
        // The prefix is always a plain object.
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "a"));
        ASSERT_TRUE(graph->canPathBeArray(nullptr, "a.c"));
        ASSERT_TRUE(graph->canPathBeArray(nullptr, "a.b"));
    });
}

TEST_F(PipelineDependencyGraphTest, SubFieldExistsAndIsNonMultikey) {
    pathArrayness->addPath("a", {}, true);
    pathArrayness->addPath("a.sub", {}, true);

    setPipeline(R"([
        {$project: {'a': 1}}
    ])");

    runTest([&] {
        // "a" is defined as non-array.
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "a"));

        ASSERT_FALSE(graph->canPathBeArray(nullptr, "a.sub"));
    });
}

TEST_F(PipelineDependencyGraphTest, SubFieldExistsIsSetAndIsNonMultikey) {
    pathArrayness->addPath("a", {}, true);
    pathArrayness->addPath("a.sub", {}, true);

    setPipeline(R"([
        {$set: {'a.b': 'baz'}}
    ])");

    runTest([&] {
        // "a" is defined as non-array.
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "a"));

        ASSERT_FALSE(graph->canPathBeArray(nullptr, "a.sub"));
    });
}

TEST_F(PipelineDependencyGraphTest, SubFieldDoesNotExistAndIsNonMultikey) {
    pathArrayness->addPath("a.sub", {}, true);

    setPipeline(R"([
        {$set: {a: 1}}
    ])");

    runTest([&] {
        // "a" is defined as non-array, and goes through kExact.
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "a"));

        // After setting 'a', this goes through 'kShadowed'
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "a.sub"));
    });
}

TEST_F(PipelineDependencyGraphTest, LeafRedeclaredAsDottedPath) {
    setPipeline(
        "[{$set: {a: 1}},"
        " {$set: {'a.b.c': 1}}]");

    runTest([&] {
        // Before stage 1: 'a.unknown' is shadowed by a:1, declared by stage 0.
        ASSERT_EQUALS(graph->getPrevModifyingStage(stages[1].get(), "a.unknown"), stages[0]);
        // After stage 1: 'a.unknown' is now attributed to stage 1.
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "a.unknown"), stages[1]);
    });
}

TEST_F(PipelineDependencyGraphTest, MissingPathChecksPrefixArrayness) {
    setPipeline(
        "[{$project: {a: 1}},"
        " {$set: {a: [1, 2, 3]}},"
        " {$set: {'a.b': 1}}]");

    runTest([&] { ASSERT_TRUE(graph->canPathBeArray(nullptr, "a.unknown")); });
}

TEST_F(PipelineDependencyGraphTest, MissingRenameChecksPrefixArrayness) {
    setPipeline(
        "[{$project: {a: 1}},"
        " {$set: {a: [1, 2, 3]}},"
        " {$set: {'a.b': 1}},"
        " {$set: {y: '$a.unknown'}}]");

    runTest([&] {
        ASSERT_TRUE(graph->canPathBeArray(nullptr, "a"));
        ASSERT_TRUE(graph->canPathBeArray(nullptr, "y"));
    });
}

// Tests for malformed/edge-case paths

TEST_F(PipelineDependencyGraphTest, PathEmptyString) {
    // Empty string field names are rejected by $set because FieldPath construction
    // requires non-empty path components.
    ASSERT_THROWS(setPipeline(R"([{$set: {"": 1}}])"), DBException);

    ASSERT_THROWS(pathArrayness->addPath("", {}, true), DBException);

    // $match accepts empty string field names as its using FieldRef.
    setPipeline(R"([{$match: {"": 1}}])");

    runTest([&] {
        // getPrevModifyingStage() with empty string returns nullptr (comes from base collection)
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, ""), nullptr);

        // canPathBeArray uses FieldRef.
        ASSERT_TRUE(graph->canPathBeArray(nullptr, ""));
    });
}

TEST_F(PipelineDependencyGraphTest, PathWithLeadingDot) {
    // Field name ".a" has a leading dot which creates an empty path component before 'a'.
    // $set rejects this because FieldPath construction requires non-empty components.
    ASSERT_THROWS(setPipeline(R"([{$set: {".a": 1}}])"), DBException);

    // $match accepts ".a" as a literal field name since we can have field names containing dots.
    setPipeline(R"([{$set: {"a": 1}}, {$match: {".a": 1}}])");

    runTest([&] {
        // getPrevModifyingStage returns nullptr (field comes from base collection)
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, ".a"), nullptr);

        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "a"), stages[0]);

        // canPathBeArray uses FieldRef.
        ASSERT_TRUE(graph->canPathBeArray(nullptr, ".a"));
    });
}

TEST_F(PipelineDependencyGraphTest, PathWithLeadingDotInExpression) {
    // Field path expression "$.a" has a dollar sign followed by a dot, creating an empty
    // path component. $set rejects this because FieldPath construction requires non-empty
    // components.
    ASSERT_THROWS(setPipeline(R"([{$set: {b: "$.a"}}])"), DBException);

    // $match interprets field names starting with '$' as operators (e.g., $gt, $eq).
    // Since "$.a" is not a recognized operator, it fails.
    ASSERT_THROWS(setPipeline(R"([{$match: {"$.a": 1}}])"), DBException);
}

TEST_F(PipelineDependencyGraphTest, PathWithTrailingDot) {
    // Field name "a." has a trailing dot which creates an empty path component after 'a'.
    // $set rejects this because FieldPath construction requires non-empty components.
    ASSERT_THROWS(setPipeline(R"([{$set: {"a.": 1}}])"), DBException);

    // $match accepts "a." as a literal field name since MongoDB documents can have
    // field names containing dots.
    setPipeline(R"([{$set: {"a": 1}}, {$match: {"a.": 1}}])");

    runTest([&] {
        // getPrevModifyingStage returns stage[0] (field comes from the preceding stage)
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "a."), stages[0]);

        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "a"), stages[0]);

        // canPathBeArray accepts both FieldPath and FieldRef.
        ASSERT_TRUE(graph->canPathBeArray(nullptr, "a."));
    });
}

TEST_F(PipelineDependencyGraphTest, PathWithBareDot) {
    // Field name "." is a bare dot which represents an empty path component.
    // $set rejects this because FieldPath construction requires non-empty components.
    ASSERT_THROWS(setPipeline(R"([{$set: {".": 1}}])"), DBException);

    // $match accepts "." as a literal field name since MongoDB documents can have
    // field names containing dots.
    setPipeline(R"([{$match: {".": 1}}])");

    runTest([&] {
        // getPrevModifyingStage returns nullptr (field comes from base collection)
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "."), nullptr);

        // canPathBeArray uses FieldRef which is less restrictive.
        ASSERT_TRUE(graph->canPathBeArray(nullptr, "."));
    });
}

TEST_F(PipelineDependencyGraphTest, PathWithDoubleDot) {
    // Field name "a..b" has consecutive dots which create an empty path component between
    // 'a' and 'b'. $set rejects this because FieldPath construction requires non-empty
    // components.
    ASSERT_THROWS(setPipeline(R"([{$set: {"a..b": 1}}])"), DBException);

    // $match accepts "a..b" as a literal field name since MongoDB documents can have
    // field names containing dots.
    setPipeline(R"([{$match: {"a..b": 1}}])");

    runTest([&] {
        // getPrevModifyingStage returns nullptr (field comes from base collection)
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "a..b"), nullptr);

        // canPathBeArray uses FieldRef which is less restrictive.
        ASSERT_TRUE(graph->canPathBeArray(nullptr, "a..b"));
    });
}

TEST_F(PipelineDependencyGraphTest, PathWithDoubleDotInExpression) {
    // Field path expression "$a..b" has consecutive dots which create an empty path
    // component between 'a' and 'b'. $set rejects this because FieldPath construction
    // requires non-empty components.
    ASSERT_THROWS(setPipeline(R"([{$set: {"result": "$a..b"}}])"), DBException);

    // $match interprets field names starting with '$' as operators (e.g., $gt, $eq).
    // Since "$a..b" is not a recognized operator, it fails.
    ASSERT_THROWS(setPipeline(R"([{$match: {"$a..b": 1}}])"), DBException);
}

TEST_F(PipelineDependencyGraphTest, PathWithManyDotsInExpression) {
    // Test a very long path with 257 components to ensure we properly reject extremely long
    // paths. FieldPath has a maximum depth limit and should throw an exception when exceeded.
    std::string longPath = "a";
    for (int i = 1; i < 257; ++i) {
        longPath += ".a";
    }

    // $set with a field path expression containing 257 components should fail with
    // "FieldPath is too long" error.
    std::string setStage = R"([{$set: {"result": "$)" + longPath + R"("}}])";
    ASSERT_THROWS_WITH_CHECK(setPipeline(setStage), DBException, [](const DBException& ex) {
        ASSERT_STRING_CONTAINS(ex.what(), "FieldPath is too long");
    });

    // Very long path as a target field name should also fail
    std::string setStageTarget = R"([{$set: {")" + longPath + R"(": 1}}])";
    ASSERT_THROWS_WITH_CHECK(setPipeline(setStageTarget), DBException, [](const DBException& ex) {
        ASSERT_STRING_CONTAINS(ex.what(), "FieldPath is too long");
    });

    // $match also rejects the long path, even though it uses FieldRef which is generally
    // more permissive. Both FieldPath and FieldRef have depth limits.
    std::string matchStage = R"([{$match: {")" + longPath + R"(": 1}}])";
    ASSERT_THROWS_WITH_CHECK(setPipeline(matchStage), DBException, [](const DBException& ex) {
        ASSERT_STRING_CONTAINS(ex.what(), "FieldPath is too long");
    });
}

TEST_F(PipelineDependencyGraphTest, PathWithEmptyComponentInMiddle) {
    // Field name "a..b.c" has consecutive dots in the middle which create an empty path
    // component. $set rejects this because FieldPath construction requires non-empty
    // components.
    ASSERT_THROWS(setPipeline(R"([{$set: {"a..b.c": 1}}])"), DBException);

    // $match accepts "a..b.c" as a literal field name since MongoDB documents can have
    // field names containing dots.
    setPipeline(R"([{$match: {"a..b.c": 1}}])");

    runTest([&] {
        // getPrevModifyingStage returns nullptr (field comes from base collection)
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "a..b.c"), nullptr);

        // canPathBeArray accepts both FieldPath and FieldRef.
        ASSERT_TRUE(graph->canPathBeArray(nullptr, "a..b.c"));
    });
}

TEST_F(PipelineDependencyGraphTest, PathWithMultipleEmptyComponents) {
    // Field name "a...b" has three consecutive dots which create multiple empty path
    // components. $set rejects this because FieldPath construction requires non-empty
    // components.
    ASSERT_THROWS(setPipeline(R"([{$set: {"a...b": 1}}])"), DBException);

    // $match accepts "a...b" as a literal field name since MongoDB documents can have
    // field names containing dots.
    setPipeline(R"([{$match: {"a...b": 1}}])");

    runTest([&] {
        // getPrevModifyingStage returns nullptr (field comes from base collection)
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "a...c"), nullptr);

        // canPathBeArray accepts both FieldPath and FieldRef.
        ASSERT_TRUE(graph->canPathBeArray(nullptr, "a...c"));
    });
}

TEST_F(PipelineDependencyGraphTest, PathWithDollarPrefixEmptyComponent) {
    // Field path expression "$." has a dollar sign followed by a dot, creating an empty
    // path component. $set rejects this because FieldPath construction requires non-empty
    // components.
    ASSERT_THROWS(setPipeline(R"([{$set: {result: "$."}}])"), DBException);

    // $match interprets field names starting with '$' as operators (e.g., $gt, $eq).
    // Since "$." is not a recognized operator, it fails.
    ASSERT_THROWS(setPipeline(R"([{$match: {"$.": 1}}])"), DBException);
}

// Tests for dollar sign in path components

TEST_F(PipelineDependencyGraphTest, DollarSignAsComponentInFieldName) {
    // $match interprets "$a" as an operator (like $eq, $gt, etc).
    ASSERT_THROWS(setPipeline(R"([{$match: {"$a": 1}}])"), DBException);

    // The dollar prefixed field '$' in 'a.$' is not valid
    ASSERT_THROWS(setPipeline(R"([{$set: {"a.$": 1}}])"), DBException);

    // $match accepts "a.$" as a literal field name.
    setPipeline(R"([{$match: {"a.$": 1}}])");

    runTest([&] {
        // getPrevModifyingStage returns nullptr (field comes from base collection)
        ASSERT_EQ(graph->getPrevModifyingStage(nullptr, "a.$"), nullptr);

        ASSERT_TRUE(graph->canPathBeArray(nullptr, "a.$"));
    });
}

TEST_F(PipelineDependencyGraphTest, BareDollarSignAsFieldName) {
    // The dollar prefixed field '$' in '$' is not valid
    ASSERT_THROWS(setPipeline(R"([{$set: {"$": 1}}])"), DBException);

    // $match interprets "$" as an operator (like $eq, $gt, etc).
    ASSERT_THROWS(setPipeline(R"([{$match: {"$": 1}}])"), DBException);
}


TEST_F(PipelineDependencyGraphTest, DollarSignInMiddleOfFieldName) {
    // Dollar sign in the middle of a field name like 'field$name' is valid
    // $match accepts it as a literal field name.
    setPipeline(R"([{$match: {"field$name": 1}}])");

    runTest([&] {
        // getPrevModifyingStage returns nullptr (field comes from base collection)
        ASSERT_EQ(graph->getPrevModifyingStage(nullptr, "field$name"), nullptr);

        ASSERT_TRUE(graph->canPathBeArray(nullptr, "field$name"));
    });
}

TEST_F(PipelineDependencyGraphTest, DollarSignInMiddleOfNestedPath) {
    // Dollar sign in the middle of a nested path component like 'a.b$c.d' is valid
    // $match accepts it as a literal field name path.
    setPipeline(R"([{$match: {"a.b$c.d": 1}}])");

    runTest([&] {
        // getPrevModifyingStage returns nullptr (field comes from base collection)
        ASSERT_EQ(graph->getPrevModifyingStage(nullptr, "a.b$c.d"), nullptr);

        ASSERT_TRUE(graph->canPathBeArray(nullptr, "a"));
        ASSERT_TRUE(graph->canPathBeArray(nullptr, "a.b$c"));
        ASSERT_TRUE(graph->canPathBeArray(nullptr, "a.b$c.d"));
    });
}

TEST_F(PipelineDependencyGraphTest, DollarSignAtEndOfNestedPath) {
    // Dollar sign at the end of a nested path like 'a.b.c$' is valid (not the bare '$')
    // $match accepts it as a literal field name path.
    setPipeline(R"([{$match: {"a.b.c$": 1}}])");

    runTest([&] {
        // getPrevModifyingStage returns nullptr (field comes from base collection)
        ASSERT_EQ(graph->getPrevModifyingStage(nullptr, "a.b.c$"), nullptr);

        ASSERT_TRUE(graph->canPathBeArray(nullptr, "a"));
        ASSERT_TRUE(graph->canPathBeArray(nullptr, "a.b"));
        ASSERT_TRUE(graph->canPathBeArray(nullptr, "a.b.c$"));
    });
}

TEST_F(PipelineDependencyGraphTest, MultipleDollarSignsInFieldName) {
    // Multiple dollar signs like 'a$b$c' in the middle/end positions are valid
    // $match accepts it as a literal field name.
    setPipeline(R"([{$match: {"a$b$c": 1}}])");

    runTest([&] {
        // getPrevModifyingStage returns nullptr (field comes from base collection)
        ASSERT_EQ(graph->getPrevModifyingStage(nullptr, "a$b$c"), nullptr);

        ASSERT_TRUE(graph->canPathBeArray(nullptr, "a$b$c"));
    });
}

TEST_F(PipelineDependencyGraphTest, DollarPrefixedNestedPathComponent) {
    // Dollar-prefixed path component in nested path like 'foo.$bar' is invalid in $set
    ASSERT_THROWS(setPipeline(R"([{$set: {"foo.$bar": 1}}])"), DBException);

    // $match accepts "foo.$bar" as a literal field name path.
    setPipeline(R"([{$match: {"foo.$bar": 1}}])");

    runTest([&] {
        // getPrevModifyingStage returns nullptr (field comes from base collection)
        ASSERT_EQ(graph->getPrevModifyingStage(nullptr, "foo.$bar"), nullptr);

        ASSERT_TRUE(graph->canPathBeArray(nullptr, "foo"));
        ASSERT_TRUE(graph->canPathBeArray(nullptr, "foo.$bar"));
    });
}

TEST_F(PipelineDependencyGraphTest, LookupWithDollarInAsField) {
    // $lookup with a dollar-prefixed component in the 'as' field should fail
    ASSERT_THROWS(setPipeline(R"([{$lookup: {
        from: "coll_b",
        localField: "foo",
        foreignField: "bar",
        as: "results.$data"
    }}])"),
                  DBException);

    // $lookup with a field containing dollar in the middle is valid
    setPipeline(R"([{$lookup: {
        from: "coll_b",
        localField: "foo",
        foreignField: "bar",
        as: "result$data"
    }}])");

    runTest([&] {
        // The $lookup stage declares "result$data"
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "result$data"), stages[0]);

        // "result$data" can be an array (it's the output array from $lookup)
        ASSERT_TRUE(graph->canPathBeArray(nullptr, "result$data"));
    });
}

TEST_F(PipelineDependencyGraphTest, UnwindOnFieldWithDollarSign) {
    // $unwind on a field with a dollar-prefixed component should fail
    ASSERT_THROWS(setPipeline(R"([{$unwind: "$foo.$bar"}])"), DBException);

    // $unwind on a field with dollar in the middle is valid
    setPipeline(R"([
        {$match: {"items$list": [1, 2, 3]}},
        {$unwind: "$items$list"}
    ])");

    runTest([&] {
        // The dependency graph conservatively assumes "items$list" can be an array
        // even after $unwind (it doesn't track the unwinding semantics precisely)
        ASSERT_TRUE(graph->canPathBeArray(nullptr, "items$list"));
    });
}

// Tests for numeric path component handling (numeric component in prefix)

TEST_F(PipelineDependencyGraphTest, NumericFirstComponentIsFieldName) {
    pathArrayness->addPath("a", {}, true);
    pathArrayness->addPath("a.sub", {}, true);
    pathArrayness->addPath("0.sub", {}, true);

    setPipeline(R"([
        {$project: {"0": 1}},
        {$project: {result: "$0.sub"}}
    ])");

    runTest([&] {
        // First component "0" is treated as a field name. Will look up path "0.sub".
        // Should find the stage removing "0.sub"
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "0.sub"), stages[1]);
        // 0 comes originally from the collection
        ASSERT_EQUALS(graph->getPrevModifyingStage(stages[0].get(), "0"), nullptr);

        // "a" is removed by the 'project' operator and goes to 'kMissing'.
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "a"));
        ASSERT_FALSE(graph->canPathBeArray(stages[1].get(), "a"));

        // Similarly, a.sub goes to 'kMissing'
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "a.sub"));

        ASSERT_FALSE(graph->canPathBeArray(nullptr, "0"));
        ASSERT_FALSE(graph->canPathBeArray(stages[1].get(), "0"));

        ASSERT_FALSE(graph->canPathBeArray(nullptr, "0.sub"));
    });
}

// Tests for numeric path component handling (numeric component in between other components)

TEST_F(PipelineDependencyGraphTest, NumericPathComponentInMiddle) {
    pathArrayness->addPath("items", {}, true);
    pathArrayness->addPath("items.0.details", {}, true);

    setPipeline(R"([
        {$project: {items: 1}},
        {$project: {result: "$items.0.details"}}
    ])");

    runTest([&] {
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "items.0.details"), stages[1]);

        // "items.0.details" was set as non-array, thus this should be false.
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "result"));
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "items.0.details"));
    });
}

TEST_F(PipelineDependencyGraphTest, MultipleNumericComponentsInMiddle) {
    pathArrayness->addPath("matrix", {}, true);

    setPipeline(R"([
        {$project: {matrix: 1}},
        {$set: {'matrix.0.1.value': [1,2,3]}},
        {$project: {val: "$matrix.0.1.value"}}
    ])");

    runTest([&] {
        // Should handle nested numeric paths without crashing
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "matrix"), stages[2]);
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "matrix.0.1"), stages[2]);
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "matrix.0.1.value"), stages[2]);

        // "matrix" was set as non-array, thus this should be false.
        // This goes to kMissing
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "matrix"));

        // "matrix" is not an array, however we set matrix.0.1.value to an array.
        // This goes to kMissing
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "matrix.0.1.value"));
        ASSERT_TRUE(graph->canPathBeArray(nullptr, "val"));
    });
}

TEST_F(PipelineDependencyGraphTest, NumericWithLeadingZeroNotTruncated) {
    pathArrayness->addPath("data", {}, true);
    pathArrayness->addPath("data.01", {}, true);

    setPipeline(R"([
        {$project: {data: {"01": {value: 1}}}},
        {$project: {result: "$data.01.value"}}
    ])");

    runTest([&] {
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "data.01.value"), stages[1]);

        // goes to kMissing
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "data"));
        // goes to kMissing
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "data.01"));
        // goes to kMissing
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "data.01.value"));
        ASSERT_TRUE(graph->canPathBeArray(nullptr, "result"));
    });
}

TEST_F(PipelineDependencyGraphTest, CheckingValidityOfNumericWithLeadingZeroNotTruncatedTest) {
    pathArrayness->addPath("data", {}, true);
    pathArrayness->addPath("data.foo", {}, true);

    setPipeline(R"([
        {$project: {data: {"foo": {value: 1}}}},
        {$project: {result: "$data.foo.value"}}
    ])");

    runTest([&] {
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "data.foo.value"), stages[1]);

        // goes to kMissing
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "data"));
        // goes to kMissing
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "data.foo"));
        // goes to kMissing
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "data.foo.value"));
        ASSERT_TRUE(graph->canPathBeArray(nullptr, "result"));
    });
}

// Tests for numeric path component handling (numeric component in suffix components)

TEST_F(PipelineDependencyGraphTest, SuffixNumericPathComponentSimple) {
    pathArrayness->addPath("arr", {}, true);

    setPipeline(R"([
        {$project: {arr: 1}},
        {$project: {val: "$arr.0"}}
    ])");

    runTest([&] {
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "arr.0"), stages[1]);

        // goes to kMissing
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "arr.0"));
        ASSERT_TRUE(graph->canPathBeArray(nullptr, "val"));
    });
}

TEST_F(PipelineDependencyGraphTest, SuffixNumericPathSetToArray) {
    setPipeline(R"([
        {$set: {items: [1, 2, 3]}},
        {$project: {first: "$items.0"}}
    ])");

    runTest([&] {
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "items.0"), stages[1]);

        ASSERT_TRUE(graph->canPathBeArray(nullptr, "first"));

        // "items" is explicitly set to an array.
        ASSERT_TRUE(graph->canPathBeArray(stages[1].get(), "items"));
        ASSERT_TRUE(graph->canPathBeArray(stages[1].get(), "items.0"));
    });
}

TEST_F(PipelineDependencyGraphTest, SuffixNumericPathMultipleComponents) {
    setPipeline(R"([
        {$set: {matrix: [[1, 2], [3, 4]]}},
        {$project: {val: "$matrix.0.1"}}
    ])");

    runTest([&] {
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "matrix.0"), stages[1]);

        ASSERT_TRUE(graph->canPathBeArray(nullptr, "val"));

        // "matrix" is explicitly set to an array.
        ASSERT_TRUE(graph->canPathBeArray(stages[1].get(), "matrix"));
        ASSERT_TRUE(graph->canPathBeArray(stages[1].get(), "matrix.0"));
    });
}

TEST_F(PipelineDependencyGraphTest, SuffixNumericPathSetToNonArray) {
    setPipeline(R"([
        {$set: {matrix: "foo"}},
        {$project: {val: "$matrix.0.1"}}
    ])");

    runTest([&] {
        // The $project stage declares "matrix.0" (and by extension "matrix.0.1")
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "matrix.0"), stages[1]);

        ASSERT_FALSE(graph->canPathBeArray(nullptr, "val"));
    });
}

TEST_F(PipelineDependencyGraphTest, SuffixNumericPathSetToNonArrayWithSet) {
    setPipeline(R"([
        {$set: {matrix: "foo"}},
        {$set: {val: "$matrix.0.1"}}
    ])");

    runTest([&] {
        // The $set stage declares "val"
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "val"), stages[1]);

        ASSERT_FALSE(graph->canPathBeArray(nullptr, "val"));
    });
}

TEST_F(PipelineDependencyGraphTest, DollarPrefixedNestedPathComponentWithArrayness) {
    // PathArrayness API should reject paths with dollar-prefixed components like "foo.$bar"
    // because field path components may not start with '$'
    ASSERT_THROWS(pathArrayness->addPath("foo.$bar", {}, false), DBException);

    // $match accepts "foo.$bar" as a literal field name path.
    setPipeline(R"([{$match: {"foo.$bar": 1}}])");

    runTest([&] {
        // getPrevModifyingStage returns nullptr (field comes from base collection)
        ASSERT_EQ(graph->getPrevModifyingStage(nullptr, "foo.$bar"), nullptr);

        // Without PathArrayness metadata, both conservatively assume they can be an array
        ASSERT_TRUE(graph->canPathBeArray(nullptr, "foo"));
        ASSERT_TRUE(graph->canPathBeArray(nullptr, "foo.$bar"));
    });
}

TEST_F(PipelineDependencyGraphTest, SuffixNumericPathSetToNonArrayWithArrayness) {
    // Add PathArrayness metadata for the non-array base path
    pathArrayness->addPath("matrix", {}, true);
    pathArrayness->addPath("matrix.0", {}, true);
    pathArrayness->addPath("matrix.0.1", {}, true);

    setPipeline(R"([
        {$set: {matrix: "foo"}},
        {$project: {val: "$matrix.0.1"}}
    ])");

    runTest([&] {
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "matrix"), stages[1]);
        // The $project stage declares "matrix.0" (and by extension "matrix.0.1")
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "matrix.0"), stages[1]);
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "matrix.0.1"), stages[1]);

        // "matrix" is explicitly marked as non-array in PathArrayness, but
        // after the $set it goes to kMissing since the $project doesn't include it
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "matrix"));
        // "matrix.0.1" is also marked as non-array and goes to kMissing
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "matrix.0.1"));
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "val"));
    });
}

TEST_F(PipelineDependencyGraphTest, SuffixNumericPathSetToNonArrayWithSetAndArrayness) {
    // Add PathArrayness metadata for the non-array base path
    pathArrayness->addPath("matrix", {}, true);
    pathArrayness->addPath("matrix.0", {}, true);
    pathArrayness->addPath("matrix.0.1", {}, true);

    setPipeline(R"([
        {$set: {matrix: "foo"}},
        {$set: {val: "$matrix.0.1"}}
    ])");

    runTest([&] {
        // The $set stage declares "val"
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "val"), stages[1]);

        // With $set (unlike $project), "matrix" is still accessible
        // "matrix" is explicitly marked as non-array in PathArrayness
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "matrix"));
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "matrix.0.1"));
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "val"));
    });
}

// When 'a' is known to be non-array, $lookup will preserve all siblings, same as $set in this case.
TEST_F(PipelineDependencyGraphTest, ModifyPathLookupDottedAsPreservesSiblingWhenNonArray) {
    pathArrayness->addPath("a.c", {}, true);
    setPipeline(R"([{$lookup: {from: "coll_b", as: "a.b", pipeline: []}}])");

    runTest([&] {
        // 'a.c' definitely comes from the base document, if it exists.
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "a.c"), nullptr);
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "a.b"), stages[0]);
        // The prefix is always a plain object.
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "a"));
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "a.c"));
        ASSERT_TRUE(graph->canPathBeArray(nullptr, "a.b"));
    });
}

// Same as above, given that 'a' could be an array, the $lookup is considered to not preserve the
// sibling paths.
TEST_F(PipelineDependencyGraphTest, ModifyPathLookupDottedAsShadowsPriorSibling) {
    setPipeline(R"([
        {$set: {'a.c': 1}},
        {$lookup: {from: "coll_b", as: "a.b", pipeline: []}}
    ])");

    runTest([&] {
        // The $set declared 'a.c', but the $lookup will discard it if 'a' is an array.
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "a.c"), stages[1]);
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "a.b"), stages[1]);
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "a"));
    });
}

// When the prefix is known to be non-array, $lookup will preserve 'a.c'.
TEST_F(PipelineDependencyGraphTest, ModifyPathLookupDottedAsPreservesPriorSiblingWhenNonArray) {
    pathArrayness->addPath("a", {}, true);
    setPipeline(R"([
        {$set: {'a.c': 1}},
        {$lookup: {from: "coll_b", as: "a.b", pipeline: []}}
    ])");

    runTest([&] {
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "a.c"), stages[0]);
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "a.b"), stages[1]);
    });
}

// $lookup with a 3-level deep path, ensures 'a' is non-array, 'a.b' is non-array.
TEST_F(PipelineDependencyGraphTest, LookupDeepAsDestroySiblingsAtAllLevels) {
    setPipeline(R"([{$lookup: {from: "coll_b", as: "a.b.c", pipeline: []}}])");
    runTest([&] {
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "a.d"), stages[0]);
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "a.b.d"), stages[0]);
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "a"));
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "a.b"));
    });
}

// Only 'a' is non-array, but 'a.b' could be. So siblings of 'a.b' are preserved, but not siblings
// of 'a.b.c'.
TEST_F(PipelineDependencyGraphTest, LookupDeepAsPartialPreservation) {
    pathArrayness->addPath("a", {}, true);
    setPipeline(R"([{$lookup: {from: "coll_b", as: "a.b.c", pipeline: []}}])");

    runTest([&] {
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "a.d"), nullptr);
        ASSERT_EQUALS(graph->getPrevModifyingStage(nullptr, "a.b.d"), stages[0]);
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "a"));
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "a.b"));
    });
}

TEST_F(PipelineDependencyGraphTest, GetConstantScalarLiteral) {
    setPipeline("[{$set: {a: 1}}]");
    runTest([&] {
        auto c = graph->getConstant(nullptr, "a");
        ASSERT_TRUE(c.has_value());
        ASSERT_VALUE_EQ(*c, Value(1));
    });
}

TEST_F(PipelineDependencyGraphTest, GetConstantStringLiteral) {
    setPipeline("[{$set: {a: 'hello'}}]");
    runTest([&] {
        auto c = graph->getConstant(nullptr, "a");
        ASSERT_TRUE(c.has_value());
        ASSERT_VALUE_EQ(*c, Value("hello"sv));
    });
}

TEST_F(PipelineDependencyGraphTest, GetConstantSubpathOfScalarIsMissing) {
    setPipeline("[{$set: {a: 1}}]");
    runTest([&] {
        auto c = graph->getConstant(nullptr, "a.b");
        ASSERT_TRUE(c.has_value());
        ASSERT_TRUE(c->missing());
    });
}

TEST_F(PipelineDependencyGraphTest, GetConstantObjectLiteralCapturesLeafConstants) {
    pathArrayness->addPath("a", {}, false);
    pathArrayness->addPath("a.b", {}, false);
    pathArrayness->addPath("a.c", {}, false);
    setPipeline("[{$set: {a: {b: 1, c: 'two'}}}]");
    runTest([&] {
        ASSERT_FALSE(graph->getConstant(nullptr, "a").has_value());

        auto ab = graph->getConstant(nullptr, "a.b");
        ASSERT_TRUE(ab.has_value());
        ASSERT_VALUE_EQ(*ab, Value(1));

        auto ac = graph->getConstant(nullptr, "a.c");
        ASSERT_TRUE(ac.has_value());
        ASSERT_VALUE_EQ(*ac, Value("two"sv));
    });
}

TEST_F(PipelineDependencyGraphTest, GetConstantNestedObjectLiteralReachesLeaf) {
    pathArrayness->addPath("a", {}, false);
    pathArrayness->addPath("a.b", {}, false);
    pathArrayness->addPath("a.b.c", {}, false);
    setPipeline("[{$set: {a: {b: {c: 99}}}}]");
    runTest([&] {
        auto abc = graph->getConstant(nullptr, "a.b.c");
        ASSERT_TRUE(abc.has_value());
        ASSERT_VALUE_EQ(*abc, Value(99));
    });
}

TEST_F(PipelineDependencyGraphTest, GetConstantArrayLiteralCaptured) {
    setOptimizedPipeline("[{$set: {a: [1, 2, 3]}}]");
    runTest([&] {
        auto a = graph->getConstant(nullptr, "a");
        ASSERT_TRUE(a.has_value());
        ASSERT_VALUE_EQ(*a, Value(std::vector<Value>{Value(1), Value(2), Value(3)}));
        ASSERT_TRUE(graph->canPathBeArray(nullptr, "a"));
    });
}

TEST_F(PipelineDependencyGraphTest, GetConstantNestedArrayCapturedButSubpathsAreNot) {
    setOptimizedPipeline("[{$set: {a: [[1, 2], [3, 4]]}}]");
    runTest([&] {
        auto a = graph->getConstant(nullptr, "a");
        ASSERT_TRUE(a.has_value());
        ASSERT_VALUE_EQ(*a,
                        Value(std::vector<Value>{Value(std::vector<Value>{Value(1), Value(2)}),
                                                 Value(std::vector<Value>{Value(3), Value(4)})}));
        ASSERT_FALSE(graph->getConstant(nullptr, "a.0").has_value());
        ASSERT_FALSE(graph->getConstant(nullptr, "a.x").has_value());
    });
}

TEST_F(PipelineDependencyGraphTest, GetConstantArrayOfObjectsCapturedButSubpathsAreNot) {
    setOptimizedPipeline("[{$set: {a: [{b: 1}, {b: 2}]}}]");
    runTest([&] {
        auto a = graph->getConstant(nullptr, "a");
        ASSERT_TRUE(a.has_value());
        ASSERT_VALUE_EQ(
            *a, Value(std::vector<Value>{Value(Document{{"b", 1}}), Value(Document{{"b", 2}})}));
        ASSERT_FALSE(graph->getConstant(nullptr, "a.b").has_value());
        ASSERT_FALSE(graph->getConstant(nullptr, "a.b.c").has_value());
    });
}

TEST_F(PipelineDependencyGraphTest, GetConstantDottedPathSet) {
    pathArrayness->addPath("a", {}, false);
    pathArrayness->addPath("a.b", {}, false);
    pathArrayness->addPath("a.b.c", {}, false);
    setPipeline("[{$set: {'a.b.c': 42}}]");
    runTest([&] {
        auto abc = graph->getConstant(nullptr, "a.b.c");
        ASSERT_TRUE(abc.has_value());
        ASSERT_VALUE_EQ(*abc, Value(42));

        auto abcx = graph->getConstant(nullptr, "a.b.c.x");
        ASSERT_TRUE(abcx.has_value());
        ASSERT_TRUE(abcx->missing());
    });
}

TEST_F(PipelineDependencyGraphTest, GetConstantDottedPathSetWithoutPrefixArraynessIsNotTrusted) {
    setPipeline("[{$set: {'a.b.c': 42}}]");
    runTest([&] {
        ASSERT_FALSE(graph->getConstant(nullptr, "a.b.c").has_value());
        ASSERT_FALSE(graph->getConstant(nullptr, "a.b.c.x").has_value());
    });
}

TEST_F(PipelineDependencyGraphTest, GetConstantPropagatesThroughSimpleRename) {
    setPipeline(
        "[{$set: {a: 1}},"
        " {$set: {b: '$a'}}]");
    runTest([&] {
        auto b = graph->getConstant(nullptr, "b");
        ASSERT_TRUE(b.has_value());
        ASSERT_VALUE_EQ(*b, Value(1));
    });
}

TEST_F(PipelineDependencyGraphTest, GetConstantPropagatesThroughDottedRename) {
    pathArrayness->addPath("a", {}, false);
    pathArrayness->addPath("a.b", {}, false);
    setPipeline(
        "[{$set: {'a.b': 7}},"
        " {$set: {c: '$a.b'}}]");
    runTest([&] {
        auto c = graph->getConstant(nullptr, "c");
        ASSERT_TRUE(c.has_value());
        ASSERT_VALUE_EQ(*c, Value(7));
    });
}

TEST_F(PipelineDependencyGraphTest, GetConstantRenameOfMissingSubpath) {
    setPipeline(
        "[{$set: {a: 1}},"
        " {$set: {b: '$a.x'}}]");
    runTest([&] { ASSERT_FALSE(graph->canPathBeArray(nullptr, "b")); });
}

TEST_F(PipelineDependencyGraphTest, GetConstantLiteralExpression) {
    setPipeline("[{$set: {a: {$literal: '$x'}}}]");
    runTest([&] {
        auto a = graph->getConstant(nullptr, "a");
        ASSERT_TRUE(a.has_value());
        ASSERT_VALUE_EQ(*a, Value("$x"sv));
    });
}

TEST_F(PipelineDependencyGraphTest, GetConstantNoConstantForRuntimeVariable) {
    setPipeline("[{$set: {a: '$$NOW'}}]");
    runTest([&] { ASSERT_FALSE(graph->getConstant(nullptr, "a").has_value()); });
}

TEST_F(PipelineDependencyGraphTest, GetConstantNoConstantForFieldReference) {
    setPipeline("[{$set: {a: '$x'}}]");
    runTest([&] { ASSERT_FALSE(graph->getConstant(nullptr, "a").has_value()); });
}

TEST_F(PipelineDependencyGraphTest, GetConstantPartialObjectIsNotTracked) {
    // ExpressionObject with a non-constant child does not fold to ExpressionConstant.
    setPipeline("[{$set: {a: {b: 1, c: '$x'}}}]");
    runTest([&] { ASSERT_FALSE(graph->getConstant(nullptr, "a").has_value()); });
}

TEST_F(PipelineDependencyGraphTest, GetConstantDroppedWhenLeafRedeclared) {
    setPipeline(
        "[{$set: {a: 1}},"
        " {$set: {a: 2}}]");
    runTest([&] {
        auto a = graph->getConstant(nullptr, "a");
        ASSERT_TRUE(a.has_value());
        ASSERT_VALUE_EQ(*a, Value(2));
    });
}

TEST_F(PipelineDependencyGraphTest, GetConstantNoneForBaseDocument) {
    setPipeline("[{$set: {a: 1}}]");
    runTest([&] {
        ASSERT_FALSE(graph->getConstant(nullptr, "b").has_value());
        // No previous stage relative to the first stage.
        ASSERT_FALSE(graph->getConstant(stages.front().get(), "a").has_value());
    });
}

TEST_F(PipelineDependencyGraphTest, IncludeFieldShadowedByObjectConstant) {
    pathArrayness->addPath("a", {}, false);
    pathArrayness->addPath("a.b", {}, false);
    setPipeline(
        "[{$set: {a: {$literal: {b: 7, c: 'x'}}}},"
        " {$project: {'a.b': 1}}]");
    runTest([&] {
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "a.b"));
        auto ab = graph->getConstant(nullptr, "a.b");
        ASSERT_TRUE(ab.has_value());
        ASSERT_VALUE_EQ(*ab, Value(7));
    });
}

TEST_F(PipelineDependencyGraphTest, IncludeFieldShadowedByObjectConstantMissingSubpath) {
    pathArrayness->addPath("a", {}, false);
    pathArrayness->addPath("a.x", {}, false);
    setPipeline(
        "[{$set: {a: {$literal: {b: 7}}}},"
        " {$project: {'a.x': 1}}]");
    runTest([&] {
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "a.x"));
        auto ax = graph->getConstant(nullptr, "a.x");
        ASSERT_TRUE(ax.has_value());
        ASSERT_TRUE(ax->missing());
    });
}

TEST_F(PipelineDependencyGraphTest, CanPathBeArrayDeepScalarShadow) {
    pathArrayness->addPath("a", {}, false);
    pathArrayness->addPath("a.b", {}, false);
    pathArrayness->addPath("a.b.c", {}, false);
    setPipeline("[{$set: {'a.b.c': 1}}]");
    runTest([&] {
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "a.b.c"));
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "a.b.c.d"));
        ASSERT_FALSE(graph->canPathBeArray(nullptr, "a.b.c.d.e"));
    });
}

TEST_F(PipelineDependencyGraphTest, AlivenessFlagsUnreadAddFields) {
    setPipeline("[{$addFields: {foo: 1}}, {$group: {_id: '$bar'}}]");
    runTest([&] { assertDeadFieldsEq({{stages[0].get(), "foo"}}); });
}

TEST_F(PipelineDependencyGraphTest, AlivenessKeepsAliveAddFields) {
    setPipeline("[{$addFields: {foo: 1}}, {$group: {_id: '$foo'}}]");
    runTest([&] { assertDeadFieldsEq({}); });
}

TEST_F(PipelineDependencyGraphTest, AlivenessPartialAddFields) {
    setPipeline("[{$addFields: {foo: 1, bar: 2}}, {$group: {_id: '$bar'}}]");
    runTest([&] { assertDeadFieldsEq({{stages[0].get(), "foo"}}); });
}

TEST_F(PipelineDependencyGraphTest, AlivenessUsedInMatchThenDropped) {
    setPipeline("[{$match: {foo: 5}}, {$project: {_id: 1}}]");
    runTest([&] { assertDeadFieldsEq({}); });
}

TEST_F(PipelineDependencyGraphTest, AlivenessOverwriteMakesEarlierDead) {
    setPipeline("[{$set: {foo: 1}}, {$set: {foo: 2}}, {$group: {_id: '$foo'}}]");
    runTest([&] { assertDeadFieldsEq({{stages[0].get(), "foo"}}); });
}

TEST_F(PipelineDependencyGraphTest, AlivenessRenameTargetUnreadIsDead) {
    setPipeline("[{$set: {foo: 1}}, {$set: {bar: '$foo'}}, {$group: {_id: '$foo'}}]");
    runTest([&] { assertDeadFieldsEq({{stages[1].get(), "bar"}}); });
}

TEST_F(PipelineDependencyGraphTest, AlivenessRenameTargetAliveKeepsSource) {
    setPipeline("[{$set: {foo: 1}}, {$set: {bar: '$foo'}}, {$group: {_id: '$bar'}}]");
    runTest([&] { assertDeadFieldsEq({}); });
}

TEST_F(PipelineDependencyGraphTest, AlivenessFieldOnlyUsedByDeadFieldIsDead) {
    setPipeline("[{$set: {foo: 1}}, {$set: {bar: '$foo'}}, {$group: {_id: '$baz'}}]");
    runTest([&] { assertDeadFieldsEq({{stages[0].get(), "foo"}, {stages[1].get(), "bar"}}); });
}

TEST_F(PipelineDependencyGraphTest, AlivenessThreeStageDeadChain) {
    setPipeline("[{$set: {a: 1}}, {$set: {b: '$a'}}, {$set: {c: '$b'}}, {$group: {_id: '$x'}}]");
    runTest([&] {
        assertDeadFieldsEq(
            {{stages[0].get(), "a"}, {stages[1].get(), "b"}, {stages[2].get(), "c"}});
    });
}

TEST_F(PipelineDependencyGraphTest, AlivenessAliveSiblingKeepsSharedSource) {
    setPipeline("[{$set: {a: 1}}, {$set: {b: '$a', c: '$a'}}, {$group: {_id: '$b'}}]");
    runTest([&] { assertDeadFieldsEq({{stages[1].get(), "c"}}); });
}

TEST_F(PipelineDependencyGraphTest, AlivenessAllDeadSiblingsKillSharedSource) {
    setPipeline("[{$set: {a: 1}}, {$set: {b: '$a', c: '$a'}}, {$group: {_id: '$x'}}]");
    runTest([&] {
        assertDeadFieldsEq(
            {{stages[0].get(), "a"}, {stages[1].get(), "b"}, {stages[1].get(), "c"}});
    });
}

TEST_F(PipelineDependencyGraphTest, AlivenessMultiDepDeadFieldKillsAllDeps) {
    setPipeline("[{$set: {a: 1, b: 2}}, {$set: {c: {$add: ['$a', '$b']}}}, {$group: {_id: '$x'}}]");
    runTest([&] {
        assertDeadFieldsEq(
            {{stages[0].get(), "a"}, {stages[0].get(), "b"}, {stages[1].get(), "c"}});
    });
}

TEST_F(PipelineDependencyGraphTest, AlivenessAliveSiblingDoesNotReviveDeadSiblingSource) {
    setPipeline("[{$set: {a: 1, b: 1}}, {$set: {c: '$a', d: '$b'}}, {$group: {_id: '$c'}}]");
    runTest([&] { assertDeadFieldsEq({{stages[0].get(), "b"}, {stages[1].get(), "d"}}); });
}

TEST_F(PipelineDependencyGraphTest, AlivenessInclusionThenExclusionProjection) {
    setPipeline("[{$project: {a: 1}}, {$project: {a: 0}}, {$group: {_id: '$x'}}]");
    runTest([&] { assertDeadFieldsEq({{stages[1].get(), "a"}}); });
}

TEST_F(PipelineDependencyGraphTest, AlivenessNestedPathDeadChain) {
    setPipeline("[{$set: {'a.b': 1}}, {$set: {c: '$a.b'}}, {$group: {_id: '$x'}}]");
    runTest([&] { assertDeadFieldsEq({{stages[0].get(), "a.b"}, {stages[1].get(), "c"}}); });
}

TEST_F(PipelineDependencyGraphTest, AlivenessInclusionProjectionKeepsUpstreamAlive) {
    setPipeline("[{$set: {foo: '$x'}}, {$project: {foo: 1}}]");
    runTest([&] { assertDeadFieldsEq({}); });
}

TEST_F(PipelineDependencyGraphTest, AlivenessReplaceRootKeepsUpstreamAlive) {
    setPipeline("[{$set: {foo: 1, user: {a: 1}}}, {$replaceWith: '$user'}]");
    runTest([&] { assertDeadFieldsEq({{stages[0].get(), "foo"}}); });
}

TEST_F(PipelineDependencyGraphTest, AlivenessReplaceRootUnobserved) {
    setPipeline("[{$set: {x: 1}}, {$replaceWith: {a: '$x'}}, {$group: {_id: 1}}]");
    runTest([&] { assertDeadFieldsEq({{stages[0].get(), "x"}}); });
}

TEST_F(PipelineDependencyGraphTest, AlivenessDeadChainBlockedByNonSingleDocStage) {
    setPipeline("[{$set: {bar: 1}}, {$match: {bar: 5}}, {$set: {x: '$bar'}}, {$group: {_id: 1}}]");
    runTest([&] { assertDeadFieldsEq({{stages[2].get(), "x"}}); });
}

TEST_F(PipelineDependencyGraphTest, AlivenessFinalScopePreservation) {
    // foo survives to the pipeline output, so it is alive even though no stage reads it.
    setPipeline("[{$set: {foo: 1}}]");
    runTest([&] { assertDeadFieldsEq({}); });
}

TEST_F(PipelineDependencyGraphTest, AlivenessNestedPath) {
    setPipeline("[{$set: {'a.b': 1}}, {$group: {_id: '$x'}}]");
    runTest([&] { assertDeadFieldsEq({{stages[0].get(), "a.b"}}); });
}

TEST_F(PipelineDependencyGraphTest, AlivenessUnsetReported) {
    setPipeline("[{$unset: 'foo'}, {$project: {bar: 1}}]");
    runTest([&] { assertDeadFieldsEq({{stages[0].get(), "foo"}}); });
}

TEST_F(PipelineDependencyGraphTest, AlivenessExclusionProjectionReported) {
    setPipeline("[{$project: {foo: 0}}, {$project: {bar: 1}}]");
    runTest([&] { assertDeadFieldsEq({{stages[0].get(), "foo"}}); });
}

TEST_F(PipelineDependencyGraphTest, AlivenessUnwindNotReported) {
    setPipeline("[{$unwind: '$arr'}, {$group: {_id: 1}}]");
    runTest([&] { assertDeadFieldsEq({}); });
}

TEST_F(PipelineDependencyGraphTest, AlivenessGroupIdRenameNotReported) {
    // $group emits the _id rename via kAllExcept, but $group is not a single-document
    // transformation so we do not report its _id as dead even when it's unused.
    setPipeline("[{$group: {_id: '$foo'}}, {$project: {bar: 1}}]");
    runTest([&] { assertDeadFieldsEq({}); });
}

TEST_F(PipelineDependencyGraphTest, AlivenessReturnsAllDeadAtOnce) {
    // Two independent dead fields in different stages.
    setPipeline("[{$set: {foo: 1}}, {$set: {bar: 2}}, {$group: {_id: '$baz'}}]");
    runTest([&] { assertDeadFieldsEq({{stages[0].get(), "foo"}, {stages[1].get(), "bar"}}); });
}

TEST_F(PipelineDependencyGraphTest, AlivenessSubpipelineNotAnalyzedAtTopLevel) {
    // 'deadInner' is written and then dropped by $group inside the sub-pipeline, but the
    // top-level analysis does not recurse into it.
    setPipeline(R"([{$lookup: {
        from: "coll_b",
        localField: "a",
        foreignField: "b",
        as: "docs",
        let: {},
        pipeline: [
            {$set: {deadInner: 1}},
            {$group: {_id: "$other"}}
        ]
    }}])");
    runTest([&] {
        assertDeadFieldsEq({});

        const auto* subGraph = graph->getSubpipelineGraph(stages[0].get());
        ASSERT_NOT_EQUALS(subGraph, nullptr);
        auto dead = subGraph->getDeadFields();
        ASSERT_EQ(dead.size(), 1u);
        ASSERT_EQ(dead[0].path.fullPath(), "deadInner");
    });
}

TEST_F(PipelineDependencyGraphTest, AlivenessSubpipelineAliveFieldIsNotReported) {
    // 'extra' survives to the sub-pipeline's final scope, so it is alive in the sub-graph too.
    setPipeline(R"([{$lookup: {
        from: "coll_b",
        localField: "a",
        foreignField: "b",
        as: "docs",
        let: {},
        pipeline: [{$set: {extra: 1}}]
    }}])");
    runTest([&] {
        assertDeadFieldsEq({});
        const auto* subGraph = graph->getSubpipelineGraph(stages[0].get());
        ASSERT_NOT_EQUALS(subGraph, nullptr);
        ASSERT_EQ(subGraph->getDeadFields().size(), 0u);
    });
}

TEST_F(PipelineDependencyGraphTest, AlivenessRenameFromBaseCollectionFieldAliveIsNotDead) {
    // Rename whose source is a base-collection field. The target is referenced downstream, so
    // nothing is dead.
    setPipeline("[{$set: {bar: '$foo'}}, {$group: {_id: '$bar'}}]");
    runTest([&] { assertDeadFieldsEq({}); });
}

TEST_F(PipelineDependencyGraphTest, AlivenessRenameFromBaseCollectionFieldUnreadIsDead) {
    // Rename whose source is a base-collection field but the target is never referenced.
    setPipeline("[{$set: {bar: '$foo'}}, {$group: {_id: '$baz'}}]");
    runTest([&] { assertDeadFieldsEq({{stages[0].get(), "bar"}}); });
}

TEST_F(PipelineDependencyGraphTest, AlivenessRenameFromExplicitlyMissingFieldIsNotDead) {
    // After the inclusion projection, every field other than 'x' is known to be missing, so
    // '$z' resolves to the explicitly-missing field. The rename target is referenced downstream and
    // must stay alive.
    setPipeline("[{$project: {x: 1}}, {$set: {y: '$z'}}, {$group: {_id: '$y'}}]");
    runTest([&] { assertDeadFieldsEq({}); });
}

TEST_F(PipelineDependencyGraphTest, AlivenessRenameFromExplicitlyMissingFieldIsDead) {
    // Same as above, but the rename target is unread, so 'y' is reported as dead.
    setPipeline("[{$project: {x: 1}}, {$set: {y: '$z'}}, {$group: {_id: '$x'}}]");
    runTest([&] { assertDeadFieldsEq({{stages[1].get(), "y"}}); });
}

TEST_F(PipelineDependencyGraphTest, AlivenessExpressionWithDepsTargetAlive) {
    // The expression at the rename target references two upstream fields; the target itself is
    // referenced by $group, so nothing is dead.
    setPipeline(
        "[{$set: {a: 1, b: 2}}, {$set: {c: {$add: ['$a', '$b']}}}, "
        "{$group: {_id: '$c'}}]");
    runTest([&] { assertDeadFieldsEq({}); });
}

TEST_F(PipelineDependencyGraphTest, AlivenessExpressionWithDepsTargetDead) {
    // 'a' stays alive via $group; 'b' dies with the dead 'c'.
    setPipeline(
        "[{$set: {a: 1, b: 2}}, {$set: {c: {$add: ['$a', '$b']}}}, "
        "{$group: {_id: '$a'}}]");
    runTest([&] { assertDeadFieldsEq({{stages[0].get(), "b"}, {stages[1].get(), "c"}}); });
}

TEST_F(PipelineDependencyGraphTest, AlivenessExpressionDepsKeepBaseFieldsAlive) {
    pathArrayness->addPath("base", {}, false);
    setPipeline(
        "[{$set: {a: 1}}, {$set: {c: {$add: ['$a', '$base']}}}, "
        "{$group: {_id: '$c'}}]");
    runTest([&] { assertDeadFieldsEq({}); });
}

TEST_F(PipelineDependencyGraphTest, AlivenessRenameOfRenameTargetAliveKeepsChain) {
    // 'b' renames 'a' and then 'c' renames 'b'; 'c' is referenced downstream so the whole chain is
    // alive.
    pathArrayness->addPath("a", {}, false);
    setPipeline("[{$set: {b: '$a'}}, {$set: {c: '$b'}}, {$group: {_id: '$c'}}]");
    runTest([&] { assertDeadFieldsEq({}); });
}

TEST_F(PipelineDependencyGraphTest, AlivenessRenameOfRenameOnlyMiddleAlive) {
    // 'b' renames 'a' and 'c' renames 'b'; only 'b' is referenced downstream, so 'c' is dead but
    // 'b' is alive.
    pathArrayness->addPath("a", {}, false);
    setPipeline("[{$set: {b: '$a'}}, {$set: {c: '$b'}}, {$group: {_id: '$b'}}]");
    runTest([&] { assertDeadFieldsEq({{stages[1].get(), "c"}}); });
}

TEST_F(PipelineDependencyGraphTest, AlivenessThreeLevelNestedPathDead) {
    setPipeline("[{$set: {'a.b.c': 1}}, {$group: {_id: '$x'}}]");
    runTest([&] { assertDeadFieldsEq({{stages[0].get(), "a.b.c"}}); });
}

TEST_F(PipelineDependencyGraphTest, AlivenessThreeLevelNestedPathAlive) {
    setPipeline("[{$set: {'a.b.c': 1}}, {$group: {_id: '$a.b.c'}}]");
    runTest([&] { assertDeadFieldsEq({}); });
}

TEST_F(PipelineDependencyGraphTest, AlivenessThreeLevelNestedPathSiblingDead) {
    // 'a.b.c' is referenced, but 'a.b.d' isn't, so the latter is dead.
    setPipeline("[{$set: {'a.b.c': 1, 'a.b.d': 2}}, {$group: {_id: '$a.b.c'}}]");
    runTest([&] { assertDeadFieldsEq({{stages[0].get(), "a.b.d"}}); });
}

void assertOrigin(const FieldOrigin& origin,
                  FieldOriginKind expectedKind,
                  const DocumentSource* expectedSource,
                  const char* expectedName) {
    ASSERT_EQ(origin.kind, expectedKind);
    ASSERT_EQ(origin.modifyingStage.get(), expectedSource);
    if (expectedName) {
        ASSERT_NE(origin.inputField, boost::none);
        ASSERT_EQ(*origin.inputField, std::string(expectedName));
    } else {
        ASSERT_EQ(origin.inputField, boost::none);
    }
}

TEST_F(PipelineDependencyGraphTest, ResolveFieldOriginBaseDocumentPassthrough) {
    setPipeline("[{$match: {x: 1}}]");
    runTest([&] {
        assertOrigin(
            graph->resolveFieldOrigin(nullptr, "a"), FieldOriginKind::kBaseDocument, nullptr, "a");
    });
}

TEST_F(PipelineDependencyGraphTest, ResolveFieldOriginInclusionKeptBaseField) {
    setPipeline("[{$project: {x: 1}}]");
    runTest([&] {
        assertOrigin(
            graph->resolveFieldOrigin(nullptr, "x"), FieldOriginKind::kBaseDocument, nullptr, "x");
        assertOrigin(graph->resolveFieldOrigin(nullptr, "x.x"),
                     FieldOriginKind::kBaseDocument,
                     nullptr,
                     "x.x");
    });
}

TEST_F(PipelineDependencyGraphTest, ResolveFieldOriginSimpleRename) {
    setPipeline("[{$set: {a: '$b'}}]");
    runTest([&] {
        assertOrigin(
            graph->resolveFieldOrigin(nullptr, "a"), FieldOriginKind::kAlias, stages[0].get(), "b");
        assertOrigin(graph->resolveFieldOrigin(nullptr, "a.x"),
                     FieldOriginKind::kAlias,
                     stages[0].get(),
                     "b.x");
        assertOrigin(graph->resolveFieldOrigin(nullptr, "a.x.y"),
                     FieldOriginKind::kAlias,
                     stages[0].get(),
                     "b.x.y");
    });
}

TEST_F(PipelineDependencyGraphTest, ResolveFieldOriginDottedRenameWithArrayFreePrefix) {
    pathArrayness->addPath("b", {}, true);
    setPipeline("[{$set: {a: '$b.c'}}]");
    runTest([&] {
        assertOrigin(graph->resolveFieldOrigin(nullptr, "a"),
                     FieldOriginKind::kAlias,
                     stages[0].get(),
                     "b.c");
        assertOrigin(graph->resolveFieldOrigin(nullptr, "a.x"),
                     FieldOriginKind::kAlias,
                     stages[0].get(),
                     "b.c.x");
        assertOrigin(graph->resolveFieldOrigin(nullptr, "a.x.y"),
                     FieldOriginKind::kAlias,
                     stages[0].get(),
                     "b.c.x.y");
    });
}

TEST_F(PipelineDependencyGraphTest, ResolveFieldOriginDottedRenameWithPossibleArrayPrefix) {
    setPipeline("[{$set: {a: '$b.c'}}]");
    runTest([&] {
        assertOrigin(graph->resolveFieldOrigin(nullptr, "a"),
                     FieldOriginKind::kOther,
                     stages[0].get(),
                     nullptr);
        assertOrigin(graph->resolveFieldOrigin(nullptr, "a.x"),
                     FieldOriginKind::kOther,
                     stages[0].get(),
                     nullptr);
        assertOrigin(graph->resolveFieldOrigin(nullptr, "a.x.y"),
                     FieldOriginKind::kOther,
                     stages[0].get(),
                     nullptr);
    });
}

TEST_F(PipelineDependencyGraphTest, ResolveFieldOriginRenameReachedThroughArrayPrefixIsOther) {
    setPipeline("[{$set: {a: '$b'}}, {$set: {'a.c': '$d'}}]");
    runTest([&] {
        assertOrigin(graph->resolveFieldOrigin(nullptr, "a.c"),
                     FieldOriginKind::kOther,
                     stages[1].get(),
                     nullptr);
        assertOrigin(graph->resolveFieldOrigin(nullptr, "a.c.x"),
                     FieldOriginKind::kOther,
                     stages[1].get(),
                     nullptr);
        assertOrigin(graph->resolveFieldOrigin(nullptr, "a.c.x.y"),
                     FieldOriginKind::kOther,
                     stages[1].get(),
                     nullptr);
    });
}

TEST_F(PipelineDependencyGraphTest, ResolveFieldOriginAliasChainResolvesOneHopAtATime) {
    setPipeline("[{$set: {b: '$a'}}, {$set: {c: '$b'}}]");
    runTest([&] {
        auto origin = graph->resolveFieldOrigin(nullptr, "c");
        assertOrigin(origin, FieldOriginKind::kAlias, stages[1].get(), "b");

        origin = graph->resolveFieldOrigin(origin.modifyingStage.get(), *origin.inputField);
        assertOrigin(origin, FieldOriginKind::kAlias, stages[0].get(), "a");

        origin = graph->resolveFieldOrigin(origin.modifyingStage.get(), *origin.inputField);
        assertOrigin(origin, FieldOriginKind::kBaseDocument, nullptr, "a");
    });
}

TEST_F(PipelineDependencyGraphTest, ResolveFieldOriginComputedField) {
    setPipeline("[{$set: {a: {$add: ['$x', 1]}}}]");
    runTest([&] {
        assertOrigin(graph->resolveFieldOrigin(nullptr, "a"),
                     FieldOriginKind::kOther,
                     stages[0].get(),
                     nullptr);
    });
}

TEST_F(PipelineDependencyGraphTest, ResolveFieldOriginAliasOfComputedFieldIsStillAlias) {
    setPipeline("[{$set: {a: {$add: ['$x', 1]}}}, {$set: {b: '$a'}}]");
    runTest([&] {
        assertOrigin(
            graph->resolveFieldOrigin(nullptr, "b"), FieldOriginKind::kAlias, stages[1].get(), "a");
        assertOrigin(graph->resolveFieldOrigin(nullptr, "b.x"),
                     FieldOriginKind::kAlias,
                     stages[1].get(),
                     "a.x");
        assertOrigin(graph->resolveFieldOrigin(nullptr, "a"),
                     FieldOriginKind::kOther,
                     stages[0].get(),
                     nullptr);
        assertOrigin(graph->resolveFieldOrigin(nullptr, "a.x"),
                     FieldOriginKind::kOther,
                     stages[0].get(),
                     nullptr);
    });
}

TEST_F(PipelineDependencyGraphTest, ResolveFieldOriginDroppedByInclusionProjectionIsOther) {
    setPipeline("[{$project: {b: 1}}]");
    runTest([&] {
        assertOrigin(graph->resolveFieldOrigin(nullptr, "a"),
                     FieldOriginKind::kOther,
                     stages[0].get(),
                     nullptr);
    });
}

TEST_F(PipelineDependencyGraphTest, ResolveFieldOriginRemovedByExclusionProjectionIsOther) {
    setPipeline("[{$project: {a: 0}}]");
    runTest([&] {
        assertOrigin(graph->resolveFieldOrigin(nullptr, "a"),
                     FieldOriginKind::kOther,
                     stages[0].get(),
                     nullptr);
    });
}

TEST_F(PipelineDependencyGraphTest, ResolveFieldOriginReplaceRootIsOther) {
    setPipeline("[{$replaceRoot: {newRoot: {a: 1}}}]");
    runTest([&] {
        assertOrigin(graph->resolveFieldOrigin(nullptr, "a"),
                     FieldOriginKind::kOther,
                     stages[0].get(),
                     nullptr);
    });
}

TEST_F(PipelineDependencyGraphTest, ResolveFieldOriginSubpipelineCrossing) {
    setOptimizedPipeline(
        "[{$lookup: {from: 'coll_b', localField: 'x', foreignField: 'y', as: 'e'}}, "
        " {$unwind: '$e'}]");
    runTest([&] {
        // A path under the embedding crosses into the sub-pipeline with the prefix stripped.
        assertOrigin(graph->resolveFieldOrigin(nullptr, "e.y"),
                     FieldOriginKind::kSubpipeline,
                     stages[0].get(),
                     "y");
        assertOrigin(graph->resolveFieldOrigin(nullptr, "e.y.x"),
                     FieldOriginKind::kSubpipeline,
                     stages[0].get(),
                     "y.x");
        assertOrigin(graph->resolveFieldOrigin(nullptr, "e.y.x.z"),
                     FieldOriginKind::kSubpipeline,
                     stages[0].get(),
                     "y.x.z");
        // Referencing the whole embedded document is not a single collection field.
        assertOrigin(graph->resolveFieldOrigin(nullptr, "e"),
                     FieldOriginKind::kOther,
                     stages[0].get(),
                     nullptr);
    });
}

TEST_F(PipelineDependencyGraphTest, ResolveFieldOriginAliasThenSubpipeline) {
    setOptimizedPipeline(
        "[{$lookup: {from: 'coll_b', localField: 'x', foreignField: 'y', as: 'e'}}, "
        " {$unwind: '$e'}, "
        " {$set: {f: '$e.y'}}]");
    runTest([&] {
        assertOrigin(graph->resolveFieldOrigin(nullptr, "f"),
                     FieldOriginKind::kAlias,
                     stages[1].get(),
                     "e.y");
        assertOrigin(graph->resolveFieldOrigin(stages[1].get(), "e.y"),
                     FieldOriginKind::kSubpipeline,
                     stages[0].get(),
                     "y");
    });
}

}  // namespace
}  // namespace mongo::pipeline::dependency_graph
