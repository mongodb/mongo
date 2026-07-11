// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/shared/extension_status.h"
#include "mongo/db/extension/shared/handle/handle.h"
#include "mongo/util/modules.h"

namespace mongo::extension {

namespace sdk {
class LoggerAPI;
}

template <>
struct c_api_to_cpp_api<::MongoExtensionLogger> {
    using CppApi_t = sdk::LoggerAPI;
};

namespace sdk {

/**
 * A simple named struct that allows an extension to specify the name/value of a desired log
 * attribute.
 */
struct ExtensionLogAttribute {
    std::string name;
    std::string value;
};

/**
 * RAII guard for MongoExtensionLogMessage that manages the lifetime of
 * dynamically allocated log attributes.
 *
 * Automatically deletes the attributes array when the guard is destroyed,
 * preventing memory leaks from newly allocated MongoExtensionLogAttribute arrays.
 */
class LogMessageGuard {
    ::MongoExtensionLogMessage _msg;

public:
    LogMessageGuard(const ::MongoExtensionLogMessage& msg) : _msg(msg) {}
    ~LogMessageGuard() {
        if (_msg.attributes.size > 0) {
            delete[] _msg.attributes.elements;
        }
    }
    ::MongoExtensionLogMessage* get() {
        return &_msg;
    }
};

using LoggerHandle = UnownedHandle<const ::MongoExtensionLogger>;

class LoggerAPI : public VTableAPI<::MongoExtensionLogger> {
public:
    LoggerAPI(::MongoExtensionLogger* services) : VTableAPI<::MongoExtensionLogger>(services) {}

    /**
     * Creates a MongoExtensionLogMessage struct for regular log messages.
     * The returned struct should be passed to log() and is valid only during the call.
     */
    static LogMessageGuard createLogMessageStruct(const std::string& message,
                                                  std::int32_t code,
                                                  MongoExtensionLogSeverity severity,
                                                  const std::vector<ExtensionLogAttribute>& attrs);

    /**
     * Creates a MongoExtensionLogMessage struct for debug log messages.
     * The returned struct should be passed to logDebug() and is valid only during the call.
     */
    static LogMessageGuard createDebugLogMessageStruct(
        const std::string& message,
        std::int32_t code,
        std::int32_t level,
        const std::vector<ExtensionLogAttribute>& attrs);

    void log(const std::string& message,
             std::int32_t code,
             MongoExtensionLogSeverity severity,
             const std::vector<ExtensionLogAttribute>& attrs) const {

        // Prevent materializing log messages that would not be logged.
        if (!shouldLog(severity, ::MongoExtensionLogType::kLog)) {
            return;
        }

        auto logMessage = createLogMessageStruct(message, code, severity, attrs);
        invokeCAndConvertStatusToException([&]() { return _vtable().log(logMessage.get()); });
    }

    void logDebug(const std::string& message,
                  std::int32_t code,
                  std::int32_t level,
                  const std::vector<ExtensionLogAttribute>& attrs) const {

        // Prevent materializing log messages that would not be logged.
        if (!shouldLog(::MongoExtensionLogSeverity(level), ::MongoExtensionLogType::kDebug)) {
            return;
        }

        auto logMessage = createDebugLogMessageStruct(message, code, level, attrs);
        invokeCAndConvertStatusToException([&]() { return _vtable().log(logMessage.get()); });
    }

    bool shouldLog(::MongoExtensionLogSeverity levelOrSeverity,
                   ::MongoExtensionLogType logType) const {
        bool out = false;
        invokeCAndConvertStatusToException(
            [&]() { return _vtable().should_log(levelOrSeverity, logType, &out); });
        return out;
    }

    static void assertVTableConstraints(const VTable_t& vtable);
};
}  // namespace sdk
}  // namespace mongo::extension
