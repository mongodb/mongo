// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/dependency_analysis/pipeline_dependency_graph_test_util.h"

#include "mongo/bson/json.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/db/pipeline/resolved_namespace.h"

namespace mongo::pipeline::dependency_graph {

std::string toString(const Pipeline& pipeline) {
    auto bson = pipeline.serializeToBson();
    BSONArrayBuilder ba{};
    ba.append(bson.begin(), bson.end());
    return BSON("pipeline" << ba.arr()).jsonString(ExtendedRelaxedV2_0_0, true /*pretty*/);
}

std::unique_ptr<Pipeline> parsePipeline(const std::string& inputPipeJson) {
    const BSONObj inputBson = fromjson("{pipeline: " + inputPipeJson + "}");
    std::vector<BSONObj> rawPipeline;
    for (auto&& stageElem : inputBson["pipeline"].Array()) {
        rawPipeline.push_back(stageElem.embeddedObject());
    }
    const NamespaceString kTestNss =
        NamespaceString::createNamespaceString_forTest("test", "collection");
    const NamespaceString kCollB = NamespaceString::createNamespaceString_forTest("test", "coll_b");
    AggregateCommandRequest request(kTestNss, rawPipeline);
    boost::intrusive_ptr<ExpressionContextForTest> ctx = new ExpressionContextForTest(kTestNss);
    ResolvedNamespaceMap resolvedNs;
    resolvedNs.insert_or_assign(kTestNss, {kTestNss, std::vector<BSONObj>{}});
    resolvedNs.insert_or_assign(kCollB, {kCollB, std::vector<BSONObj>{}});
    ctx->setResolvedNamespaces(std::move(resolvedNs));
    return pipeline_factory::makePipeline(
        request.getPipeline(), ctx, pipeline_factory::kOptionsMinimal);
}
}  // namespace mongo::pipeline::dependency_graph
