// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/api_version_vector_to_span.h"
#include "mongo/db/extension/sdk/assert_util.h"
#include "mongo/db/extension/sdk/host_portal.h"
#include "mongo/db/extension/sdk/host_services.h"
#include "mongo/db/extension/sdk/versioned_extension.h"
#include "mongo/db/extension/shared/extension_status.h"
#include "mongo/db/extension/shared/handle/pipeline_rewrite_context_handle.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/modules.h"

#include <memory>

namespace mongo::extension::sdk {

/**
 * To register a new extension, extension writers should derive from this class and implement
 * initialize() to register aggregation stages. See test_examples/foo.cpp for an example.
 */
class Extension {
public:
    virtual ~Extension() = default;

    virtual void initialize(const HostPortalHandle& portal) = 0;

protected:
    template <class StageDescriptor>
    void _registerStage(const HostPortalHandle& portal) {
        // Error out if StageDescriptor is already registered to this extension.
        sdk_uassert(10696402,
                    (str::stream() << StageDescriptor::kStageName << " is already registered"),
                    _stageDescriptors.find(StageDescriptor::kStageName) == _stageDescriptors.end());

        auto stageDesc = std::make_unique<ExtensionAggStageDescriptorAdapter>(
            std::make_unique<StageDescriptor>());

        portal->registerStageDescriptor(stageDesc.get());

        _stageDescriptors.emplace(StageDescriptor::kStageName, std::move(stageDesc));
    }

    template <class StageDescriptor>
    void _registerStageRules(const HostPortalHandle& portal,
                             const std::vector<PipelineRewriteRule>& rules) {
        sdk_uassert(12201400, "No rules to register", !rules.empty());
        stdx::unordered_set<std::string> seenRuleNames;
        for (const auto& rule : rules) {
            sdk_uassert(12201404,
                        "Cannot register rule with duplicate rule name for same stage",
                        seenRuleNames.insert(rule.name).second);
        }
        portal->registerStageRules(StageDescriptor::kStageName, rules);
    }

private:
    stdx::unordered_map<std::string, std::unique_ptr<ExtensionAggStageDescriptorAdapter>>
        _stageDescriptors;
    stdx::unordered_map<std::string, std::unique_ptr<ExtensionAggStageDescriptorAdapter>> _rules;
};

/**
 * Adapter from a C++ MongoExtension to the C API MongoExtension struct.
 */
class ExtensionAdapter final : public ::MongoExtension {
public:
    ExtensionAdapter(std::unique_ptr<sdk::Extension> extensionPointer,
                     ::MongoExtensionAPIVersion version)
        : ::MongoExtension{&VTABLE, version}, _extensionPointer(std::move(extensionPointer)) {
        sdk_tassert(11417101, "Provided Extension is null", _extensionPointer != nullptr);
    }

    ExtensionAdapter(const VersionedExtension& versionedExtension)
        : ::MongoExtension{&VTABLE, versionedExtension.version},
          _extensionPointer(versionedExtension.factoryFunc()) {
        sdk_tassert(11417102, "Provided Extension is null", _extensionPointer != nullptr);
    }

    ~ExtensionAdapter() = default;

    // ExtensionAdapter is non-copyable and non-movable, as adapters should be heap-allocated, and
    // managed via a unique_ptr or Handle. This property guarantees that the adapter's underlying
    // implementation pointer remains valid for object's lifetime. The same is true for all
    // adapters.
    ExtensionAdapter(const ExtensionAdapter&) = delete;
    ExtensionAdapter& operator=(const ExtensionAdapter&) = delete;
    ExtensionAdapter(ExtensionAdapter&&) = delete;
    ExtensionAdapter& operator=(ExtensionAdapter&&) = delete;

private:
    static ::MongoExtensionStatus* _extInitialize(
        const ::MongoExtension* extensionPtr, const ::MongoExtensionHostPortal* portal) noexcept {
        return wrapCXXAndConvertExceptionToStatus([&]() {
            // The host portal will go out of scope on the host side after initialization, so we
            // should not retain it to avoid a dangling pointer
            auto hostPortal = HostPortalHandle(portal);
            static_cast<const sdk::ExtensionAdapter*>(extensionPtr)
                ->_extensionPointer->initialize(hostPortal);
        });
    }
    static constexpr ::MongoExtensionVTable VTABLE = {
        .initialize = &_extInitialize,
    };
    std::unique_ptr<sdk::Extension> _extensionPointer;
};

/**
 * Registers extension type with an API version attached.
 */
#define REGISTER_EXTENSION_WITH_VERSION(ExtensionType, ApiVersion)                             \
    namespace {                                                                                \
    struct ExtensionType##Registrar {                                                          \
        ExtensionType##Registrar() {                                                           \
            mongo::extension::sdk::VersionedExtensionContainer::getInstance().registerVersion( \
                ApiVersion, []() { return std::make_unique<ExtensionType>(); });               \
        }                                                                                      \
    };                                                                                         \
    static ExtensionType##Registrar ExtensionType##RegistrarInstance;                          \
    }

