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
#include "mongo/db/extension/public/extension_log_gen.h"
#include "mongo/db/extension/sdk/byte_buf.h"
#include "mongo/db/extension/sdk/extension_status.h"
#include "mongo/db/extension/sdk/handle.h"
#include "mongo/util/modules.h"

namespace mongo::extension::sdk {
/**
 * Wrapper for ::MongoExtensionHostServices, providing safe access to its public API through the
 * underlying vtable.
 *
 * The host services pointer is expected to be valid for the lifetime of the extension and is
 * statically accessible via HostServicesHandle::getHostServices().
 *
 * This is an unowned handle, meaning the host services remain fully owned by the host, and
 * ownership is never transferred to the extension.
 */
class HostServicesHandle : public sdk::UnownedHandle<const ::MongoExtensionHostServices> {
public:
    HostServicesHandle(const ::MongoExtensionHostServices* services)
        : sdk::UnownedHandle<const ::MongoExtensionHostServices>(services) {}

    bool alwaysTrue_TEMPORARY() const {
        assertValid();
        sdk::enterC([&]() { return vtable().alwaysOK_TEMPORARY(); });
        return true;
    }

    static BSONObj createExtensionLogMessage(
        std::string message,
        std::int32_t code,
        mongo::extension::MongoExtensionLogSeverityEnum severity);

    static BSONObj createExtensionDebugLogMessage(std::string message,
                                                  std::int32_t code,
                                                  std::int32_t level);

    static HostServicesHandle* getHostServices() {
        return &_hostServices;
    }

    void log(std::string message,
             std::int32_t code,
             mongo::extension::MongoExtensionLogSeverityEnum severity =
                 mongo::extension::MongoExtensionLogSeverityEnum::kInfo) const {
        assertValid();

        BSONObj obj = createExtensionLogMessage(std::move(message), code, severity);
        sdk::enterC([&]() { return vtable().log(sdk::objAsByteView(obj)); });
    }

    /**
     * setHostServices() should be called only once during initialization of the extension. The host
     * guarantees that the pointer remains valid during the lifetime of the extension.
     */
    static void setHostServices(const ::MongoExtensionHostServices* services) {
        _hostServices = HostServicesHandle(services);
    }

private:
    static HostServicesHandle _hostServices;

    void _assertVTableConstraints(const VTable_t& vtable) const override {
        tassert(11097600,
                "Host services' 'alwaysOK_TEMPORARY' is null",
                vtable.alwaysOK_TEMPORARY != nullptr);
    };
};

}  // namespace mongo::extension::sdk
