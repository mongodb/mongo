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
#include "mongo/db/extension/host_adapter/host_services_adapter.h"

#include "mongo/db/extension/host/host_services.h"
#include "mongo/db/extension/public/extension_error_types_gen.h"
#include "mongo/db/extension/public/extension_log_gen.h"
#include "mongo/db/extension/shared/extension_status.h"

namespace mongo::extension::host_adapter {

// Initialize the static instance of HostServicesAdapter.
HostServicesAdapter HostServicesAdapter::_hostServicesAdapter;

MongoExtensionStatus* HostServicesAdapter::_extAlwaysOK_TEMPORARY() noexcept {
    return wrapCXXAndConvertExceptionToStatus(
        [&]() { return host::HostServices::alwaysTrue_TEMPORARY(); });
}

MongoExtensionStatus* HostServicesAdapter::_extLog(::MongoExtensionByteView logMessage) noexcept {
    return wrapCXXAndConvertExceptionToStatus([&]() {
        BSONObj obj = bsonObjFromByteView(logMessage);

        mongo::extension::MongoExtensionLog extensionLog =
            mongo::extension::MongoExtensionLog::parse(std::move(obj));

        return host::HostServices::log(extensionLog);
    });
}

MongoExtensionStatus* HostServicesAdapter::_extLogDebug(::MongoExtensionByteView rawLog) noexcept {
    return extension::wrapCXXAndConvertExceptionToStatus([&]() {
        BSONObj bsonLog = bsonObjFromByteView(rawLog);
        auto debugLog = mongo::extension::MongoExtensionDebugLog::parse(bsonLog);
        return host::HostServices::logDebug(std::move(debugLog));
    });
}

::MongoExtensionStatus* HostServicesAdapter::_extUserAsserted(
    ::MongoExtensionByteView structuredErrorMessage) {
    // We throw the exception here so that we get a stack trace that looks like a host exception but
    // originates from within the extension, so that we have a complete stack trace for diagnostic
    // information. At the same time, we are not allowed to throw an exception across the API
    // boundary, so we immediately convert this to a MongoExtensionStatus. It will be rethrown after
    // being passed through the boundary.
    return extension::wrapCXXAndConvertExceptionToStatus([&]() {
        BSONObj errorBson = bsonObjFromByteView(structuredErrorMessage);
        auto exceptionInfo = mongo::extension::ExtensionExceptionInformation::parse(
            errorBson, IDLParserContext("extUassert"));

        // Call the host's uassert implementation.
        uasserted(exceptionInfo.getErrorCode(),
                  "Extension encountered error: " + exceptionInfo.getMessage());
    });
}

::MongoExtensionStatus* HostServicesAdapter::_extTripwireAsserted(
    ::MongoExtensionByteView structuredErrorMessage) {
    // We follow the same throw-then-catch pattern here as in _extUserAsserted, for the same
    // reasons.
    return extension::wrapCXXAndConvertExceptionToStatus([&]() {
        BSONObj errorBson = bsonObjFromByteView(structuredErrorMessage);
        auto exceptionInfo = mongo::extension::ExtensionExceptionInformation::parse(
            errorBson, IDLParserContext("extTassert"));

        // Call the host's tassert implementation.
        tasserted(exceptionInfo.getErrorCode(),
                  "Extension encountered error: " + exceptionInfo.getMessage());
    });
}
}  // namespace mongo::extension::host_adapter
