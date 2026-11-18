// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/assert_util.h"
#include "mongo/db/extension/sdk/command.h"
#include "mongo/db/extension/shared/extension_status.h"
#include "mongo/db/extension/shared/handle/command/command_descriptor.h"
#include "mongo/db/extension/shared/handle/pipeline_rewrite_context_handle.h"
#include "mongo/util/modules.h"
#include "mongo/util/str.h"

#include <string_view>

#include <yaml-cpp/yaml.h>

namespace mongo::extension {

namespace sdk {
class HostPortalAPI;
}
template <>
struct c_api_to_cpp_api<::MongoExtensionHostPortal> {
    using CppApi_t = sdk::HostPortalAPI;
};
namespace sdk {
using HostPortalHandle = UnownedHandle<const ::MongoExtensionHostPortal>;

/**
 * Wrapper for ::MongoExtensionHostPortal providing safe access to its public API via the
 * vtable.
 *
 * The HostPortal always remains fully owned by the host, and ownership is never transferred to the
 * extension. Therefore, the extension should only use this API via an UnownedHandle.
 *
 * Note that the host portal pointer is only valid during initialization and should not be
 * retained by the extension.
 */
class HostPortalAPI : public VTableAPI<::MongoExtensionHostPortal> {
public:
    HostPortalAPI(::MongoExtensionHostPortal* portal, const CapabilityVersion& capability)
        : VTableAPI<::MongoExtensionHostPortal>(portal, capability) {}

    void registerStageDescriptor(const ExtensionAggStageDescriptorAdapter* stageDesc) const {
        invokeCAndConvertStatusToException([&] {
            return _vtable().register_stage_descriptor(
                get(), reinterpret_cast<const ::MongoExtensionAggStageDescriptor*>(stageDesc));
        });
    }

    void registerStageRules(std::string_view stageName,
                            const std::vector<PipelineRewriteRule>& rules) const {
        ::MongoExtensionByteView stageNameView{reinterpret_cast<const uint8_t*>(stageName.data()),
                                               stageName.size()};
        std::vector<::MongoExtensionPipelineRewriteRule> cRules;
        cRules.reserve(rules.size());
        for (const auto& rule : rules) {
            cRules.push_back(rule.convertToCRule());
        }
        invokeCAndConvertStatusToException([&] {
            return _vtable().register_stage_rules(
                get(), stageNameView, cRules.data(), cRules.size());
        });
    }

    void registerCommandDescriptor(const ExtensionCommandDescriptorAdapter* commandDesc) const {
        sdk_uassert(13730000,
                    (str::stream() << "Extension commands require API version 1.1, but the host "
                                      "negotiated "
                                   << _getCapabilityVersion().toString()),
                    _getCapabilityVersion() >= CommandDescriptorAPI::kMinSupportedCapability);
        invokeCAndConvertStatusToException(
            [&] { return _vtable().register_command_descriptor(get(), commandDesc); });
    }

    ::MongoExtensionAPIVersion getHostExtensionsAPIVersion() const {
        assertValid();
        return get()->hostExtensionsAPIVersion;
    }

    int getHostMongoDBMaxWireVersion() const {
        assertValid();
        return get()->hostMongoDBMaxWireVersion;
    }

    YAML::Node getExtensionOptions() const {
        return YAML::Load(
            std::string(byteViewAsStringView(_vtable().get_extension_options(get()))));
    }

    static void assertVTableConstraints(const VTable_t& vtable,
                                        const CapabilityVersion& capability) {
        sdk_tassert(10926401,
                    "Extension 'register_stage_descriptor' is null",
                    vtable.register_stage_descriptor != nullptr);
        sdk_tassert(10999108,
                    "Extension 'get_extension_options' is null",
                    vtable.get_extension_options != nullptr);
        sdk_tassert(12201401,
                    "Extension 'register_stage_rules' is null",
                    vtable.register_stage_rules != nullptr);
        if (capability >= CommandDescriptorAPI::kMinSupportedCapability) {
            sdk_tassert(13730001,
                        "Extension 'register_command_descriptor' is null",
                        vtable.register_command_descriptor != nullptr);
        }
    };
};
}  // namespace sdk

}  // namespace mongo::extension
