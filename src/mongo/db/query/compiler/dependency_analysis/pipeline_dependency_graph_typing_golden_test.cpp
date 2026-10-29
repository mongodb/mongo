// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/compiler/dependency_analysis/pipeline_dependency_graph.h"
#include "mongo/db/query/compiler/dependency_analysis/pipeline_dependency_graph_test_util.h"
#include "mongo/unittest/golden_test.h"
#include "mongo/unittest/unittest.h"

#include <sstream>
#include <string>
#include <vector>

namespace mongo::pipeline::dependency_graph {
namespace {

class PipelineDependencyGraphTypingGoldenTest : public unittest::Test {
public:
    struct TestCase {
        const std::string& name;
        const std::string& pipeline;
        // Paths whose type is rendered at the input of every stage and at the end of the pipeline.
        std::vector<std::string> paths;
    };

    PipelineDependencyGraphTypingGoldenTest()
        : _cfg{"src/mongo/db/test_output/query/compiler/dependency_analysis"} {}

    void runVariation(TestCase testCase) {
        auto pipeline = parsePipeline(testCase.pipeline);
        DependencyGraph graph(pipeline->getSources());

        // Run the golden test portion.
        {
            unittest::GoldenTestContext ctx(&_cfg);
            ctx.outStream() << "VARIATION " << testCase.name << std::endl;
            ctx.outStream() << "input: " << toString(*pipeline) << std::endl;
            ctx.outStream() << "output:\n"
                            << renderTypes(graph, *pipeline, testCase.paths) << std::endl;
        }

        // The inferred types must not change when a part of the graph is recomputed.
        std::string base = renderTypes(graph, *pipeline, testCase.paths);
        recomputeAndAssert(graph, *pipeline, [&] {
            ASSERT_EQ(base, renderTypes(graph, *pipeline, testCase.paths));
        });
    }

private:
    /**
     * Renders the type of every path after every stage in the pipeline.
     */
    static std::string renderTypes(const DependencyGraph& graph,
                                   const Pipeline& pipeline,
                                   const std::vector<std::string>& paths) {
        std::ostringstream ss;
        auto render = [&](const DocumentSource* stage) {
            for (auto&& path : paths) {
                ss << "    " << path << ": " << graph.getType_forTest(stage, path).toDebugString()
                   << std::endl;
            }
        };

        size_t prevIdx = 0;
        const DocumentSource* prev = nullptr;
        for (auto&& stage : pipeline.getSources()) {
            if (prev) {
                ss << "  after " << prev->getSourceName() << ":" << prevIdx++ << std::endl;
                render(stage.get());
            }
            prev = stage.get();
        }
        ss << "  end of pipeline" << std::endl;
        render(nullptr);
        return ss.str();
    }

