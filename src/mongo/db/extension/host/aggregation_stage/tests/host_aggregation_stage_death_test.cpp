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

#include "mongo/db/extension/host/pipeline_rewrite_context.h"
#include "mongo/db/extension/host/query_execution_context.h"
#include "mongo/db/extension/host_connector/adapter/logical_agg_stage_adapter.h"
#include "mongo/db/extension/host_connector/adapter/pipeline_rewrite_context_adapter.h"
#include "mongo/db/extension/host_connector/adapter/query_execution_context_adapter.h"
#include "mongo/db/extension/sdk/tests/shared_test_stages.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/distributed_plan_logic.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/logical.h"
#include "mongo/db/extension/shared/handle/pipeline_rewrite_context_handle.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/optimization/rule_based_rewriter.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/unittest/death_test.h"

#include <memory>

namespace mongo::extension {
namespace {

// Helper to build a HostLogicalAggStageAdapter backed by a DocumentSourceMock.
// The returned mock must outlive the adapter.
std::pair<boost::intrusive_ptr<DocumentSourceMock>,
          std::unique_ptr<host_connector::HostLogicalAggStageAdapter>>
makeAdapterWithMock() {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    auto mock = DocumentSourceMock::createForTest({}, expCtx);
    auto logicalStage = host::LogicalAggStage::make(mock.get());
    auto adapter =
        std::make_unique<host_connector::HostLogicalAggStageAdapter>(std::move(logicalStage));
    return {std::move(mock), std::move(adapter)};
}

DEATH_TEST(HostLogicalAggStageAdapterDeathTest, NullUnderlyingStageAsserts, "12303709") {
    [[maybe_unused]] host_connector::HostLogicalAggStageAdapter adapter(nullptr);
}

DEATH_TEST(HostLogicalAggStageAdapterDeathTest, SerializeOnHostAllocatedStageTasserts, "12303700") {
    auto [mock, adapter] = makeAdapterWithMock();
    LogicalAggStageAPI api(adapter.get());
    [[maybe_unused]] auto result = api.serialize();
}

DEATH_TEST(HostLogicalAggStageAdapterDeathTest, ExplainOnHostAllocatedStageTasserts, "12303701") {
    auto [mock, adapter] = makeAdapterWithMock();
    LogicalAggStageAPI api(adapter.get());
    host_connector::QueryExecutionContextAdapter ctxAdapter(
        std::make_unique<sdk::shared_test_stages::MockQueryExecutionContext>());
    [[maybe_unused]] auto result =
        api.explain(ctxAdapter, ExplainOptions::Verbosity::kQueryPlanner);
}

DEATH_TEST(HostLogicalAggStageAdapterDeathTest, CompileOnHostAllocatedStageTasserts, "12303702") {
    auto [mock, adapter] = makeAdapterWithMock();
    LogicalAggStageAPI api(adapter.get());
    [[maybe_unused]] auto result = api.compile();
}

DEATH_TEST(HostLogicalAggStageAdapterDeathTest,
           GetDistributedPlanLogicOnHostAllocatedStageTasserts,
           "12303703") {
    auto [mock, adapter] = makeAdapterWithMock();
    LogicalAggStageAPI api(adapter.get());
    [[maybe_unused]] auto result = api.getDistributedPlanLogic();
}

DEATH_TEST(HostLogicalAggStageAdapterDeathTest, CloneOnHostAllocatedStageTasserts, "12303704") {
    auto [mock, adapter] = makeAdapterWithMock();
    LogicalAggStageAPI api(adapter.get());
    [[maybe_unused]] auto result = api.clone();
}

DEATH_TEST(HostLogicalAggStageAdapterDeathTest,
           IsSortedByVectorSearchScoreOnHostAllocatedStageTasserts,
           "12303705") {
    auto [mock, adapter] = makeAdapterWithMock();
    LogicalAggStageAPI api(adapter.get());
    [[maybe_unused]] auto result = api.isSortedByVectorSearchScore_deprecated();
}

DEATH_TEST(HostLogicalAggStageAdapterDeathTest,
           SetVectorSearchLimitForOptimizationOnHostAllocatedStageTasserts,
           "12303706") {
    auto [mock, adapter] = makeAdapterWithMock();
    LogicalAggStageAPI api(adapter.get());
    api.setExtractedLimitVal_deprecated(5LL);
}

DEATH_TEST(HostLogicalAggStageAdapterDeathTest,
           EvaluateRulePreconditionOnHostAllocatedStageTasserts,
           "12303707") {
    auto [mock, adapter] = makeAdapterWithMock();
    LogicalAggStageAPI api(adapter.get());
    [[maybe_unused]] auto result = api.evaluateRulePrecondition("rule"_sd, nullptr);
}

DEATH_TEST(HostLogicalAggStageAdapterDeathTest,
           EvaluateRuleTransformOnHostAllocatedStageTasserts,
           "12303708") {
    auto [mock, adapter] = makeAdapterWithMock();
    LogicalAggStageAPI api(adapter.get());
    [[maybe_unused]] auto result = api.evaluateRuleTransform("rule"_sd, nullptr);
}

// ---- PipelineRewriteContextAPI vtable constraint death tests ----

DEATH_TEST(PipelineRewriteContextAPIDeathTest, NullGetNthNextStageTasserts, "12200600") {
    static MongoExtensionPipelineRewriteContextVTable kVTable = {
        .get_nth_next_stage = nullptr,
        .erase_nth_next_stage = [](MongoExtensionPipelineRewriteContext*,
                                   size_t,
                                   bool*) -> MongoExtensionStatus* { return nullptr; },
        .has_at_least_n_next_stages = [](const MongoExtensionPipelineRewriteContext*,
                                         size_t,
                                         bool*) -> MongoExtensionStatus* { return nullptr; },
    };
    MongoExtensionPipelineRewriteContext ctx{&kVTable};
    [[maybe_unused]] PipelineRewriteContextAPI api(&ctx);
}

DEATH_TEST(PipelineRewriteContextAPIDeathTest, NullEraseNthNextStageTasserts, "12200601") {
    static MongoExtensionPipelineRewriteContextVTable kVTable = {
        .get_nth_next_stage = [](const MongoExtensionPipelineRewriteContext*,
                                 size_t,
                                 MongoExtensionLogicalAggStage**) -> MongoExtensionStatus* {
            return nullptr;
        },
        .erase_nth_next_stage = nullptr,
        .has_at_least_n_next_stages = [](const MongoExtensionPipelineRewriteContext*,
                                         size_t,
                                         bool*) -> MongoExtensionStatus* { return nullptr; },
    };
    MongoExtensionPipelineRewriteContext ctx{&kVTable};
    [[maybe_unused]] PipelineRewriteContextAPI api(&ctx);
}

DEATH_TEST(PipelineRewriteContextAPIDeathTest, NullHasAtLeastNNextStagesTasserts, "12200607") {
    static MongoExtensionPipelineRewriteContextVTable kVTable = {
        .get_nth_next_stage = [](const MongoExtensionPipelineRewriteContext*,
                                 size_t,
                                 MongoExtensionLogicalAggStage**) -> MongoExtensionStatus* {
            return nullptr;
        },
        .erase_nth_next_stage = [](MongoExtensionPipelineRewriteContext*,
                                   size_t,
                                   bool*) -> MongoExtensionStatus* { return nullptr; },
        .has_at_least_n_next_stages = nullptr,
    };
    MongoExtensionPipelineRewriteContext ctx{&kVTable};
    [[maybe_unused]] PipelineRewriteContextAPI api(&ctx);
}

// ---- PipelineRewriteContextAdapter death tests ----

DEATH_TEST(PipelineRewriteContextAdapterDeathTest, NullCtxAtConstructionTasserts, "12200604") {
    [[maybe_unused]] host_connector::PipelineRewriteContextAdapter adapter(nullptr);
}

DEATH_TEST(PipelineRewriteContextAdapterDeathTest, GetNthNextStageOutOfBoundsTasserts, "12200602") {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    DocumentSourceContainer container;
    container.push_back(DocumentSourceLimit::create(expCtx, 10));

    // Context is positioned at the only stage — no next stages exist.
    rule_based_rewrites::pipeline::PipelineRewriteContext rbrCtx(*expCtx, container);
    host_connector::PipelineRewriteContextAdapter adapter(
        host::PipelineRewriteContext::make(&rbrCtx));

    [[maybe_unused]] auto stage = PipelineRewriteContextAPI(&adapter).getNthNextStage(1);
}

DEATH_TEST(PipelineRewriteContextAdapterDeathTest,
           EraseNthNextStageOutOfBoundsTasserts,
           "12200603") {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    DocumentSourceContainer container;
    container.push_back(DocumentSourceLimit::create(expCtx, 10));

    // Context is positioned at the only stage — no next stages exist.
    rule_based_rewrites::pipeline::PipelineRewriteContext rbrCtx(*expCtx, container);
    host_connector::PipelineRewriteContextAdapter adapter(
        host::PipelineRewriteContext::make(&rbrCtx));

    PipelineRewriteContextAPI(&adapter).eraseNthNext(1);
}

}  // namespace
}  // namespace mongo::extension
