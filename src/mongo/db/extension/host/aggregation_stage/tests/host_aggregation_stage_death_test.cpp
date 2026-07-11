// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/extension/host/aggregation_stage/ast_node.h"
#include "mongo/db/extension/host/pipeline_rewrite_context.h"
#include "mongo/db/extension/host/query_execution_context.h"
#include "mongo/db/extension/host_connector/adapter/executable_agg_stage_adapter.h"
#include "mongo/db/extension/host_connector/adapter/host_services_adapter.h"
#include "mongo/db/extension/host_connector/adapter/logical_agg_stage_adapter.h"
#include "mongo/db/extension/host_connector/adapter/pipeline_rewrite_context_adapter.h"
#include "mongo/db/extension/host_connector/adapter/query_execution_context_adapter.h"
#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/host_services.h"
#include "mongo/db/extension/sdk/tests/fruits_test_stage.h"
#include "mongo/db/extension/sdk/tests/shared_test_stages.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/ast_node.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/distributed_plan_logic.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/executable_agg_stage.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/logical.h"
#include "mongo/db/extension/shared/handle/pipeline_rewrite_context_handle.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/optimization/rule_based_rewriter.h"
#include "mongo/db/pipeline/search/lite_parsed_internal_search_id_lookup.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <memory>

namespace mongo::extension {
namespace {
using namespace std::literals::string_view_literals;

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
    [[maybe_unused]] auto result = api.evaluatePipelineRewriteRulePrecondition("rule"sv, nullptr);
}

DEATH_TEST(HostLogicalAggStageAdapterDeathTest,
           EvaluateRuleTransformOnHostAllocatedStageTasserts,
           "12303708") {
    auto [mock, adapter] = makeAdapterWithMock();
    LogicalAggStageAPI api(adapter.get());
    [[maybe_unused]] auto result = api.evaluatePipelineRewriteRuleTransform("rule"sv, nullptr);
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
        .get_pipeline_suffix_bounds = [](const MongoExtensionPipelineRewriteContext*,
                                         MongoExtensionDocsNeededBounds*) -> MongoExtensionStatus* {
            return nullptr;
        },
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
        .get_pipeline_suffix_bounds = [](const MongoExtensionPipelineRewriteContext*,
                                         MongoExtensionDocsNeededBounds*) -> MongoExtensionStatus* {
            return nullptr;
        },
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
        .get_pipeline_suffix_bounds = [](const MongoExtensionPipelineRewriteContext*,
                                         MongoExtensionDocsNeededBounds*) -> MongoExtensionStatus* {
            return nullptr;
        },
    };
    MongoExtensionPipelineRewriteContext ctx{&kVTable};
    [[maybe_unused]] PipelineRewriteContextAPI api(&ctx);
}

