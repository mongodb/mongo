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

#include "mongo/db/extension/sdk/assert_util.h"
#include "mongo/db/extension/sdk/host_services.h"
#include "mongo/db/extension/shared/byte_buf_utils.h"

namespace mongo::extension::sdk {

namespace {
void populateLogAttributes(MongoExtensionLogMessage& logMessage,
                           const std::vector<ExtensionLogAttribute>& attrs) {
    // Allocate an array on the heap for log attributes. Note that this array must be manually freed
    // after logging. Consider wrapping the MongoExtensionLogMessage struct in a RAII class, such as
    // LogMessageGuard, that frees the array upon destruction.
    logMessage.attributes.size = attrs.size();
    logMessage.attributes.elements = nullptr;
    if (!attrs.empty()) {
        auto* logAttributes = new ::MongoExtensionLogAttribute[attrs.size()];
        for (size_t i = 0; i < attrs.size(); ++i) {
            logAttributes[i] = {stringViewAsByteView(std::string_view(attrs[i].name)),
                                stringViewAsByteView(std::string_view(attrs[i].value))};
        }
        logMessage.attributes.elements = logAttributes;
    }
}

void populateLogMessageCommon(MongoExtensionLogMessage& logMessage,
                              const std::string& message,
                              std::int32_t code,
                              const std::vector<ExtensionLogAttribute>& attrs) {
    // Convert message string to byte view.
    auto messageBytes = stringViewAsByteView(std::string_view(message));
    logMessage.code = static_cast<uint32_t>(code);
    logMessage.message = messageBytes;
    populateLogAttributes(logMessage, attrs);
}
}  // namespace

LogMessageGuard LoggerHandle::createLogMessageStruct(
    const std::string& message,
    std::int32_t code,
    MongoExtensionLogSeverity severity,
    const std::vector<ExtensionLogAttribute>& attrs) {
    ::MongoExtensionLogMessage logMessage;

    // Populate common fields for the log struct.
    populateLogMessageCommon(logMessage, message, code, attrs);

    // Set union field for severity.
    logMessage.type = ::MongoExtensionLogType::kLog;
    logMessage.severityOrLevel.severity = severity;

    return LogMessageGuard(logMessage);
}

LogMessageGuard LoggerHandle::createDebugLogMessageStruct(
    const std::string& message,
    std::int32_t code,
    std::int32_t level,
    const std::vector<ExtensionLogAttribute>& attrs) {
    ::MongoExtensionLogMessage logMessage;

    // Populate common fields for the log struct.
    populateLogMessageCommon(logMessage, message, code, attrs);

    // Set union field for level.
    logMessage.type = ::MongoExtensionLogType::kDebug;
    logMessage.severityOrLevel.level = level;

    return LogMessageGuard(logMessage);
}

void LoggerHandle::_assertVTableConstraints(const VTable_t& vtable) const {
    sdk_tassert(11188200, "Logger's 'log' is null", vtable.log != nullptr);
    sdk_tassert(11288201, "Logger's 'should_log' is null", vtable.should_log != nullptr);
}

}  // namespace mongo::extension::sdk