/**
 * Default case. Registers MyExtension with MONGODB_EXTENSION_API_VERSION.
 */
#define REGISTER_EXTENSION(MyExtensionType) \
    REGISTER_EXTENSION_WITH_VERSION(MyExtensionType, (MONGODB_EXTENSION_API_VERSION))

/**
 * Base case macro to define both halves of the two-phase load protocol:
 *
 *   - get_mongodb_extension_versions: publishes the registered set of supported API versions.
 *     This runs BEFORE the host has selected a version, so no HostServices is available and
 *     exceptions MUST NOT escape across the C boundary. The body is wrapped in a noexcept
 *     try/catch that swallows any thrown exception, leaving 'len=0' so the host treats it
 *     as "extension supports no versions" and rejects in negotiation.
 *
 *   - get_mongodb_extension: instantiates the extension at the host-selected version.
 *     HostServices is delivered with this call and is laid out per the chosen version's
 *     major, so 'setHostServices' is safe before the wrap and sdk_uassert/sdk_tassert
 *     work normally inside the lambda.
 */
#define DEFINE_GET_EXTENSION()                                                                     \
    extern "C" {                                                                                   \
    void get_mongodb_extension_versions(                                                           \
        ::MongoExtensionAPIVersionVector* extensionVersions) noexcept {                            \
        extensionVersions->len = 0;                                                                \
        extensionVersions->versions = nullptr;                                                     \
        try {                                                                                      \
            /* The static vector of versions is materialized once, on the first call, from the */  \
            /* singleton container. This is safe, given that version negotiation happens once  */  \
            /* during an extension's lifetime, at which point all versions have already been   */  \
            /* registered. */                                                                      \
            static const std::vector<::MongoExtensionAPIVersion> kVersions =                       \
                mongo::extension::sdk::VersionedExtensionContainer::getInstance()                  \
                    .getVersionsList();                                                            \
            extensionVersions->len = kVersions.size();                                             \
            extensionVersions->versions = kVersions.data();                                        \
        } catch (...) {                                                                            \
            /* Swallow error. No HostServices available, no error-reporting path. Host will see */ \
            /* len=0 and reject during negotiation. */                                             \
            extensionVersions->len = 0;                                                            \
            extensionVersions->versions = nullptr;                                                 \
        }                                                                                          \
    }                                                                                              \
                                                                                                   \
    ::MongoExtensionStatus* get_mongodb_extension(                                                 \
        ::MongoExtensionAPIVersion version,                                                        \
        const ::MongoExtensionHostServices* hostServices,                                          \
        const ::MongoExtension** extension) {                                                      \
        mongo::extension::sdk::HostServicesAPI::setHostServices(hostServices);                     \
        return mongo::extension::wrapCXXAndConvertExceptionToStatus([&] {                          \
            const auto& container =                                                                \
                mongo::extension::sdk::VersionedExtensionContainer::getInstance();                 \
            static auto wrapper = std::make_unique<mongo::extension::sdk::ExtensionAdapter>(       \
                container.getVersionedExtension(version));                                         \
            *extension = reinterpret_cast<const ::MongoExtension*>(wrapper.get());                 \
        });                                                                                        \
    }                                                                                              \
    }

}  // namespace mongo::extension::sdk
