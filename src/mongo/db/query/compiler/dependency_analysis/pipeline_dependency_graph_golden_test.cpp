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

#include "mongo/bson/json.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/db/query/compiler/dependency_analysis/pipeline_dependency_graph.h"
#include "mongo/db/query/compiler/dependency_analysis/pipeline_dependency_graph_test_util.h"
#include "mongo/unittest/golden_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo::pipeline::dependency_graph {
namespace {

class PipelineDependencyGraphGoldenTest : public unittest::Test {
public:
    struct TestCase {
        const std::string& name;
        const std::string& pipeline;
    };

    PipelineDependencyGraphGoldenTest()
        : _cfg{"src/mongo/db/test_output/query/compiler/dependency_analysis"} {}

    void runVariation(TestCase testCase) {
        auto [name, pipelineStr] = testCase;
        auto pipeline = parsePipeline(pipelineStr);
        DependencyGraph graph(pipeline->getSources());

        // Run the golden test portion.
        {
            unittest::GoldenTestContext ctx(&_cfg);
            ctx.outStream() << "VARIATION " << name << std::endl;
            ctx.outStream() << "input: " << toString(pipeline) << std::endl;
            ctx.outStream() << "output: " << graph.toDebugString() << std::endl;
            ctx.outStream() << std::endl;
        }

        // Now that we've established that the result is correct, assert that it doesn't change if
        // we recompute a part of the graph starting from any stage in the pipeline.
        recomputeAndCompare(graph, *pipeline);
    }

private:
    /**
     * Recompute the subgraphs corresponding to every stage and assert that the graph remains the
     * same in every case.
     */
    void recomputeAndCompare(DependencyGraph& graph, const Pipeline& pipeline) {
        std::string base = graph.toDebugString();
        recomputeAndAssert(graph, pipeline, [&] {
            std::string current = graph.toDebugString();
            ASSERT_EQ(base, current);
        });
    }

    static std::string toString(const std::unique_ptr<Pipeline>& pipeline) {
        auto bson = pipeline->serializeToBson();
        BSONArrayBuilder ba{};
        ba.append(bson.begin(), bson.end());
        return BSON("pipeline" << ba.arr()).jsonString(ExtendedRelaxedV2_0_0, true /*pretty*/);
    }

    std::unique_ptr<Pipeline> parsePipeline(const std::string& inputPipeJson) const {
        const BSONObj inputBson = fromjson("{pipeline: " + inputPipeJson + "}");
        std::vector<BSONObj> rawPipeline;
        for (auto&& stageElem : inputBson["pipeline"].Array()) {
            rawPipeline.push_back(stageElem.embeddedObject());
        }
        const NamespaceString kTestNss =
            NamespaceString::createNamespaceString_forTest("test", "collection");
        AggregateCommandRequest request(kTestNss, rawPipeline);
        boost::intrusive_ptr<ExpressionContextForTest> ctx = new ExpressionContextForTest(kTestNss);
        return pipeline_factory::makePipeline(
            request.getPipeline(), ctx, pipeline_factory::kOptionsMinimal);
    }