DEATH_TEST(PipelineRewriteContextAPIDeathTest, NullGetPipelineSuffixBoundsTasserts, "12200501") {
    static MongoExtensionPipelineRewriteContextVTable kVTable = {
        .get_nth_next_stage = [](const MongoExtensionPipelineRewriteContext*,
                                 size_t,
                                 MongoExtensionLogicalAggStage**) -> MongoExtensionStatus* {
            return nullptr;
        },
        .erase_nth_next_stage = [](MongoExtensionPipelineRewriteContext*,
                                   size_t,
                                   bool*) -> MongoExtensionStatus* { return nullptr; },
        .has_at_least_n_next_stages = [](const MongoExtensionPipelineRewriteContext*,
                                         size_t,
                                         bool*) -> MongoExtensionStatus* { return nullptr; },
        .get_pipeline_suffix_bounds = nullptr,
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

// ---- Host exec agg stage adapter/handle death tests ----

class HostExecAggStageAdapterDeathTest : public unittest::Test {
public:
    void setUp() override {
        // Initialize HostServices so that aggregation stages will be able to access member
        // functions, e.g. to run assertions.
        sdk::HostServicesAPI::setHostServices(&host_connector::HostServicesAdapter::get());
    }
};

DEATH_TEST_F(HostExecAggStageAdapterDeathTest,
             HostExecAggStageAdapterNullStageAsserts,
             "10957207") {
    [[maybe_unused]] auto adapter = host_connector::HostExecAggStageAdapter{nullptr};
}

DEATH_TEST_F(HostExecAggStageAdapterDeathTest, SetSourceOnSourceStageFails, "10957210") {
    // Setting the source of a source stage should fail irrespective of the type of the stage being
    // set as the source.
    auto sourceHandle = ExecAggStageHandle{new sdk::ExtensionExecAggStageAdapter(
        sdk::shared_test_stages::AddFruitsToDocumentsExecStage::make())};
    // ValidExtensionExecAggStage is a source stage.
    auto handle = ExecAggStageHandle{new sdk::ExtensionExecAggStageAdapter(
        sdk::shared_test_stages::ValidExtensionExecAggStage::make())};

    // Calling setSource on a source stage should fail.
    handle->setSource(sourceHandle);
}

DEATH_TEST_F(HostExecAggStageAdapterDeathTest, GetSourceOnSourceStageFails, "10957208") {
    sdk::shared_test_stages::FruitsAsDocumentsExecStage sourceStage{};
    // Calling getSource on a source stage should fail.
    [[maybe_unused]] auto source = sourceStage._getSource();
}

DEATH_TEST_F(HostExecAggStageAdapterDeathTest, GetNameOnMovedHandleFails, "10596403") {
    auto sourceHandle = ExecAggStageHandle{new sdk::ExtensionExecAggStageAdapter(
        sdk::shared_test_stages::AddFruitsToDocumentsExecStage::make())};

    auto sourceHandle2 = std::move(sourceHandle);

    // Calling getName on a source handle should fail.
    [[maybe_unused]] auto source = sourceHandle->getName();
}

// ---- Host AST node adapter death tests ----

/**
 * Helper that builds an IdLookupAstNode wrapping a $_internalSearchIdLookup LiteParsed.
 */
std::unique_ptr<host::AggStageAstNode> makeIdLookupAstNode() {
    DocumentSourceIdLookupSpec spec;
    return std::make_unique<host::IdLookupAstNode>(
        std::make_unique<mongo::LiteParsedInternalSearchIdLookUp>(spec));
}

DEATH_TEST(HostAstNodeVTableDeathTest, InvalidAstNodeVTableFailsGetName, "517") {
    auto vtable = host::HostAggStageAstNodeAdapter::getVTable();
    vtable.get_name = nullptr;
    AggStageAstNodeAPI::assertVTableConstraints(vtable);
}

DEATH_TEST(HostAstNodeVTableDeathTest, InvalidAstNodeVTableFailsGetProperties, "517") {
    auto vtable = host::HostAggStageAstNodeAdapter::getVTable();
    vtable.get_properties = nullptr;
    AggStageAstNodeAPI::assertVTableConstraints(vtable);
}

DEATH_TEST(HostAstNodeVTableDeathTest, InvalidAstNodeVTableFailsPromote, "517") {
    auto vtable = host::HostAggStageAstNodeAdapter::getVTable();
    vtable.promote = nullptr;
    AggStageAstNodeAPI::assertVTableConstraints(vtable);
}

DEATH_TEST(HostAstNodeVTableDeathTest,
           InvalidAstNodeVTableFailsGetFirstStageViewApplicationPolicy,
           "517") {
    auto vtable = host::HostAggStageAstNodeAdapter::getVTable();
    vtable.get_first_stage_view_application_policy = nullptr;
    AggStageAstNodeAPI::assertVTableConstraints(vtable);
}

DEATH_TEST(HostAstNodeVTableDeathTest, InvalidAstNodeVTableFailsBindResolvedNamespace, "517") {
    auto vtable = host::HostAggStageAstNodeAdapter::getVTable();
    vtable.bind_resolved_namespace = nullptr;
    AggStageAstNodeAPI::assertVTableConstraints(vtable);
}

DEATH_TEST(HostAstNodeTestDeathTest, HostGetPropertiesUnimplemented, "11347801") {
    auto noOpAstNode = new host::HostAggStageAstNodeAdapter(makeIdLookupAstNode());
    auto handle = AggStageAstNodeHandle{noOpAstNode};

    ::MongoExtensionByteBuf** buf = nullptr;
    handle.get()->vtable->get_properties(noOpAstNode, buf);
}

DEATH_TEST(HostAstNodeTestDeathTest, HostPromoteUnimplemented, "11133600") {
    auto noOpAstNode = new host::HostAggStageAstNodeAdapter(makeIdLookupAstNode());
    auto handle = AggStageAstNodeHandle{noOpAstNode};

    ::MongoExtensionLogicalAggStage** bind = nullptr;
    handle.get()->vtable->promote(noOpAstNode, nullptr, bind);
}

}  // namespace
}  // namespace mongo::extension
