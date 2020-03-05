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

// #pragma once is not used in this header.
// This header attempts to enforce the rule that no logging should be done in
// an inline function defined in a header.
// To enforce this "no logging in header" rule, we use #include guards with a validating #else
// clause.
// Also, this header relies on a preprocessor macro to determine the default component for the
// unconditional logging functions severe(), error(), warning() and log(). Disallowing multiple
// inclusion of log.h will ensure that the default component will be set correctly.

#if defined(MONGO_UTIL_LOGV2_H_)
#error \
    "mongo/logv2/log.h cannot be included multiple times. " \
       "This may occur when log.h is included in a header. " \
       "Please check your #include's."
#else  // MONGO_UTIL_LOGV2_H_
#define MONGO_UTIL_LOGV2_H_

#include "mongo/base/status.h"
#include "mongo/bson/util/builder.h"
#include "mongo/logger/log_version_util.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_component_settings.h"
#include "mongo/logv2/log_detail.h"
#include "mongo/logv2/log_domain.h"
#include "mongo/logv2/log_options.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/logv2/redaction.h"
#include "mongo/util/errno_util.h"

#if defined(MONGO_LOG_DEFAULT_COMPONENT)
#include "mongo/logger/log_version_util.h"
#endif

namespace {
#if defined(MONGO_LOGV2_DEFAULT_COMPONENT)
const ::mongo::logv2::LogComponent MongoLogV2DefaultComponent_component =
    MONGO_LOGV2_DEFAULT_COMPONENT;

// Provide log component in global scope so that MONGO_LOG will always have a valid component.
// Global log component will be kDefault unless overridden by MONGO_LOGV2_DEFAULT_COMPONENT.
#elif defined(MONGO_LOG_DEFAULT_COMPONENT)
const ::mongo::logv2::LogComponent MongoLogV2DefaultComponent_component =
    ::mongo::logComponentV1toV2(MONGO_LOG_DEFAULT_COMPONENT);
#else
#error \
    "mongo/logv2/log.h requires MONGO_LOGV2_DEFAULT_COMPONENT to be defined. " \
       "Please see http://www.mongodb.org/about/contributors/reference/server-logging-rules/ "
#endif  // MONGO_LOGV2_DEFAULT_COMPONENT
}  // namespace