    unittest::GoldenTestConfig _cfg;
};

TEST_F(PipelineDependencyGraphGoldenTest, SimpleSet) {
    runVariation({
        .name = "SimpleSet",
        .pipeline = "[{$set: { a: 'foo' }},"
                    "{$match: { a: 'foo' }}]",
    });
}

TEST_F(PipelineDependencyGraphGoldenTest, Shadowing) {
    runVariation({
        .name = "Shadowing",
        .pipeline = "[{$set: { a: 'foo' }},"
                    "{$set: { b: 'bar' }},"
                    "{$set: { a: 'baz' }},"
                    "{$match: { a: 'baz' }}]",
    });
}

TEST_F(PipelineDependencyGraphGoldenTest, ComplexPaths) {
    runVariation({
        .name = "ComplexPaths",
        .pipeline = "[{$set: { a: 1, b: 1, 'c.c': 1 }},"
                    "{$set: { 'a.a': 1, 'a.b': 1, 'b.b.b': 1, c: 1 }},"
                    "{$set: { 'a.b.a': 1, 'b.b': 1, 'b.a': 1 }}]",
    });
}

TEST_F(PipelineDependencyGraphGoldenTest, InclusionProjection) {
    // TODO(SERVER-119374): Double check how inclusion projection should be handled.
    runVariation({
        .name = "InclusionProjection",
        .pipeline = "[{$set: { a: 'foo' }},"
                    "{$set: { b: 'bar' }},"
                    "{$set: { c: 'baz' }},"
                    "{$project: { b: 1, c: 1 }},"
                    "{$match: { a: 'foo', b: 'bar' }}]",
    });
}

TEST_F(PipelineDependencyGraphGoldenTest, ExhaustiveStage) {
    runVariation({
        .name = "ExhaustiveStage",
        .pipeline = "[{$replaceRoot: { newRoot: {} }},"
                    "{$set: { b: 'bar' }},"
                    "{$match: { a: 'baz' }}]",
    });
}

TEST_F(PipelineDependencyGraphGoldenTest, MultipleModifications) {
    runVariation({
        .name = "MultipleModifications",
        .pipeline = "[{$set: { a: 'foo', b: 'bar' }},"
                    "{$set: { a: 'foo2' }},"
                    "{$set: { b: 'bar2' }},"
                    "{$match: { a: 'foo', b: 'bar' }}]",
    });
}

TEST_F(PipelineDependencyGraphGoldenTest, RenameChain) {
    runVariation({
        .name = "RenameChain",
        .pipeline = "[{$set: { b: 'a' }},"
                    "{$set: { c: '$b' }},"
                    "{$set: { d: '$c' }},"
                    "{$set: { e: '$d' }},"
                    "{$match: { e: 1, c: 1 }}]",
    });
}

TEST_F(PipelineDependencyGraphGoldenTest, Swap) {
    runVariation({
        .name = "Swap",
        .pipeline = "[{$set: {a: 1, b: 1}},"
                    "{$set: { a: '$b', b: '$a' }},"
                    "{$match: { a: 1, b: 1 }}]",
    });
}

TEST_F(PipelineDependencyGraphGoldenTest, ParentOverwritesEmbeddedScope) {
    runVariation({
        .name = "ParentOverwritesEmbeddedScope",
        .pipeline = "[{$set: { 'a.b': 1, 'a.c': 2 }},"
                    "{$set: { a: 3 }},"
                    "{$match: { 'a.b': 1 }}]",
    });
}

TEST_F(PipelineDependencyGraphGoldenTest, DottedRenamesReuseParentScope) {
    runVariation({
        .name = "RenamesWithDeepDottedPaths",
        .pipeline = "[{$set: { 'a1.b1.c1.d1.e1.f1': '$a.b.c.d.e.f', b: '$foo1.bar1.baz1' }},"
                    "{$set: { 'a2.b2.c2.d2.e2.f2': '$a1.b1.c1.d1.e1.f1' }},"
                    "{$match: { 'a.b.c.d.e.f': 0,  'a2.b2.c2.d2.e2.f2': 2 }},"
                    "{$match: { $expr: {$eq: ['$a2', '$a1.b2']} }}]",
    });
}

TEST_F(PipelineDependencyGraphGoldenTest, ComplexRename) {
    runVariation({
        .name = "ComplexRename",
        .pipeline = "[{$set: { 'a.b': 1 }},"
                    "{$set: { c: '$a.b' }}]",
    });
}

TEST_F(PipelineDependencyGraphGoldenTest, DottedRename) {
    runVariation({
        .name = "DottedRename",
        .pipeline = "[{$set: { 'a.b.c': 1, 'd.e': 1 }},"
                    "{$set: { f: '$a.b.c', g: '$d.e.h', 'i.j': '$a' }}]",
    });
}

TEST_F(PipelineDependencyGraphGoldenTest, RenameShadowedField) {
    runVariation({
        .name = "DottedRename",
        .pipeline = "[{$set: { 'a.b': 1 }},"
                    "{$set: { 'a': 1 }},"
                    "{$set: { d: '$a.b' }}]",
    });
}

TEST_F(PipelineDependencyGraphGoldenTest, RenameMissingField) {
    runVariation({
        .name = "RenameMissingField",
        .pipeline = "[{$replaceWith: {}},"
                    "{$set: { a: '$b' }}]",
    });
}

TEST_F(PipelineDependencyGraphGoldenTest, DottedRenameMissingField) {
    runVariation({
        .name = "DottedRenameMissingField",
        .pipeline = "[{$replaceWith: {}},"
                    "{$set: { a: '$b.c', d: '$e.f.g', 'h.i': '$j.k' }}]",
    });
}

TEST_F(PipelineDependencyGraphGoldenTest, RenameCollectionField) {
    runVariation({
        .name = "RenameCollectionField",
        .pipeline = "[{$set: { a: '$b' }}]",
    });
}

TEST_F(PipelineDependencyGraphGoldenTest, DottedRenameCollectionField) {
    runVariation({
        .name = "DottedRenameCollectionField",
        .pipeline = "[{$set: { a: '$b.c', d: '$e.f.g', 'h.i': '$j.k' }}]",
    });
}

TEST_F(PipelineDependencyGraphGoldenTest, RenameSameFieldsTwice) {
    runVariation({
        .name = "RenameSameFieldsTwice",
        .pipeline = "[{$set: { a: 1 }},"
                    " {$set: { b: '$a', c: '$a', d: '$f', e: '$f' }}]",
    });
}

}  // namespace
}  // namespace mongo::pipeline::dependency_graph
