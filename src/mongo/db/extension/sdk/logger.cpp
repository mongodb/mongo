// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/extension/sdk/assert_util.h"
#include "mongo/db/extension/sdk/host_services.h"
#include "mongo/db/extension/shared/byte_buf_utils.h"

#include <string_view>

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

LogMessageGuard LoggerAPI::createLogMessageStruct(const std::string& message,
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

LogMessageGuard LoggerAPI::createDebugLogMessageStruct(
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

void LoggerAPI::assertVTableConstraints(const VTable_t& vtable) {
    sdk_tassert(11188200, "Logger's 'log' is null", vtable.log != nullptr);
    sdk_tassert(11288201, "Logger's 'should_log' is null", vtable.should_log != nullptr);
}

}  // namespace mongo::extension::sdk
