// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/extension/host_connector/adapter/pipeline_rewrite_context_adapter.h"

#include "mongo/db/extension/host/pipeline_rewrite_context.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/logical.h"

#include <string_view>

namespace mongo::extension::host_connector {
MongoExtensionStatus* PipelineRewriteContextAdapter::_hostGetNthNextStage(
    const MongoExtensionPipelineRewriteContext* ctx,
    size_t index,
    MongoExtensionLogicalAggStage** out) {
    return wrapCXXAndConvertExceptionToStatus([&]() {
        *out = nullptr;

        const PipelineRewriteContextAdapter* pipelineRewriteContextAdapter =
            static_cast<const PipelineRewriteContextAdapter*>(ctx);
        const auto& pipelineRewriteCtx = pipelineRewriteContextAdapter->getCtxImpl();

        boost::intrusive_ptr<DocumentSource> ds = pipelineRewriteCtx.getNthNextStage(index);

        pipelineRewriteContextAdapter->cachedNextStageResult =
            std::make_unique<HostLogicalAggStageAdapter>(host::LogicalAggStage::make(ds.get()));
        *out = static_cast<MongoExtensionLogicalAggStage*>(
            pipelineRewriteContextAdapter->cachedNextStageResult.get());
    });
}

MongoExtensionStatus* PipelineRewriteContextAdapter::_hostEraseNthNextStage(
    MongoExtensionPipelineRewriteContext* ctx, size_t index, bool* result) {
    return wrapCXXAndConvertExceptionToStatus([&]() {
        auto& pipelineRewriteCtx = static_cast<PipelineRewriteContextAdapter*>(ctx)->getCtxImpl();
        *result = pipelineRewriteCtx.eraseNthNext(index);
    });
}

MongoExtensionStatus* PipelineRewriteContextAdapter::_hostHasAtLeastNNextStages(
    const MongoExtensionPipelineRewriteContext* ctx, size_t n, bool* result) {
    return wrapCXXAndConvertExceptionToStatus([&]() {
        const auto& pipelineRewriteCtx =
            static_cast<const PipelineRewriteContextAdapter*>(ctx)->getCtxImpl();
        *result = pipelineRewriteCtx.hasAtLeastNNextStages(n);
    });
}

MongoExtensionStatus* PipelineRewriteContextAdapter::_hostGetPipelineSuffixBounds(
    const MongoExtensionPipelineRewriteContext* ctx, MongoExtensionDocsNeededBounds* out) {
    return wrapCXXAndConvertExceptionToStatus([&]() {
        const auto& hostCtx = static_cast<const PipelineRewriteContextAdapter*>(ctx)->getCtxImpl();
        DocsNeededBounds bounds = hostCtx.getPipelineSuffixBounds();

        auto convertConstraint =
            [](const DocsNeededConstraint& c) -> MongoExtensionDocsNeededConstraint {
            return visit(OverloadedVisitor{
                             [](long long v) -> MongoExtensionDocsNeededConstraint {
                                 return {kDocsNeededConstraintDiscrete, static_cast<uint64_t>(v)};
                             },
                             [](docs_needed_bounds::NeedAll) -> MongoExtensionDocsNeededConstraint {
                                 return {kDocsNeededConstraintNeedAll, 0};
                             },
                             [](docs_needed_bounds::Unknown) -> MongoExtensionDocsNeededConstraint {
                                 return {kDocsNeededConstraintUnknown, 0};
                             },
                         },
                         c);
        };

        *out = {convertConstraint(bounds.getMinBounds()), convertConstraint(bounds.getMaxBounds())};
    });
}

using PipelineRewriteContext = rule_based_rewrites::pipeline::PipelineRewriteContext;
using HostPipelineRewriteRule = rule_based_rewrites::pipeline::PipelineRewriteRule;

HostPipelineRewriteRule wrapExtensionRule(const extension::PipelineRewriteRule& extRule,
                                          UnownedLogicalAggStageHandle extensionStage) {
    using namespace rule_based_rewrites::pipeline;
    rule_based_rewrites::TagSet tags = 0;
    if (extRule.tags & kPipelineRewriteRuleTagReordering) {
        tags |= static_cast<rule_based_rewrites::TagSet>(PipelineRewriteContext::Tags::Reordering);
    }
    if (extRule.tags & kPipelineRewriteRuleTagInPlace) {
        tags |= static_cast<rule_based_rewrites::TagSet>(PipelineRewriteContext::Tags::InPlace);
    }

    const auto& ruleName = extRule.name;
    return HostPipelineRewriteRule{
        .name = ruleName,
        .precondition = [extensionStage, ruleName](PipelineRewriteContext& ctx) -> bool {
            auto hostCtx = host::PipelineRewriteContext::make(&ctx);
            auto hostAdapter =
                std::make_unique<host_connector::PipelineRewriteContextAdapter>(std::move(hostCtx));
            return extensionStage->evaluatePipelineRewriteRulePrecondition(
                std::string_view(ruleName.data(), ruleName.size()), hostAdapter.get());
        },
        .transform = [extensionStage, ruleName](PipelineRewriteContext& ctx) mutable -> bool {
            auto hostCtx = host::PipelineRewriteContext::make(&ctx);
            auto hostAdapter =
                std::make_unique<host_connector::PipelineRewriteContextAdapter>(std::move(hostCtx));
            return extensionStage->evaluatePipelineRewriteRuleTransform(
                std::string_view(ruleName.data(), ruleName.size()), hostAdapter.get());
        },
        .tags = tags,
    };
}

}  // namespace mongo::extension::host_connector
