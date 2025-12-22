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
        return vtable().user_asserted(structuredErrorMessage);
    }

    ::MongoExtensionStatus* tripwireAsserted(
        ::MongoExtensionByteView structuredErrorMessage) const {
        return vtable().tripwire_asserted(structuredErrorMessage);
    }

    static UnownedHandle<const ::MongoExtensionHostServices>& getInstance() {
        return _sHostServices;
    }

    IdleThreadBlockHandle markIdleThread(const char* location) const {
        ::MongoExtensionIdleThreadBlock* idleThreadBlock = nullptr;
        invokeCAndConvertStatusToException(
            [&] { return vtable().mark_idle_thread_block(&idleThreadBlock, location); });

        return IdleThreadBlockHandle{idleThreadBlock};
    }
    /**
     * setHostServices() should be called only once during initialization of the extension. The
     * host guarantees that the pointer remains valid during the lifetime of the extension.
     */
    static void setHostServices(const ::MongoExtensionHostServices* services) {
        // The host should only call this function once.
        _sHostServices = UnownedHandle<const ::MongoExtensionHostServices>{services};
    }

    AggStageParseNodeHandle createHostAggStageParseNode(BSONObj spec) const {
        ::MongoExtensionAggStageParseNode* result = nullptr;
        invokeCAndConvertStatusToException([&] {
            return vtable().create_host_agg_stage_parse_node(objAsByteView(spec), &result);
        });
        return AggStageParseNodeHandle{result};
    }

    AggStageAstNodeHandle createIdLookup(BSONObj spec) const {
        ::MongoExtensionAggStageAstNode* result = nullptr;
        invokeCAndConvertStatusToException(
            [&] { return vtable().create_id_lookup(objAsByteView(spec), &result); });
        return AggStageAstNodeHandle{result};
    }

    LoggerHandle getLogger() const {
        return LoggerHandle(vtable().get_logger());
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
