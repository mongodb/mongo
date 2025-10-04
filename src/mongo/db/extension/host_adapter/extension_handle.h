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

#include "mongo/db/extension/host/host_portal.h"
#include "mongo/db/extension/host/host_services.h"
#include "mongo/db/extension/host_adapter/extension_status.h"
#include "mongo/db/extension/host_adapter/handle.h"
#include "mongo/db/extension/public/api.h"
#include "mongo/util/modules.h"

namespace mongo::extension::host_adapter {

/**
 * Wrapper for ::MongoExtension providing safe access to its public API via the vtable.
 * This is an unowned handle, meaning extensions remain fully owned by themselves, and ownership
 * is never transferred to the host.
 */
class ExtensionHandle : public UnownedHandle<const ::MongoExtension> {

public:
    ExtensionHandle(const ::MongoExtension* ext) : UnownedHandle<const ::MongoExtension>(ext) {
        _assertValidVTable();
    }

    /**
     * Initialize the extension by providing it with a HostPortal and HostServices.
     *
     * Note that the HostServices is passed as a pointer since its lifetime extends beyond
     * the call to initialize() and will be saved by the extension. The HostPortal, on the other
     * hand, will go out of scope immediately after the call to initialize() so it is passed by
     * reference.
     */
    void initialize(const extension::host::HostPortal& portal,
                    const extension::host::HostServices* hostServices) const {
        sdk::enterC([&] {
            assertValid();
            return vtable().initialize(get(), &portal, hostServices);
        });
    }

    ::MongoExtensionAPIVersion getVersion() const {
        assertValid();
        return get()->version;
    }

protected:
    void _assertVTableConstraints(const VTable_t& vtable) const override {
        tassert(10930101, "Extension 'initialize' is null", vtable.initialize != nullptr);
    };
};

}  // namespace mongo::extension::host_adapter
