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
#include "mongo/db/extension/host_connector/adapter/logger_adapter.h"

#include "mongo/db/extension/shared/byte_buf_utils.h"
#include "mongo/db/extension/shared/extension_status.h"
#include "mongo/logv2/attribute_storage.h"

namespace mongo::extension::host_connector {

// Initialize the static instance of LoggerAdapter.
LoggerAdapter LoggerAdapter::_loggerAdapter;

namespace {
logv2::DynamicAttributes createLogAttributes(const ::MongoExtensionLogMessage* logMessage) {
    logv2::DynamicAttributes attrs;
    if (logMessage->attributes.size == 0) {
        return attrs;
    }

    BSONObjBuilder builder;
    for (size_t i = 0; i < logMessage->attributes.size; i++) {
        auto nameView = byteViewAsStringView(logMessage->attributes.elements[i].name);
        auto valueView = byteViewAsStringView(logMessage->attributes.elements[i].value);
        builder.append(std::string(nameView), std::string(valueView));
    }

    attrs.add("attr", builder.obj());
    return attrs;
}
}  // namespace

MongoExtensionStatus* LoggerAdapter::_extLog(
    const ::MongoExtensionLogMessage* logMessage) noexcept {
    return wrapCXXAndConvertExceptionToStatus([&]() {
        // For now we always log extension messages under the EXTENSION-MONGOT component. Someday
        // we'd like to dynamically create EXTENSION sub-components per extension.
        logv2::LogOptions options(logv2::LogComponent::kExtensionMongot);

        // Extract message from byte view.
        auto messageView = byteViewAsStringView(logMessage->message);
        StringData message(messageView.data(), messageView.size());

        // Extract code.
        std::int32_t code = static_cast<std::int32_t>(logMessage->code);

        logv2::LogSeverity severity = logv2::LogSeverity::Error();  // Dummy initialization
        switch (logMessage->type) {
            case ::MongoExtensionLogType::kDebug: {
                // Extract level from union and trim to the range [1, 5] since we want to make sure
                // that the log line is using one of the server's logv2 debug severities.
                std::int32_t level = logMessage->severityOrLevel.level;
                severity = logv2::LogSeverity::Debug(std::min(5, std::max(1, level)));
                break;
            }
            case ::MongoExtensionLogType::kLog: {
                // Convert C enum to logv2 severity.
                severity = convertSeverity(logMessage->severityOrLevel.severity);
                break;
            }
        };

        logv2::DynamicAttributes dynamicAttrs = createLogAttributes(logMessage);
        logv2::TypeErasedAttributeStorage attrs(dynamicAttrs);

        // We must go through logv2::detail::doLogImpl since the LOGV2 macros expect a static string
        // literal for the message, but we have to log the message received at runtime from the
        // extension.
        logv2::detail::doLogImpl(code, severity, options, message, attrs);
    });
}

MongoExtensionStatus* LoggerAdapter::_extShouldLog(::MongoExtensionLogSeverity levelOrSeverity,
                                                   ::MongoExtensionLogType logType,
                                                   bool* out) noexcept {
    return extension::wrapCXXAndConvertExceptionToStatus([&]() {
        logv2::LogSeverity severity = logv2::LogSeverity::Debug(5);  // Dummy initialization.
        if (logType == ::MongoExtensionLogType::kDebug) {
            severity =
                logv2::LogSeverity::Debug(std::min(5, std::max(1, int32_t(levelOrSeverity))));
        } else {
            severity = convertSeverity(levelOrSeverity);
        }
        bool result = logv2::shouldLog(logv2::LogComponent::kExtensionMongot, severity);
        *out = result;
    });
}
}  // namespace mongo::extension::host_connector
