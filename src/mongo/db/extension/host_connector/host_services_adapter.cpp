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
#include "mongo/db/extension/host_connector/host_services_adapter.h"

#include "mongo/db/extension/host/aggregation_stage/parse_node.h"
#include "mongo/db/extension/public/extension_error_types_gen.h"
#include "mongo/db/extension/shared/byte_buf_utils.h"
#include "mongo/db/extension/shared/extension_status.h"
#include "mongo/logv2/attribute_storage.h"
#include "mongo/logv2/log_detail.h"
#include "mongo/logv2/log_options.h"

namespace mongo::extension::host_connector {

// Initialize the static instance of HostServicesAdapter.
HostServicesAdapter HostServicesAdapter::_hostServicesAdapter;

MongoExtensionStatus* HostServicesAdapter::_extLog(
    const ::MongoExtensionLogMessage* logMessage) noexcept {
    return wrapCXXAndConvertExceptionToStatus([&]() {
        // Validate that the message type is kLog.
        uassert(11288500,
                "MongoExtensionLogMessage type must be kLog for log() function",
                logMessage->type == ::MongoExtensionLogType::kLog);

        // For now we always log extension messages under the EXTENSION-MONGOT component. Someday

        // For now we always log extension messages under the EXTENSION-MONGOT component. Someday
        // we'd like to dynamically create EXTENSION sub-components per extension.

        logv2::LogOptions options(logv2::LogComponent::kExtensionMongot);

        // Extract message from byte view.
        auto messageView = byteViewAsStringView(logMessage->message);
        StringData message(messageView.data(), messageView.size());

        // Extract code.
        std::int32_t code = static_cast<std::int32_t>(logMessage->code);

        // Convert C enum to logv2 severity.
        logv2::LogSeverity severity = [&]() {
            switch (logMessage->severityOrLevel.severity) {
                case ::MongoExtensionLogSeverity::kWarning:
                    return logv2::LogSeverity::Warning();
                case ::MongoExtensionLogSeverity::kError:
                    return logv2::LogSeverity::Error();
                case ::MongoExtensionLogSeverity::kInfo:
                default:
                    return logv2::LogSeverity::Info();
            }
        }();

        // TODO SERVER-111339 Populate attributes from logMessage->attributes.
        logv2::TypeErasedAttributeStorage attrs;

        // We must go through logv2::detail::doLogImpl since the LOGV2 macros expect a static string
        // literal for the message, but we have to log the message received at runtime from the
        // extension.
        logv2::detail::doLogImpl(code, severity, options, message, attrs);
    });
}

MongoExtensionStatus* HostServicesAdapter::_extLogDebug(
    const ::MongoExtensionLogMessage* logMessage) noexcept {
    return extension::wrapCXXAndConvertExceptionToStatus([&]() {
        // Validate that the message type is kDebug.
        uassert(11288501,
                "MongoExtensionLogMessage type must be kDebug for log_debug() function",
                logMessage->type == ::MongoExtensionLogType::kDebug);

        // For now we always log extension messages under the EXTENSION-MONGOT component. Someday
        // we'd like to dynamically create EXTENSION sub-components per extension.
        logv2::LogOptions options(logv2::LogComponent::kExtensionMongot);

        // Extract message from byte view.
        auto messageView = byteViewAsStringView(logMessage->message);
        StringData message(messageView.data(), messageView.size());

        // Extract code.
        std::int32_t code = static_cast<std::int32_t>(logMessage->code);

        // Extract level from union and trim to the range [1, 5] since we want to make sure that the
        // log line is using one of the server's logv2 debug severities.
        std::int32_t level = logMessage->severityOrLevel.level;
        logv2::LogSeverity logSeverity = logv2::LogSeverity::Debug(std::min(5, std::max(1, level)));

        // TODO SERVER-111339 Populate attributes from logMessage->attributes.
        logv2::TypeErasedAttributeStorage attrs;

        logv2::detail::doLogImpl(code, logSeverity, options, message, attrs);
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

::MongoExtensionStatus* HostServicesAdapter::_extCreateHostAggStageParseNode(
    ::MongoExtensionByteView spec, ::MongoExtensionAggStageParseNode** node) noexcept {
    return wrapCXXAndConvertExceptionToStatus([&]() {
        *node = nullptr;
        auto parseNode = std::make_unique<host::AggStageParseNode>(bsonObjFromByteView(spec));
        *node = static_cast<::MongoExtensionAggStageParseNode*>(
            new host::HostAggStageParseNode(std::move(parseNode)));
    });
}
}  // namespace mongo::extension::host_connector
