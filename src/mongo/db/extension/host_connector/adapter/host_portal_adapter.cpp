// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/db/extension/host_connector/adapter/host_portal_adapter.h"

#include "mongo/db/extension/shared/extension_status.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/stage_descriptor.h"

namespace mongo::extension::host_connector {

::MongoExtensionStatus* HostPortalAdapter::_extRegisterStageDescriptor(
    const MongoExtensionHostPortal* hostPortal,
    const MongoExtensionAggStageDescriptor* stageDesc) noexcept {
    return wrapCXXAndConvertExceptionToStatus([&]() {
        const auto& impl = static_cast<const HostPortalAdapter*>(hostPortal)->getImpl();
        impl.registerStageDescriptor(stageDesc);
    });
}

::MongoExtensionStatus* HostPortalAdapter::_extRegisterStageRules(
    const MongoExtensionHostPortal* hostPortal,
    ::MongoExtensionByteView stageName,
    const ::MongoExtensionPipelineRewriteRule* rules,
    size_t numRules) noexcept {
    return wrapCXXAndConvertExceptionToStatus([&]() {
        const auto& impl = static_cast<const HostPortalAdapter*>(hostPortal)->getImpl();
        impl.registerStageRules(stageName, rules, numRules);
    });
}

::MongoExtensionByteView HostPortalAdapter::_extGetOptions(
    const ::MongoExtensionHostPortal* portal) noexcept {
    return stringViewAsByteView(static_cast<const HostPortalAdapter*>(portal)->_extensionOpts);
}

}  // namespace mongo::extension::host_connector
