// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/shared/extension_status.h"
#include "mongo/db/extension/shared/handle/handle.h"
#include "mongo/util/modules.h"

namespace mongo::extension {

namespace host_connector {
class ExtensionAPI;
}

template <>
struct c_api_to_cpp_api<::MongoExtension> {
    using CppApi_t = host_connector::ExtensionAPI;
};

namespace host_connector {

/**
 * Wrapper for ::MongoExtension providing safe access to its public API via the vtable.
 * This is an unowned handle, meaning extensions remain fully owned by themselves, and ownership
 * is never transferred to the host.
 */
class ExtensionAPI : public VTableAPI<::MongoExtension> {
public:
    ExtensionAPI(::MongoExtension* ext) : VTableAPI<::MongoExtension>(ext) {}

    /**
     * Initialize the extension by providing it with a HostPortal.
     *
     * The HostPortal pointer is only valid during the call to initialize() and must not be saved.
     */
    void initialize(const MongoExtensionHostPortal* portal) const {
        invokeCAndConvertStatusToException([&] { return _vtable().initialize(get(), portal); });
    }

    ::MongoExtensionAPIVersion getVersion() const {
        assertValid();
        return get()->version;
    }

    static void assertVTableConstraints(const VTable_t& vtable) {
        tassert(ErrorCodes::InvalidExtensionVTable,
                "Extension 'initialize' is null",
                vtable.initialize != nullptr);
    };
};

using ExtensionHandle = UnownedHandle<const ::MongoExtension>;
}  // namespace host_connector
}  // namespace mongo::extension
