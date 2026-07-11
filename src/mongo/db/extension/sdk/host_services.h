// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/sdk/logger.h"
#include "mongo/db/extension/shared/extension_status.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/parse_node.h"
#include "mongo/db/extension/shared/handle/handle.h"
#include "mongo/util/modules.h"

namespace mongo::extension {
namespace sdk {
class IdleThreadBlockAPI;
class HostServicesAPI;
}  // namespace sdk

template <>
struct c_api_to_cpp_api<::MongoExtensionIdleThreadBlock> {
    using CppApi_t = sdk::IdleThreadBlockAPI;
};

template <>
struct c_api_to_cpp_api<::MongoExtensionHostServices> {
    using CppApi_t = sdk::HostServicesAPI;
};

namespace sdk {

/**
 * Wrapper for ::MongoExtensionIdleThreadBlock, providing access to an 'IdleThreadBlock' object
 * constructed by the host-side adapter, marking a spawned thread as idle in gdb.
 *
 * Typically, ownership of the 'IdleThreadBlock' object is transferred to the extension by the host,
 * so this API should be referenced via an OwnedHandle. This ensures that the said object remains
 * valid for as long as the handle lifetime is managed by still in scope.
 */
class IdleThreadBlockAPI : public VTableAPI<::MongoExtensionIdleThreadBlock> {
public:
    IdleThreadBlockAPI(::MongoExtensionIdleThreadBlock* ptr)
        : VTableAPI<::MongoExtensionIdleThreadBlock>(ptr) {}

    static void assertVTableConstraints(const VTable_t& vtable) {}
};

using IdleThreadBlockHandle = OwnedHandle<::MongoExtensionIdleThreadBlock>;
using HostServicesHandle = UnownedHandle<const ::MongoExtensionHostServices>;

/**
 * Wrapper for ::MongoExtensionHostServices, providing safe access to its public API through the
 * underlying vtable.
 *
 * The host services pointer is expected to be valid for the lifetime of the extension and is
 * statically accessible via HostServicesAPI::getInstance()
 *
 * The HostServices pointer remains fully owned by the Host, and ownership is never transferred to
 * the extension, so this API should only be referenced via an UnownedHandle.
 */
class HostServicesAPI : public VTableAPI<::MongoExtensionHostServices> {
public:
    HostServicesAPI(::MongoExtensionHostServices* services)
        : VTableAPI<::MongoExtensionHostServices>(services) {}

    ::MongoExtensionStatus* userAsserted(::MongoExtensionByteView structuredErrorMessage) const {
        return _vtable().user_asserted(structuredErrorMessage);
    }

    ::MongoExtensionStatus* tripwireAsserted(
        ::MongoExtensionByteView structuredErrorMessage) const {
        return _vtable().tripwire_asserted(structuredErrorMessage);
    }

    static UnownedHandle<const ::MongoExtensionHostServices>& getInstance() {
        return _sHostServices;
    }

    IdleThreadBlockHandle markIdleThread(const char* location) const {
        ::MongoExtensionIdleThreadBlock* idleThreadBlock = nullptr;
        invokeCAndConvertStatusToException(
            [&] { return _vtable().mark_idle_thread_block(&idleThreadBlock, location); });

        return IdleThreadBlockHandle{idleThreadBlock};
    }
    /**
     * Sets the static HostServices instance. Called during get_mongodb_extension() so that
     * sdk_uassert/sdk_tassert are available from the start of extension loading. The host
     * guarantees the pointer remains valid for the lifetime of the extension.
     */
    static void setHostServices(const ::MongoExtensionHostServices* services) noexcept {
        // The host should only call this function once.
        _sHostServices = UnownedHandle<const ::MongoExtensionHostServices>{services};
    }

    AggStageParseNodeHandle createHostAggStageParseNode(BSONObj spec) const {
        ::MongoExtensionAggStageParseNode* result = nullptr;
        invokeCAndConvertStatusToException([&] {
            return _vtable().create_host_agg_stage_parse_node(objAsByteView(spec), &result);
        });
        return AggStageParseNodeHandle{result};
    }

    AggStageAstNodeHandle createIdLookup(BSONObj spec) const {
        ::MongoExtensionAggStageAstNode* result = nullptr;
        invokeCAndConvertStatusToException(
            [&] { return _vtable().create_id_lookup(objAsByteView(spec), &result); });
        return AggStageAstNodeHandle{result};
    }

    AggStageAstNodeHandle createDocumentResultsAndMetadata(
        BSONObj spec,
        ::MongoExtensionDocResultsDPLCallback dplCallback = nullptr,
        void* dplCallbackUserData = nullptr,
        void (*dplCallbackDestroy)(void*) = nullptr) const {
        ::MongoExtensionAggStageAstNode* result = nullptr;
        invokeCAndConvertStatusToException([&] {
            return _vtable().create_document_results_and_metadata(
                objAsByteView(spec), dplCallback, dplCallbackUserData, dplCallbackDestroy, &result);
        });
        return AggStageAstNodeHandle{result};
    }

    LoggerHandle getLogger() const {
        return LoggerHandle(_vtable().get_logger());
    }

    static void assertVTableConstraints(const VTable_t& vtable);

private:
    static UnownedHandle<const ::MongoExtensionHostServices> _sHostServices;
};
/**
 * These macros are used to get 'file:line' as a const char*. You should only be calling
 * MONGO_EXTENSION_IDLE_LOCATION as a parameter to 'markIdleThread'.
 */
#define MONGO_EXTENSION_IDLE_LOCATION_STR1_(x) #x
#define MONGO_EXTENSION_IDLE_LOCATION_STR_(x) MONGO_EXTENSION_IDLE_LOCATION_STR1_(x)
#define MONGO_EXTENSION_IDLE_LOCATION __FILE__ ":" MONGO_EXTENSION_IDLE_LOCATION_STR_(__LINE__)
}  // namespace sdk
}  // namespace mongo::extension
