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
#include "mongo/db/extension/shared/extension_status.h"
#include "mongo/db/extension/shared/handle/handle.h"
#include "mongo/util/modules.h"

namespace mongo::extension::sdk {

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

class LoggerHandle : public UnownedHandle<const ::MongoExtensionLogger> {
public:
    LoggerHandle(const ::MongoExtensionLogger* services)
        : UnownedHandle<const ::MongoExtensionLogger>(services) {}

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
        assertValid();

        // Prevent materializing log messages that would not be logged.
        if (!shouldLog(severity, ::MongoExtensionLogType::kLog)) {
            return;
        }

        auto logMessage = createLogMessageStruct(message, code, severity, attrs);
        invokeCAndConvertStatusToException([&]() { return vtable().log(logMessage.get()); });
    }

    void logDebug(const std::string& message,
                  std::int32_t code,
                  std::int32_t level,
                  const std::vector<ExtensionLogAttribute>& attrs) const {
        assertValid();

        // Prevent materializing log messages that would not be logged.
        if (!shouldLog(::MongoExtensionLogSeverity(level), ::MongoExtensionLogType::kDebug)) {
            return;
        }

        auto logMessage = createDebugLogMessageStruct(message, code, level, attrs);
        invokeCAndConvertStatusToException([&]() { return vtable().log(logMessage.get()); });
    }

    bool shouldLog(::MongoExtensionLogSeverity levelOrSeverity,
                   ::MongoExtensionLogType logType) const {
        assertValid();
        bool out = false;
        invokeCAndConvertStatusToException(
            [&]() { return vtable().should_log(levelOrSeverity, logType, &out); });
        return out;
    }

private:
    void _assertVTableConstraints(const VTable_t& vtable) const override;
};

}  // namespace mongo::extension::sdk
