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
#include "mongo/logv2/log.h"

namespace mongo::extension::host_connector {

/**
 * Provides logging services for extensions.
 *
 * LoggerAdapter implements the MongoExtensionLogger interface to enable extensions to send
 * structured log messages to MongoDB's logv2 logging system. It acts as an adapter between
 * the C extension API and MongoDB's internal logging infrastructure.
 *
 * All extension log messages are routed to the EXTENSION-MONGOT log component. The class is a
 * singleton and stateless, with a static instance maintained by the host.
 */
class LoggerAdapter final : public ::MongoExtensionLogger {
public:
    LoggerAdapter() : ::MongoExtensionLogger{&VTABLE} {}

    static LoggerAdapter* get() {
        return &_loggerAdapter;
    }

private:
    static LoggerAdapter _loggerAdapter;

    static MongoExtensionStatus* _extLog(const ::MongoExtensionLogMessage* logMessage) noexcept;

    static ::MongoExtensionStatus* _extShouldLog(::MongoExtensionLogSeverity levelOrSeverity,
                                                 ::MongoExtensionLogType logType,
                                                 bool* out) noexcept;

    static constexpr logv2::LogSeverity convertSeverity(::MongoExtensionLogSeverity severity) {
        switch (severity) {
            case ::MongoExtensionLogSeverity::kWarning:
                return logv2::LogSeverity::Warning();
            case ::MongoExtensionLogSeverity::kError:
                return logv2::LogSeverity::Error();
            case ::MongoExtensionLogSeverity::kInfo:
            default:
                return logv2::LogSeverity::Info();
        }
    }

    static constexpr ::MongoExtensionLoggerVTable VTABLE{.log = &_extLog,
                                                         .should_log = &_extShouldLog};
};
}  // namespace mongo::extension::host_connector
