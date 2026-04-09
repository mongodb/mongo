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

#include "mongo/db/extension/host_connector/adapter/pipeline_rewrite_context_adapter.h"

#include "mongo/db/extension/host/pipeline_rewrite_context.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/logical.h"

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
    MongoExtensionPipelineRewriteContext* ctx, size_t index, bool* out) {
    return wrapCXXAndConvertExceptionToStatus([&]() {
        auto& pipelineRewriteCtx = static_cast<PipelineRewriteContextAdapter*>(ctx)->getCtxImpl();
        *out = pipelineRewriteCtx.eraseNthNext(index);
    });
}

MongoExtensionStatus* PipelineRewriteContextAdapter::_hostHasAtLeastNNextStages(
    const MongoExtensionPipelineRewriteContext* ctx, size_t n, bool* out) {
    return wrapCXXAndConvertExceptionToStatus([&]() {
        const auto& pipelineRewriteCtx =
            static_cast<const PipelineRewriteContextAdapter*>(ctx)->getCtxImpl();
        *out = pipelineRewriteCtx.hasAtLeastNNextStages(n);
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
            return extensionStage->evaluateRulePrecondition(
                StringData(ruleName.data(), ruleName.size()), hostAdapter.get());
        },
        .transform = [extensionStage, ruleName](PipelineRewriteContext& ctx) mutable -> bool {
            auto hostCtx = host::PipelineRewriteContext::make(&ctx);
            auto hostAdapter =
                std::make_unique<host_connector::PipelineRewriteContextAdapter>(std::move(hostCtx));
            return extensionStage->evaluateRuleTransform(
                StringData(ruleName.data(), ruleName.size()), hostAdapter.get());
        },
        .tags = tags,
    };
}

}  // namespace mongo::extension::host_connector
