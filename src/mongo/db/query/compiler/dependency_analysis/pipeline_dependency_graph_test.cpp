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
            return pathArrayness->canPathBeArray(FieldRef(path), pipeline->getContext().get());
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
        AggregateCommandRequest request(kTestNss, rawPipeline);
        boost::intrusive_ptr<ExpressionContextForTest> ctx = new ExpressionContextForTest(kTestNss);
        return pipeline_factory::makePipeline(
            request.getPipeline(), ctx, pipeline_factory::kOptionsMinimal);
    }
};

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
        ASSERT_EQUALS(graph->getDeclaringStage(stages.back().get(), "d.b.c"), stages.back());
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
        // TODO(SERVER-122273): This is technically kept by the inclusion (prefix "a.b.c" was
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
        // TODO(SERVER-122273): 'a.b' preserved by projection, originates from $set (we currently
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
        auto* ds = stages.back().get();
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
        auto* ds = stages.back().get();
        ASSERT_FALSE(graph->canPathBeArray(ds, "b"));
        ASSERT_FALSE(graph->canPathBeArray(ds, "a"));
        ASSERT_TRUE(graph->canPathBeArray(ds, "a.a"));
        ASSERT_TRUE(graph->canPathBeArray(ds, "a.unknown"));
        ASSERT_TRUE(graph->canPathBeArray(ds, "unknown"));
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
        auto* ds = stages.back().get();
        ASSERT_FALSE(graph->canPathBeArray(ds, "a"));
        ASSERT_FALSE(graph->canPathBeArray(ds, "b"));
        ASSERT_FALSE(graph->canPathBeArray(ds, "c"));
        ASSERT_FALSE(graph->canPathBeArray(ds, "d"));
        // TODO(SERVER-121932): This should pass
        ASSERT_TRUE(graph->canPathBeArray(ds, "d.x"));
    });
}

TEST_F(PipelineDependencyGraphTest, CanRenamedWithProjectBeArray) {
    pathArrayness->addPath("a", {}, true);
    setPipeline("[{$set: {}}, {$group: { _id: '$a' }}]");
    runTest([&] {
        // Lookup from the end of the pipeline.
        auto* ds = stages.back().get();
        ASSERT_FALSE(graph->canPathBeArray(ds, "_id"));
        // 'a' is missing after the $group.
        ASSERT_TRUE(graph->canPathBeArray(ds, "a"));
        ASSERT_FALSE(graph->canPathBeArray(stages.front().get(), "a"));
    });
}

TEST_F(PipelineDependencyGraphTest, CanDottedRenamedPathsBeArray) {
    pathArrayness->addPath("x.y.z", {}, true);
    setPipeline("[{$set: { a: '$x', b: '$x.y', 'c.d': '$x', 'e.f': '$x.y.z', 'g': '$x.y.z' }}]");
    runTest([&] {
        // Lookup from the end of the pipeline.
        auto* ds = stages.back().get();
        ASSERT_FALSE(graph->canPathBeArray(ds, "a"));
        ASSERT_TRUE(graph->canPathBeArray(ds, "a.unknown"));
        ASSERT_FALSE(graph->canPathBeArray(ds, "b"));
        ASSERT_TRUE(graph->canPathBeArray(ds, "b.unknown"));
        // TODO(SERVER-121932): Once we can see .y, this should pass.
        // ASSERT_FALSE(graph->canPathBeArray(ds, "b.z"));
        ASSERT_TRUE(graph->canPathBeArray(ds, "b.z"));
        ASSERT_FALSE(graph->canPathBeArray(ds, "g"));
        ASSERT_TRUE(graph->canPathBeArray(ds, "c"));
        ASSERT_TRUE(graph->canPathBeArray(ds, "c.d"));
        ASSERT_TRUE(graph->canPathBeArray(ds, "c.unknown"));
        ASSERT_TRUE(graph->canPathBeArray(ds, "e"));
        ASSERT_TRUE(graph->canPathBeArray(ds, "e.f"));
        ASSERT_TRUE(graph->canPathBeArray(ds, "e.unknown"));
    });
}

TEST_F(PipelineDependencyGraphTest, CanRedefinedBaseFieldBeArray) {
    setPipeline(
        "[{$set: { a: 1, b: 1, 'c.c': 1 }},"
        "{$set: { 'a.a': 1, 'a.b': 1, 'b.b.b': 1, c: 1 }},"
        "{$set: { 'a.b.a': 1, 'b.b': 1, 'b.a': 1 }}]");
    runTest([&] {
        // Lookup from the end of the pipeline.
        auto* ds = stages.back().get();
        ASSERT_FALSE(graph->canPathBeArray(ds, "a"));
        ASSERT_FALSE(graph->canPathBeArray(ds, "a.a"));
        ASSERT_FALSE(graph->canPathBeArray(ds, "a.b"));
        ASSERT_FALSE(graph->canPathBeArray(ds, "a.b.a"));
        ASSERT_FALSE(graph->canPathBeArray(ds, "b"));
        ASSERT_FALSE(graph->canPathBeArray(ds, "b.b"));
        ASSERT_FALSE(graph->canPathBeArray(ds, "b.a"));
        // TODO(SERVER-119392): This should pass.
        // ASSERT_FALSE(graph->canPathBeArray(ds, "b.b.b"));
    });
}

// TODO(SERVER-121932): Implement prefix rewrite and lookup full path in path arrayness.
// TEST_F(PipelineDependencyGraphTest, CanBeArraysLooksThroughDottedRenamedPaths) {
//     pathArrayness->addPath("x.y.z", {}, true);
//     setPipeline("[{$set: { a: '$x', b: '$x.y', 'c.d': '$x', 'e.f': '$x.y.z' }}]");
//     runTest([&] {
//         // Lookup from the end of the pipeline.
//         auto* ds = stages.back().get();
//         ASSERT_FALSE(graph->canPathBeArray(ds, "a.y.z"));
//     });
// }

TEST_F(PipelineDependencyGraphTest, ReplaceRootAttributesAllFields) {
    setPipeline(
        "[{$set: { a: 1 }},"
        "{$replaceRoot: { newRoot: '$a' }},"
        "{$set: { c: 1 }}]");

    runTest([&] {
        auto* last = stages.back().get();
        // All pre-existing fields are attributed to $replaceRoot.
        ASSERT_EQUALS(graph->getDeclaringStage(last, "a"), stages[1]);
        ASSERT_EQUALS(graph->getDeclaringStage(last, "b"), stages[1]);
        // 'c' set after $replaceRoot.
        ASSERT_EQUALS(graph->getDeclaringStage(last, "c"), stages[2]);
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

// TODO(SERVER-121639): Enable.
// TEST_F(PipelineDependencyGraphTest, GroupKeyFromBaseDocument) {
//     setPipeline(
//         "[{$group: { _id: '$x' }},"
//         "{$match: { _id: 1 }}]");
//
//     runTest([&] {
//         auto* last = stages.back().get();
//         // _id declared by $group.
//         ASSERT_EQUALS(graph->getDeclaringStage(last, "_id"), stages[0]);
//     });
// }

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
