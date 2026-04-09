/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
#pragma once

#include "mongo/db/extension/host/document_source_extension_optimizable.h"
#include "mongo/db/extension/host_connector/adapter/host_portal_adapter.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/stage_descriptor.h"
#include "mongo/db/extension/shared/handle/pipeline_rewrite_context_handle.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

namespace mongo::extension::host {

/**
 * Concrete HostPortal used at extension load time. Registers each extension's stage descriptors
 * with DocumentSourceExtensionOptimizable so the server can parse and optimize those stages.
 */
class HostPortal : public host_connector::HostPortalBase {
public:
    void registerStageDescriptor(
        const ::MongoExtensionAggStageDescriptor* descriptor) const override {
        tassert(10596400,
                "Got null stage descriptor during extension registration",
                descriptor != nullptr);
        host::DocumentSourceExtensionOptimizable::registerStage(
            AggStageDescriptorHandle(descriptor));
    };

    void registerStageRules(MongoExtensionByteView stageName,
                            const MongoExtensionPipelineRewriteRule* rules,
                            size_t numRules) const override {
        StringData name(reinterpret_cast<const char*>(stageName.data), stageName.len);
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
