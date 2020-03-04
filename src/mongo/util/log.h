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

#if defined(MONGO_UTIL_LOG_H_)
#error \
    "This may occur when log.h is included in a header. " \
       "Please check your #include's."
#else  // MONGO_UTIL_LOG_H_
#define MONGO_UTIL_LOG_H_

#include "mongo/base/status.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/server_options.h"
#include "mongo/logger/log_component.h"
#include "mongo/logger/log_severity_limiter.h"
#include "mongo/logger/logger.h"
#include "mongo/logger/logstream_builder.h"
#include "mongo/logger/redaction.h"
#include "mongo/logger/tee.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_component_settings.h"
#include "mongo/logv2/log_manager.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/log_global_settings.h"

namespace mongo {

namespace logger {
typedef void (*ExtraLogContextFn)(BufBuilder& builder);
Status registerExtraLogContextFn(ExtraLogContextFn contextFn);

}  // namespace logger

/*
 * Although this is a "header" file, it is only supposed to be included in C++ files.  The
 * definitions of the following objects and functions (in the below anonymous namespace) will
 * potentially differ by TU.  Therefore these names must remain with internal linkage.  This
 * particular anoymous namespace in this header, therefore, is not an ODR problem but is instead a
 * solution to an ODR problem which would arise.
 */
namespace {

// Provide log component in global scope so that MONGO_LOG will always have a valid component.
// Global log component will be kDefault unless overridden by MONGO_LOG_DEFAULT_COMPONENT.
#if defined(MONGO_LOG_DEFAULT_COMPONENT)
const ::mongo::logger::LogComponent MongoLogDefaultComponent_component =
    MONGO_LOG_DEFAULT_COMPONENT;
#else
#error "Please see http://www.mongodb.org/about/contributors/reference/server-logging-rules/ "
#endif  // MONGO_LOG_DEFAULT_COMPONENT

using logger::LogstreamBuilderDeprecated;
using logger::Tee;

/**
 * Returns a LogstreamBuilder for logging a message with LogSeverity::Severe().
 */
inline LogstreamBuilderDeprecated severe() {
    return LogstreamBuilderDeprecated(logger::globalLogDomain(),
                                      getThreadName(),
                                      logger::LogSeverity::Severe(),
                                      MongoLogDefaultComponent_component);
}

inline LogstreamBuilderDeprecated severe(logger::LogComponent component) {
    return LogstreamBuilderDeprecated(
        logger::globalLogDomain(), getThreadName(), logger::LogSeverity::Severe(), component);
}

/**
 * Returns a LogstreamBuilder for logging a message with LogSeverity::Error().
 */
inline LogstreamBuilderDeprecated error() {
    return LogstreamBuilderDeprecated(logger::globalLogDomain(),
                                      getThreadName(),
                                      logger::LogSeverity::Error(),
                                      MongoLogDefaultComponent_component);
}

inline LogstreamBuilderDeprecated error(logger::LogComponent component) {
    return LogstreamBuilderDeprecated(
        logger::globalLogDomain(), getThreadName(), logger::LogSeverity::Error(), component);
}

/**
 * Returns a LogstreamBuilder for logging a message with LogSeverity::Warning().
 */
inline LogstreamBuilderDeprecated warning() {
    return LogstreamBuilderDeprecated(logger::globalLogDomain(),
                                      getThreadName(),
                                      logger::LogSeverity::Warning(),
                                      MongoLogDefaultComponent_component);
}

inline LogstreamBuilderDeprecated warning(logger::LogComponent component) {
    return LogstreamBuilderDeprecated(
        logger::globalLogDomain(), getThreadName(), logger::LogSeverity::Warning(), component);
}

/**
 * Returns a LogstreamBuilder for logging a message with LogSeverity::Log().
 */
inline LogstreamBuilderDeprecated log() {
    return LogstreamBuilderDeprecated(logger::globalLogDomain(),
                                      getThreadName(),
                                      logger::LogSeverity::Log(),
                                      MongoLogDefaultComponent_component);
}

/**
 * Returns a LogstreamBuilder that does not cache its ostream in a threadlocal cache.
 * Use this variant when logging from places that may not be able to access threadlocals,
 * such as from within other threadlocal-managed objects, or thread_specific_ptr-managed
 * objects.
 *
 * Once SERVER-29377 is completed, this overload can be removed.
 */
inline LogstreamBuilderDeprecated logNoCache() {
    return LogstreamBuilderDeprecated(logger::globalLogDomain(),
                                      getThreadName(),
                                      logger::LogSeverity::Log(),
                                      MongoLogDefaultComponent_component,
                                      false);
}

inline LogstreamBuilderDeprecated log(logger::LogComponent component) {
    return LogstreamBuilderDeprecated(
        logger::globalLogDomain(), getThreadName(), logger::LogSeverity::Log(), component);
}

inline LogstreamBuilderDeprecated log(logger::LogComponent::Value componentValue) {
    return LogstreamBuilderDeprecated(
        logger::globalLogDomain(), getThreadName(), logger::LogSeverity::Log(), componentValue);
}

}  // namespace

// this can't be in log_global_settings.h because it utilizes MongoLogDefaultComponent_component
inline bool shouldLogV1(logger::LogSeverity severity) {
    if (logV2Enabled())
        return logv2::LogManager::global().getGlobalSettings().shouldLog(
            logComponentV1toV2(MongoLogDefaultComponent_component), logSeverityV1toV2(severity));
    return mongo::shouldLogV1(MongoLogDefaultComponent_component, severity);
}

// MONGO_LOG uses log component from MongoLogDefaultComponent from current or global namespace.
#define MONGO_LOG(DLEVEL)                                                                   \
    if (!::mongo::shouldLogV1(MongoLogDefaultComponent_component,                           \
                              ::mongo::LogstreamBuilderDeprecated::severityCast(DLEVEL))) { \
    } else                                                                                  \
        ::mongo::logger::LogstreamBuilderDeprecated(                                        \
            ::mongo::logger::globalLogDomain(),                                             \
            ::mongo::getThreadName(),                                                       \
            ::mongo::LogstreamBuilderDeprecated::severityCast(DLEVEL),                      \
            MongoLogDefaultComponent_component)

#define LOG MONGO_LOG

#define MONGO_LOG_COMPONENT(DLEVEL, COMPONENT1)                                             \
    if (!::mongo::shouldLogV1((COMPONENT1),                                                 \
                              ::mongo::LogstreamBuilderDeprecated::severityCast(DLEVEL))) { \
    } else                                                                                  \
        ::mongo::logger::LogstreamBuilderDeprecated(                                        \
            ::mongo::logger::globalLogDomain(),                                             \
            ::mongo::getThreadName(),                                                       \
            ::mongo::LogstreamBuilderDeprecated::severityCast(DLEVEL),                      \
            (COMPONENT1))

extern Tee* const warnings;            // Things put here go in serverStatus
extern Tee* const startupWarningsLog;  // Things put here get reported in MMS

}  // namespace mongo

#endif  // MONGO_UTIL_LOG_H_
