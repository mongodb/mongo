// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/extension/public/api.h"
#include "mongo/logv2/log.h"
#include "mongo/util/modules.h"

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

    static constexpr ::MongoExtensionLoggerVTable VTABLE = {.log = &_extLog,
                                                            .should_log = &_extShouldLog};
};
}  // namespace mongo::extension::host_connector