namespace mongo {

#define LOGV2_IMPL(ID, SEVERITY, OPTIONS, MESSAGE, ...) \
    ::mongo::logv2::detail::doLog(ID, SEVERITY, OPTIONS, FMT_STRING(MESSAGE), ##__VA_ARGS__)

#define LOGV2(ID, MESSAGE, ...)                                                  \
    LOGV2_IMPL(ID,                                                               \
               ::mongo::logv2::LogSeverity::Log(),                               \
               ::mongo::logv2::LogOptions{MongoLogV2DefaultComponent_component}, \
               MESSAGE,                                                          \
               ##__VA_ARGS__)

#define LOGV2_OPTIONS(ID, OPTIONS, MESSAGE, ...)                   \
    LOGV2_IMPL(ID,                                                 \
               ::mongo::logv2::LogSeverity::Log(),                 \
               ::mongo::logv2::LogOptions::ensureValidComponent(   \
                   OPTIONS, MongoLogV2DefaultComponent_component), \
               MESSAGE,                                            \
               ##__VA_ARGS__)

#define LOGV2_INFO(ID, MESSAGE, ...)                                             \
    LOGV2_IMPL(ID,                                                               \
               ::mongo::logv2::LogSeverity::Info(),                              \
               ::mongo::logv2::LogOptions{MongoLogV2DefaultComponent_component}, \
               MESSAGE,                                                          \
               ##__VA_ARGS__)

#define LOGV2_INFO_OPTIONS(ID, OPTIONS, MESSAGE, ...)              \
    LOGV2_IMPL(ID,                                                 \
               ::mongo::logv2::LogSeverity::Info(),                \
               ::mongo::logv2::LogOptions::ensureValidComponent(   \
                   OPTIONS, MongoLogV2DefaultComponent_component), \
               MESSAGE,                                            \
               ##__VA_ARGS__)

#define LOGV2_WARNING(ID, MESSAGE, ...)                                          \
    LOGV2_IMPL(ID,                                                               \
               ::mongo::logv2::LogSeverity::Warning(),                           \
               ::mongo::logv2::LogOptions{MongoLogV2DefaultComponent_component}, \
               MESSAGE,                                                          \
               ##__VA_ARGS__)

#define LOGV2_WARNING_OPTIONS(ID, OPTIONS, MESSAGE, ...)           \
    LOGV2_IMPL(ID,                                                 \
               ::mongo::logv2::LogSeverity::Warning(),             \
               ::mongo::logv2::LogOptions::ensureValidComponent(   \
                   OPTIONS, MongoLogV2DefaultComponent_component), \
               MESSAGE,                                            \
               ##__VA_ARGS__)

#define LOGV2_ERROR(ID, MESSAGE, ...)                                            \
    LOGV2_IMPL(ID,                                                               \
               ::mongo::logv2::LogSeverity::Error(),                             \
               ::mongo::logv2::LogOptions{MongoLogV2DefaultComponent_component}, \
               MESSAGE,                                                          \
               ##__VA_ARGS__)

#define LOGV2_ERROR_OPTIONS(ID, OPTIONS, MESSAGE, ...)             \
    LOGV2_IMPL(ID,                                                 \
               ::mongo::logv2::LogSeverity::Error(),               \
               ::mongo::logv2::LogOptions::ensureValidComponent(   \
                   OPTIONS, MongoLogV2DefaultComponent_component), \
               MESSAGE,                                            \
               ##__VA_ARGS__)

#define LOGV2_FATAL(ID, MESSAGE, ...)                                            \
    LOGV2_IMPL(ID,                                                               \
               ::mongo::logv2::LogSeverity::Severe(),                            \
               ::mongo::logv2::LogOptions{MongoLogV2DefaultComponent_component}, \
               MESSAGE,                                                          \
               ##__VA_ARGS__)

#define LOGV2_FATAL_OPTIONS(ID, OPTIONS, MESSAGE, ...)             \
    LOGV2_IMPL(ID,                                                 \
               ::mongo::logv2::LogSeverity::Severe(),              \
               ::mongo::logv2::LogOptions::ensureValidComponent(   \
                   OPTIONS, MongoLogV2DefaultComponent_component), \
               MESSAGE,                                            \
               ##__VA_ARGS__)

#define LOGV2_DEBUG_OPTIONS(ID, DLEVEL, OPTIONS, MESSAGE, ...)                               \
    do {                                                                                     \
        auto severityMacroLocal_ = ::mongo::logv2::LogSeverity::Debug(DLEVEL);               \
        auto optionsMacroLocal_ = ::mongo::logv2::LogOptions::ensureValidComponent(          \
            OPTIONS, MongoLogV2DefaultComponent_component);                                  \
        if (::mongo::logv2::LogManager::global().getGlobalSettings().shouldLog(              \
                optionsMacroLocal_.component(), severityMacroLocal_)) {                      \
            LOGV2_IMPL(ID, severityMacroLocal_, optionsMacroLocal_, MESSAGE, ##__VA_ARGS__); \
        }                                                                                    \
    } while (false)

#define LOGV2_DEBUG(ID, DLEVEL, MESSAGE, ...)                                             \
    LOGV2_DEBUG_OPTIONS(ID,                                                               \
                        DLEVEL,                                                           \
                        ::mongo::logv2::LogOptions{MongoLogV2DefaultComponent_component}, \
                        MESSAGE,                                                          \
                        ##__VA_ARGS__)

inline bool shouldLog(logv2::LogSeverity severity) {
    return logv2::LogManager::global().getGlobalSettings().shouldLog(
        MongoLogV2DefaultComponent_component, severity);
}

inline bool shouldLog(logv2::LogComponent logComponent, logv2::LogSeverity severity) {
    return logv2::LogManager::global().getGlobalSettings().shouldLog(logComponent, severity);
}

}  // namespace mongo

#endif  // MONGO_UTIL_LOGV2_H_
