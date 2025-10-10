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

#include <memory>
namespace mongo::extension::host_connector {
/**
 * HostServicesAdapter is an implementation of ::MongoExtensionHostServices, providing host
 * services to extensions.
 *
 * For each function in the MongoExtensionHostServicesVTable, this adapter has a corresponding
 * function that translates between the C API and the core implementation of the host services
 * provided primarily through mongo::extension::host::HostServices.
 *
 * The HostServicesAdapter instance is a singleton, and is accessible via
 * HostServicesAdapter::get(). The pointer to the singleton instance is passed to extensions
 * during initialization, and is expected to be valid for the lifetime of the extension.
 */
class HostServicesAdapter final : public ::MongoExtensionHostServices {
public:
    HostServicesAdapter() : ::MongoExtensionHostServices{&VTABLE} {}

    static HostServicesAdapter* get() {
        return &_hostServicesAdapter;
    }

private:
    static HostServicesAdapter _hostServicesAdapter;

    static MongoExtensionStatus* _extAlwaysOK_TEMPORARY() noexcept;

    static MongoExtensionStatus* _extLog(::MongoExtensionByteView logMessage) noexcept;

    static MongoExtensionStatus* _extLogDebug(::MongoExtensionByteView rawLog) noexcept;

    static ::MongoExtensionStatus* _extUserAsserted(
        ::MongoExtensionByteView structuredErrorMessage);

    static ::MongoExtensionStatus* _extTripwireAsserted(
        ::MongoExtensionByteView structuredErrorMessage);

    static constexpr ::MongoExtensionHostServicesVTable VTABLE{
        &_extAlwaysOK_TEMPORARY, &_extLog, &_extLogDebug, &_extUserAsserted, &_extTripwireAsserted};
};
}  // namespace mongo::extension::host_connector
