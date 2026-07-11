// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/extension/host/document_source_extension_optimizable.h"
#include "mongo/db/extension/host_connector/adapter/host_portal_adapter.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/stage_descriptor.h"
#include "mongo/db/extension/shared/handle/pipeline_rewrite_context_handle.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo::extension::host {

/**
 * Concrete HostPortal used at extension load time. Registers each extension's stage descriptors
 * with DocumentSourceExtensionOptimizable so the server can parse and optimize those stages.
 */
class HostPortal : public host_connector::HostPortalBase {
public:
    void registerStageDescriptor(
        const ::MongoExtensionAggStageDescriptor* descriptor) const override {
        tassert(ErrorCodes::ExtensionError,
                "Got null stage descriptor during extension registration",
                descriptor != nullptr);
        host::DocumentSourceExtensionOptimizable::registerStage(
            AggStageDescriptorHandle(descriptor));
    };

    void registerStageRules(MongoExtensionByteView stageName,
                            const MongoExtensionPipelineRewriteRule* rules,
                            size_t numRules) const override {
        std::string_view name(reinterpret_cast<const char*>(stageName.data), stageName.len);
        std::vector<PipelineRewriteRule> extensionStageRules;
        extensionStageRules.reserve(numRules);
        for (size_t i = 0; i < numRules; i++) {
            std::string ruleName(reinterpret_cast<const char*>(rules[i].name.data),
                                 rules[i].name.len);
            PipelineRewriteRule rule(ruleName, rules[i].tags);
            extensionStageRules.push_back(rule);
        }
        host::DocumentSourceExtensionOptimizable::registerStageRules(
            name, std::move(extensionStageRules));
    };
};

}  // namespace mongo::extension::host
