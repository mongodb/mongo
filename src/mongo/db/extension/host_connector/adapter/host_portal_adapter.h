// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/extension/public/api.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string>

namespace mongo::extension::host_connector {

/**
 * Abstract base for the host-side implementation of the extension HostPortal. The host connector
 * wraps a concrete implementation in HostPortalAdapter to satisfy the C API
 * (MongoExtensionHostPortal) passed to extensions during initialize().
 */
class HostPortalBase {
public:
    virtual ~HostPortalBase() = default;
    virtual void registerStageDescriptor(const ::MongoExtensionAggStageDescriptor*) const = 0;
    virtual void registerStageRules(::MongoExtensionByteView stageName,
                                    const ::MongoExtensionPipelineRewriteRule* rules,
                                    size_t numRules) const = 0;
};

/**
 * Adapts a C++ HostPortalBase to the C MongoExtensionHostPortal passed to extensions during
 * initialize(). Forwards register_stage_descriptor and get_extension_options to the wrapped
 * implementation. Owns the HostPortalBase and the serialized extension options.
 */
class HostPortalAdapter final : public ::MongoExtensionHostPortal {
public:
    HostPortalAdapter(::MongoExtensionAPIVersion apiVersion,
                      int maxWireVersion,
                      std::string extensionOptions,
                      std::unique_ptr<HostPortalBase> portal)
        : ::MongoExtensionHostPortal{&VTABLE, apiVersion, maxWireVersion},
          _extensionOpts(std::move(extensionOptions)),
          _portal(std::move(portal)) {
        tassert(11417104, "Provided HostPortalBase is null", _portal != nullptr);
    }

    HostPortalAdapter(const HostPortalAdapter&) = delete;
    HostPortalAdapter& operator=(const HostPortalAdapter&) = delete;
    HostPortalAdapter(HostPortalAdapter&&) = delete;
    HostPortalAdapter& operator=(HostPortalAdapter&&) = delete;

    const HostPortalBase& getImpl() const {
        return *_portal;
    }

private:
    static ::MongoExtensionStatus* _extRegisterStageDescriptor(
        const MongoExtensionHostPortal* hostPortal,
        const MongoExtensionAggStageDescriptor* stageDesc) noexcept;

    static ::MongoExtensionByteView _extGetOptions(
        const ::MongoExtensionHostPortal* portal) noexcept;

    static ::MongoExtensionStatus* _extRegisterStageRules(
        const ::MongoExtensionHostPortal* hostPortal,
        ::MongoExtensionByteView stageName,
        const ::MongoExtensionPipelineRewriteRule* rules,
        size_t numRules) noexcept;

    static constexpr ::MongoExtensionHostPortalVTable VTABLE = {
        .register_stage_descriptor = &_extRegisterStageDescriptor,
        .get_extension_options = &_extGetOptions,
        .register_stage_rules = &_extRegisterStageRules,
    };

    const std::string _extensionOpts;
    std::unique_ptr<HostPortalBase> _portal;
};

}  // namespace mongo::extension::host_connector
