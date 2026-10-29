// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/db/extension/host_connector/adapter/logger_adapter.h"

#include "mongo/db/extension/shared/byte_buf_utils.h"
#include "mongo/db/extension/shared/extension_status.h"
#include "mongo/logv2/attribute_storage.h"

#include <string_view>

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
        std::string_view message(messageView.data(), messageView.size());

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
        *out = logv2::shouldLog(logv2::LogComponent::kExtensionMongot, severity);
    });
}
}  // namespace mongo::extension::host_connector