    unittest::GoldenTestConfig _cfg;
};

TEST_F(PipelineDependencyGraphTypingGoldenTest, NarrowedBySingleMatch) {
    runVariation({
        .name = "NarrowedBySingleMatch",
        .pipeline = "[{$match: {x: {$type: 'number'}}}]",
        .paths = {"x", "x.y", "y"},
    });
}

TEST_F(PipelineDependencyGraphTypingGoldenTest, NegatedArrayTypeRemovesArrayBracket) {
    runVariation({
        .name = "NegatedArrayTypeRemovesArrayBracket",
        .pipeline = "[{$match: {x: {$not: {$type: 'array'}}}}]",
        .paths = {"x"},
    });
}

TEST_F(PipelineDependencyGraphTypingGoldenTest, NarrowAcrossMultipleMatchStages) {
    runVariation({
        .name = "NarrowAcrossMultipleMatchStages",
        .pipeline = "[{$match: {x: {$type: 'number'}}},"
                    " {$match: {x: {$not: {$type: 'array'}}}}]",
        .paths = {"x", "x.y"},
    });
}

// Same as above but the predicates are in a different order.
TEST_F(PipelineDependencyGraphTypingGoldenTest, NarrowingOrderDoesNotMatter) {
    runVariation({
        .name = "NarrowingOrderDoesNotMatter",
        .pipeline = "[{$match: {x: {$not: {$type: 'array'}}}},"
                    " {$match: {x: {$type: 'number'}}}]",
        .paths = {"x"},
    });
}

TEST_F(PipelineDependencyGraphTypingGoldenTest, ContradictingMatchesYieldNever) {
    runVariation({
        .name = "ContradictingMatchesYieldNever",
        .pipeline = "[{$match: {x: {$type: 'number'}}},"
                    " {$match: {x: {$type: 'string'}}},"
                    " {$match: {x: {$not: {$type: 'array'}}}}]",
        .paths = {"x", "x.y"},
    });
}

TEST_F(PipelineDependencyGraphTypingGoldenTest, MatchWithoutTypePredicate) {
    runVariation({
        .name = "MatchWithoutTypePredicate",
        .pipeline = "[{$match: {x: {$gt: 5}}}]",
        .paths = {"x"},
    });
}

TEST_F(PipelineDependencyGraphTypingGoldenTest, UnrelatedSetBetweenMatches) {
    runVariation({
        .name = "UnrelatedSetBetweenMatches",
        .pipeline = "[{$match: {x: {$type: 'number'}}},"
                    " {$set: {y: 1}},"
                    " {$match: {x: {$not: {$type: 'array'}}}}]",
        .paths = {"x"},
    });
}

TEST_F(PipelineDependencyGraphTypingGoldenTest, MatchBeforeModifyingStage) {
    runVariation({
        .name = "MatchBeforeModifyingStage",
        .pipeline = "[{$match: {x: {$not: {$type: 'array'}}}},"
                    " {$set: {x: '$y'}}]",
        .paths = {"x"},
    });
}

TEST_F(PipelineDependencyGraphTypingGoldenTest, MatchesAroundModifyingStage) {
    runVariation({
        .name = "MatchesAroundModifyingStage",
        .pipeline = "[{$match: {x: {$type: 'string'}}},"
                    " {$set: {x: '$y'}},"
                    " {$match: {x: {$type: 'number'}}}]",
        .paths = {"x"},
    });
}

TEST_F(PipelineDependencyGraphTypingGoldenTest, MatchThenExhaustiveStage) {
    runVariation({
        .name = "MatchThenExhaustiveStage",
        .pipeline = "[{$match: {x: {$not: {$type: 'array'}}}},"
                    " {$group: {_id: '$a'}},"
                    " {$match: {_id: {$not: {$type: 'array'}}}}]",
        .paths = {"x", "_id"},
    });
}

TEST_F(PipelineDependencyGraphTypingGoldenTest, MatchOnFieldsMadeMissingByExhaustiveStage) {
    runVariation({
        .name = "MatchOnFieldsMadeMissingByExhaustiveStage",
        .pipeline = "[{$group: {_id: '$a'}},"
                    " {$match: {x: {$not: {$type: 'array'}}}},"
                    " {$match: {y: {$type: 'number'}}}]",
        .paths = {"x", "y"},
    });
}

TEST_F(PipelineDependencyGraphTypingGoldenTest, MatchDependingOnWholeDocument) {
    runVariation({
        .name = "MatchDependingOnWholeDocument",
        .pipeline = "[{$match: {x: {$not: {$type: 'array'}}}},"
                    " {$match: {$expr: {$eq: ['$$ROOT', {y: 1}]}}}]",
        .paths = {"x"},
    });
}

TEST_F(PipelineDependencyGraphTypingGoldenTest, MatchOnUnrelatedFields) {
    runVariation({
        .name = "MatchOnUnrelatedFields",
        .pipeline = "[{$match: {foo: {$not: {$type: 'array'}}}},"
                    " {$match: {foo: {$type: 'string'}}}]",
        .paths = {"x"},
    });
}

TEST_F(PipelineDependencyGraphTypingGoldenTest, MatchUnrelatedAndRelevantFields) {
    runVariation({
        .name = "MatchUnrelatedAndRelevantFields",
        .pipeline = "[{$match: {x: {$not: {$type: 'array'}}}},"
                    " {$match: {foo: {$type: 'string'}}}]",
        .paths = {"x"},
    });
}

TEST_F(PipelineDependencyGraphTypingGoldenTest, MatchDoesNotContradictConstant) {
    runVariation({
        .name = "MatchDoesNotContradictConstant",
        .pipeline = "[{$set: {y: 'str'}},"
                    " {$match: {y: {$type: 'string'}}}]",
        .paths = {"y"},
    });
}

// We don't consider known constants for now. Type narrowing is based solely on $match stages.
TEST_F(PipelineDependencyGraphTypingGoldenTest, MatchContradictsConstant) {
    runVariation({
        .name = "MatchContradictsConstant",
        .pipeline = "[{$set: {y: 'str'}},"
                    " {$match: {y: {$type: 'array'}}}]",
        .paths = {"y"},
    });
}

TEST_F(PipelineDependencyGraphTypingGoldenTest, MatchPlusUnrelatedFieldModifications) {
    runVariation({
        .name = "MatchPlusUnrelatedFieldModifications",
        .pipeline = "[{$match: {x: {$not: {$type: 'array'}}}},"
                    " {$set: {y: 1}},"
                    " {$project: {z: 0}}]",
        .paths = {"x"},
    });
}

TEST_F(PipelineDependencyGraphTypingGoldenTest, SubpathOfObjectField) {
    runVariation({
        .name = "SubpathOfObjectField",
        .pipeline = "[{$match: {x: {$type: 'object'}}},"
                    " {$match: {x: {$not: {$type: 'array'}}}}]",
        .paths = {"x", "x.y"},
    });
}

TEST_F(PipelineDependencyGraphTypingGoldenTest, MatchOnDottedPath) {
    runVariation({
        .name = "MatchOnDottedPath",
        .pipeline = "[{$match: {'x.y': {$not: {$type: 'array'}}}},"
                    " {$match: {'x': {$not: {$type: 'array'}}}}]",
        .paths = {"x", "x.y"},
    });
}

TEST_F(PipelineDependencyGraphTypingGoldenTest, MatchThenModifySubpath) {
    runVariation({
        .name = "MatchThenModifySubpath",
        .pipeline = "[{$match: {x: {$not: {$type: 'array'}}}},"
                    " {$set: {'x.y': 1}}]",
        .paths = {"x"},
    });
}

// Narrowing 'x' no longer constrains 'x.y' once 'x.y' is modified.
TEST_F(PipelineDependencyGraphTypingGoldenTest, DottedPathAfterSubpathModification) {
    runVariation({
        .name = "DottedPathAfterSubpathModification",
        .pipeline = "[{$match: {x: {$type: 'number'}}},"
                    " {$match: {x: {$not: {$type: 'array'}}}},"
                    " {$set: {'x.y': '$a'}}]",
        .paths = {"x.y", "x.y.z"},
    });
}

// The $match stages depend on 'x' rather than on the 'x.y' field declared by $set.
TEST_F(PipelineDependencyGraphTypingGoldenTest, MatchOnPrefixOfDeclaredSubpath) {
    runVariation({
        .name = "MatchOnPrefixOfDeclaredSubpath",
        .pipeline = "[{$set: {'x.y': '$a'}},"
                    " {$match: {x: {$type: 'number'}}},"
                    " {$match: {x: {$not: {$type: 'array'}}}}]",
        .paths = {"x", "x.y"},
    });
}

TEST_F(PipelineDependencyGraphTypingGoldenTest, EmptyPipeline) {
    runVariation({
        .name = "EmptyPipeline",
        .pipeline = "[]",
        .paths = {"x"},
    });
}

}  // namespace
}  // namespace mongo::pipeline::dependency_graph
