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
    void setPipeline(const std::string& array) {
        pipeline = parsePipeline(array);
        stages.assign(pipeline->getSources().begin(), pipeline->getSources().end());
        graph = std::make_unique<DependencyGraph>(pipeline->getSources());
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
        // For field 'a.b.c', expect stage 2 (the $project)
        // This is because we do not track inclusion specifically, but modified paths.
        // $project will report it modifies a.b.
        // TODO(SERVER-119374): Inclusion projections need to be fixed.
        ASSERT_EQUALS(graph->getDeclaringStage(stages.back().get(), "a"), stages[1]);
        ASSERT_EQUALS(graph->getDeclaringStage(stages.back().get(), "a.b"), stages[1]);
        ASSERT_EQUALS(graph->getDeclaringStage(stages.back().get(), "a.b.c"), stages[1]);
        ASSERT_EQUALS(graph->getDeclaringStage(stages.back().get(), "a.b.d"), stages[1]);
        ASSERT_EQUALS(graph->getDeclaringStage(stages.back().get(), "a.b.c.e"), stages[1]);
    });
}

TEST_F(PipelineDependencyGraphTest, ComplexPathInclusionProjectionModifiedPath) {
    setPipeline(
        "[{$set: { 'a.b.c': 1 }},"
        "{$project: { 'a.b.c.d': 1 }},"
        "{$set: { 'c': 1 }}]");

    runTest([&] {
        // For field 'a.b.c', expect stage 2 (the $project)
        // This is because we do not track inclusion specifically, but modified paths.
        // $project will report it modifies a.b.
        // TODO(SERVER-119374): Inclusion projections need to be fixed.
        ASSERT_EQUALS(graph->getDeclaringStage(stages.back().get(), "a"), stages[1]);
        ASSERT_EQUALS(graph->getDeclaringStage(stages.back().get(), "a.b"), stages[1]);
        ASSERT_EQUALS(graph->getDeclaringStage(stages.back().get(), "a.b.c"), stages[1]);
        ASSERT_EQUALS(graph->getDeclaringStage(stages.back().get(), "a.b.c.d"), stages[1]);
        ASSERT_EQUALS(graph->getDeclaringStage(stages.back().get(), "a.b.c.e"), stages[1]);
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

}  // namespace
}  // namespace mongo::pipeline::dependency_graph
