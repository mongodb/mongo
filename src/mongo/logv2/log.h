/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/base/status.h"
#include "mongo/bson/util/builder.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_component_settings.h"
#include "mongo/logv2/log_debug.h"
#include "mongo/logv2/log_detail.h"
#include "mongo/logv2/log_domain.h"
#include "mongo/logv2/log_options.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/logv2/redaction.h"
#include "mongo/util/errno_util.h"

// The logging macros below are documented in detail under docs/logging.md
//
// They all (except LOGV2_IMPL) require a `MONGO_LOGV2_DEFAULT_COMPONENT` macro.
// This configuration macro must expand at their point of use to a
// `LogComponent` expression. For example:
//
//     #define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault
//
//     LOGV2(1234500, "Something interesting happened");
//

// Internal helper to perform the logging where it requires the MESSAGE to be a compile time string.
#define LOGV2_IMPL(ID, SEVERITY, OPTIONS, FMTSTR_MESSAGE, ...) \
    ::mongo::logv2::detail::doLog(ID, SEVERITY, OPTIONS, FMT_STRING(FMTSTR_MESSAGE), ##__VA_ARGS__)

/**
 * Log with default severity and component.
 *
 * This macro acts like a function with 4 overloads:
 *   LOGV2(ID, FMTSTR_MESSAGE, ATTRIBUTES...)
 *   LOGV2(ID, FMTSTR_MESSAGE, DYNAMIC_ATTRIBUTES)
 *   LOGV2(ID, FMTSTR_MESSAGE, MESSAGE, ATTRIBUTES...)
 *   LOGV2(ID, FMTSTR_MESSAGE, MESSAGE, DYNAMIC_ATTRIBUTES)
 *
 * ID is a unique signed int32 in the same number space as other error codes.
 * FMTSTR_MESSAGE is a compile time string constant. Regular "string" is preferred.
 *   This string may contain libfmt replacement fields.
 * MESSAGE is an optional compile time string constant of message without libfmt replacement fields
 * ATTRIBUTES zero more more static attributes created with "name"_attr=value expressions
 * DYNAMIC_ATTRIBUTES single argument DynamicAttributes object
 *   no attributes may be passed with "name"_attr=value when this is used
 */
#define LOGV2(ID, FMTSTR_MESSAGE, ...)                                    \
    LOGV2_IMPL(ID,                                                        \
               ::mongo::logv2::LogSeverity::Log(),                        \
               ::mongo::logv2::LogOptions{MONGO_LOGV2_DEFAULT_COMPONENT}, \
               FMTSTR_MESSAGE,                                            \
               ##__VA_ARGS__)

/**
 * Log with default severity and custom options.
 *
 * OPTIONS is an expression that is used to construct a LogOptions.
 * See LogOptions for available parameters when performing custom logging
 *
 * See LOGV2() for documentation of the other parameters
 */
#define LOGV2_OPTIONS(ID, OPTIONS, FMTSTR_MESSAGE, ...)                                           \
    LOGV2_IMPL(                                                                                   \
        ID,                                                                                       \
        ::mongo::logv2::LogSeverity::Log(),                                                       \
        ::mongo::logv2::LogOptions::ensureValidComponent(OPTIONS, MONGO_LOGV2_DEFAULT_COMPONENT), \
        FMTSTR_MESSAGE,                                                                           \
        ##__VA_ARGS__)

/**
 * Log with info severity.
 *
 * See LOGV2() for documentation of the parameters
 */
#define LOGV2_INFO(ID, FMTSTR_MESSAGE, ...)                               \
    LOGV2_IMPL(ID,                                                        \
               ::mongo::logv2::LogSeverity::Info(),                       \
               ::mongo::logv2::LogOptions{MONGO_LOGV2_DEFAULT_COMPONENT}, \
               FMTSTR_MESSAGE,                                            \
               ##__VA_ARGS__)

/**
 * Log with info severity and custom options.
 *
 * See LOGV2_OPTIONS() for documentation of the parameters
 */
#define LOGV2_INFO_OPTIONS(ID, OPTIONS, FMTSTR_MESSAGE, ...)                                      \
    LOGV2_IMPL(                                                                                   \
        ID,                                                                                       \
        ::mongo::logv2::LogSeverity::Info(),                                                      \
        ::mongo::logv2::LogOptions::ensureValidComponent(OPTIONS, MONGO_LOGV2_DEFAULT_COMPONENT), \
        FMTSTR_MESSAGE,                                                                           \
        ##__VA_ARGS__)

/**
 * Log with warning severity.
 *
 * See LOGV2() for documentation of the parameters
 */
#define LOGV2_WARNING(ID, FMTSTR_MESSAGE, ...)                            \
    LOGV2_IMPL(ID,                                                        \
               ::mongo::logv2::LogSeverity::Warning(),                    \
               ::mongo::logv2::LogOptions{MONGO_LOGV2_DEFAULT_COMPONENT}, \
               FMTSTR_MESSAGE,                                            \
               ##__VA_ARGS__)

/**
 * Log with warning severity and custom options.
 *
 * See LOGV2_OPTIONS() for documentation of the parameters
 */
#define LOGV2_WARNING_OPTIONS(ID, OPTIONS, FMTSTR_MESSAGE, ...)                                   \
    LOGV2_IMPL(                                                                                   \
        ID,                                                                                       \
        ::mongo::logv2::LogSeverity::Warning(),                                                   \
        ::mongo::logv2::LogOptions::ensureValidComponent(OPTIONS, MONGO_LOGV2_DEFAULT_COMPONENT), \
        FMTSTR_MESSAGE,                                                                           \
        ##__VA_ARGS__)

/**
 * Log with error severity.
 *
 * See LOGV2() for documentation of the parameters
 */
#define LOGV2_ERROR(ID, FMTSTR_MESSAGE, ...)                              \
    LOGV2_IMPL(ID,                                                        \
               ::mongo::logv2::LogSeverity::Error(),                      \
               ::mongo::logv2::LogOptions{MONGO_LOGV2_DEFAULT_COMPONENT}, \
               FMTSTR_MESSAGE,                                            \
               ##__VA_ARGS__)

/**
 * Log with error severity and custom options.
 *
 * See LOGV2_OPTIONS() for documentation of the parameters
 */
#define LOGV2_ERROR_OPTIONS(ID, OPTIONS, FMTSTR_MESSAGE, ...)                                     \
    LOGV2_IMPL(                                                                                   \
        ID,                                                                                       \
        ::mongo::logv2::LogSeverity::Error(),                                                     \
        ::mongo::logv2::LogOptions::ensureValidComponent(OPTIONS, MONGO_LOGV2_DEFAULT_COMPONENT), \
        FMTSTR_MESSAGE,                                                                           \
        ##__VA_ARGS__)

/**
 * Log with fatal severity. fassertFailed(ID) will be performed after writing the log
 *
 * See LOGV2() for documentation of the parameters
 */
#define LOGV2_FATAL(ID, FMTSTR_MESSAGE, ...)                                  \
    do {                                                                      \
        LOGV2_IMPL(ID,                                                        \
                   ::mongo::logv2::LogSeverity::Severe(),                     \
                   ::mongo::logv2::LogOptions{MONGO_LOGV2_DEFAULT_COMPONENT}, \
                   FMTSTR_MESSAGE,                                            \
                   ##__VA_ARGS__);                                            \
        fassertFailed(ID);                                                    \
    } while (false)

/**
 * Log with fatal severity. fassertFailedNoTrace(ID) will be performed after writing the log
 *
 * See LOGV2() for documentation of the parameters
 */
#define LOGV2_FATAL_NOTRACE(ID, FMTSTR_MESSAGE, ...)                                        \
    do {                                                                                    \
        LOGV2_IMPL(ID,                                                                      \
                   ::mongo::logv2::LogSeverity::Severe(),                                   \
                   (::mongo::logv2::LogOptions{MONGO_LOGV2_DEFAULT_COMPONENT,               \
                                               ::mongo::logv2::FatalMode::kAssertNoTrace}), \
                   FMTSTR_MESSAGE,                                                          \
                   ##__VA_ARGS__);                                                          \
        fassertFailedNoTrace(ID);                                                           \
    } while (false)

/**
 * Log with fatal severity. Execution continues after log.
 *
 * See LOGV2() for documentation of the parameters
 */
#define LOGV2_FATAL_CONTINUE(ID, FMTSTR_MESSAGE, ...)                              \
    LOGV2_IMPL(ID,                                                                 \
               ::mongo::logv2::LogSeverity::Severe(),                              \
               (::mongo::logv2::LogOptions{MONGO_LOGV2_DEFAULT_COMPONENT,          \
                                           ::mongo::logv2::FatalMode::kContinue}), \
               FMTSTR_MESSAGE,                                                     \
               ##__VA_ARGS__)

/**
 * Log with fatal severity and custom options.
 *
 * Will perform fassert after logging depending on the fatalMode() setting in OPTIONS
 *
 * See LOGV2_OPTIONS() for documentation of the parameters
 */
#define LOGV2_FATAL_OPTIONS(ID, OPTIONS, FMTSTR_MESSAGE, ...)                       \
    do {                                                                            \
        auto optionsMacroLocal_ = ::mongo::logv2::LogOptions::ensureValidComponent( \
            OPTIONS, MONGO_LOGV2_DEFAULT_COMPONENT);                                \
        LOGV2_IMPL(ID,                                                              \
                   ::mongo::logv2::LogSeverity::Severe(),                           \
                   optionsMacroLocal_,                                              \
                   FMTSTR_MESSAGE,                                                  \
                   ##__VA_ARGS__);                                                  \
        switch (optionsMacroLocal_.fatalMode()) {                                   \
            case ::mongo::logv2::FatalMode::kAssert:                                \
                fassertFailed(ID);                                                  \
            case ::mongo::logv2::FatalMode::kAssertNoTrace:                         \
                fassertFailedNoTrace(ID);                                           \
            case ::mongo::logv2::FatalMode::kContinue:                              \
                break;                                                              \
        };                                                                          \
    } while (false)

/**
 * Log with debug level severity and custom options.
 *
 * DLEVEL is an integer representing the debug level. Valid range is [1, 5]
 *
 * See LOGV2_OPTIONS() for documentation of the other parameters
 */
#define LOGV2_DEBUG_OPTIONS(ID, DLEVEL, OPTIONS, FMTSTR_MESSAGE, ...)                        \
    do {                                                                                     \
        auto severityMacroLocal_ = ::mongo::logv2::LogSeverity::Debug(DLEVEL);               \
        auto optionsMacroLocal_ = ::mongo::logv2::LogOptions::ensureValidComponent(          \
            OPTIONS, MONGO_LOGV2_DEFAULT_COMPONENT);                                         \
        if (::mongo::logv2::LogManager::global().getGlobalSettings().shouldLog(              \
                optionsMacroLocal_.component(), severityMacroLocal_)) {                      \
            LOGV2_IMPL(                                                                      \
                ID, severityMacroLocal_, optionsMacroLocal_, FMTSTR_MESSAGE, ##__VA_ARGS__); \
        }                                                                                    \
    } while (false)

/**
 * Log with debug level severity.
 *
 * DLEVEL is an integer representing the debug level. Valid range is [1, 5]
 *
 * See LOGV2() for documentation of the other parameters
 */
#define LOGV2_DEBUG(ID, DLEVEL, FMTSTR_MESSAGE, ...)                               \
    LOGV2_DEBUG_OPTIONS(ID,                                                        \
                        DLEVEL,                                                    \
                        ::mongo::logv2::LogOptions{MONGO_LOGV2_DEFAULT_COMPONENT}, \
                        FMTSTR_MESSAGE,                                            \
                        ##__VA_ARGS__)

namespace mongo::logv2 {
inline bool shouldLog(LogComponent logComponent, LogSeverity severity) {
    return LogManager::global().getGlobalSettings().shouldLog(logComponent, severity);
}
}  // namespace mongo::logv2
