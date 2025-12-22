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
#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/assert_util.h"
#include "mongo/db/extension/shared/extension_status.h"
#include "mongo/util/modules.h"

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
    HostPortalAPI(::MongoExtensionHostPortal* portal)
        : VTableAPI<::MongoExtensionHostPortal>(portal) {}

    void registerStageDescriptor(const ExtensionAggStageDescriptor* stageDesc) const {
        invokeCAndConvertStatusToException([&] {
            return vtable().register_stage_descriptor(
                get(), reinterpret_cast<const ::MongoExtensionAggStageDescriptor*>(stageDesc));
        });
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
        return YAML::Load(std::string(byteViewAsStringView(vtable().get_extension_options(get()))));
    }

    static void assertVTableConstraints(const VTable_t& vtable) {
        sdk_tassert(10926401,
                    "Extension 'register_stage_descriptor' is null",
                    vtable.register_stage_descriptor != nullptr);
        sdk_tassert(10999108,
                    "Extension 'get_extension_options' is null",
                    vtable.get_extension_options != nullptr);
    };
};
}  // namespace sdk

}  // namespace mongo::extension
