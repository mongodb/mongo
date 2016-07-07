// @file log.h

/*    Copyright 2009 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
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
    "mongo/util/log.h cannot be included multiple times. " \
       "This may occur when log.h is included in a header. " \
       "Please check your #include's."
#else  // MONGO_UTIL_LOG_H_
#define MONGO_UTIL_LOG_H_

#include "mongo/base/status.h"
#include "mongo/bson/util/builder.h"
#include "mongo/logger/log_component.h"
#include "mongo/logger/logger.h"
#include "mongo/logger/logstream_builder.h"
#include "mongo/logger/redaction.h"
#include "mongo/logger/tee.h"
#include "mongo/util/concurrency/thread_name.h"

// Provide log component in global scope so that MONGO_LOG will always have a valid component.
// Global log component will be kDefault unless overridden by MONGO_LOG_DEFAULT_COMPONENT.
#if defined(MONGO_LOG_DEFAULT_COMPONENT)
const ::mongo::logger::LogComponent MongoLogDefaultComponent_component =
    MONGO_LOG_DEFAULT_COMPONENT;
#else
#error \
    "mongo/util/log.h requires MONGO_LOG_DEFAULT_COMPONENT to be defined. " \
       "Please see http://www.mongodb.org/about/contributors/reference/server-logging-rules/ "
#endif  // MONGO_LOG_DEFAULT_COMPONENT

namespace mongo {

namespace logger {
typedef void (*ExtraLogContextFn)(BufBuilder& builder);
Status registerExtraLogContextFn(ExtraLogContextFn contextFn);

}  // namespace logger

namespace {

using logger::LogstreamBuilder;
using logger::LabeledLevel;
using logger::Tee;

/**
 * Returns a LogstreamBuilder for logging a message with LogSeverity::Severe().
 */
inline LogstreamBuilder severe() {
    return LogstreamBuilder(logger::globalLogDomain(),
                            getThreadName(),
                            logger::LogSeverity::Severe(),
                            ::MongoLogDefaultComponent_component);
}

inline LogstreamBuilder severe(logger::LogComponent component) {
    return LogstreamBuilder(
        logger::globalLogDomain(), getThreadName(), logger::LogSeverity::Severe(), component);
}

/**
 * Returns a LogstreamBuilder for logging a message with LogSeverity::Error().
 */
inline LogstreamBuilder error() {
    return LogstreamBuilder(logger::globalLogDomain(),
                            getThreadName(),
                            logger::LogSeverity::Error(),
                            ::MongoLogDefaultComponent_component);
}

inline LogstreamBuilder error(logger::LogComponent component) {
    return LogstreamBuilder(
        logger::globalLogDomain(), getThreadName(), logger::LogSeverity::Error(), component);
}

/**
 * Returns a LogstreamBuilder for logging a message with LogSeverity::Warning().
 */
inline LogstreamBuilder warning() {
    return LogstreamBuilder(logger::globalLogDomain(),
                            getThreadName(),
                            logger::LogSeverity::Warning(),
                            ::MongoLogDefaultComponent_component);
}

inline LogstreamBuilder warning(logger::LogComponent component) {
    return LogstreamBuilder(
        logger::globalLogDomain(), getThreadName(), logger::LogSeverity::Warning(), component);
}

/**
 * Returns a LogstreamBuilder for logging a message with LogSeverity::Log().
 */
inline LogstreamBuilder log() {
    return LogstreamBuilder(logger::globalLogDomain(),
                            getThreadName(),
                            logger::LogSeverity::Log(),
                            ::MongoLogDefaultComponent_component);
}

inline LogstreamBuilder log(logger::LogComponent component) {
    return LogstreamBuilder(
        logger::globalLogDomain(), getThreadName(), logger::LogSeverity::Log(), component);
}

inline LogstreamBuilder log(logger::LogComponent::Value componentValue) {
    return LogstreamBuilder(
        logger::globalLogDomain(), getThreadName(), logger::LogSeverity::Log(), componentValue);
}

/**
 * Runs the same logic as log()/warning()/error(), without actually outputting a stream.
 */
inline bool shouldLog(logger::LogSeverity severity) {
    return logger::globalLogDomain()->shouldLog(::MongoLogDefaultComponent_component, severity);
}

}  // namespace

// MONGO_LOG uses log component from MongoLogDefaultComponent from current or global namespace.
#define MONGO_LOG(DLEVEL)                                                              \
    if (!(::mongo::logger::globalLogDomain())                                          \
             ->shouldLog(MongoLogDefaultComponent_component,                           \
                         ::mongo::LogstreamBuilder::severityCast(DLEVEL))) {           \
    } else                                                                             \
    ::mongo::logger::LogstreamBuilder(::mongo::logger::globalLogDomain(),              \
                                      ::mongo::getThreadName(),                        \
                                      ::mongo::LogstreamBuilder::severityCast(DLEVEL), \
                                      MongoLogDefaultComponent_component)

#define LOG MONGO_LOG

#define MONGO_LOG_COMPONENT(DLEVEL, COMPONENT1)                                            \
    if (!(::mongo::logger::globalLogDomain())                                              \
             ->shouldLog((COMPONENT1), ::mongo::LogstreamBuilder::severityCast(DLEVEL))) { \
    } else                                                                                 \
    ::mongo::logger::LogstreamBuilder(::mongo::logger::globalLogDomain(),                  \
                                      ::mongo::getThreadName(),                            \
                                      ::mongo::LogstreamBuilder::severityCast(DLEVEL),     \
                                      (COMPONENT1))

#define MONGO_LOG_COMPONENT2(DLEVEL, COMPONENT1, COMPONENT2)                                     \
    if (!(::mongo::logger::globalLogDomain())                                                    \
             ->shouldLog(                                                                        \
                 (COMPONENT1), (COMPONENT2), ::mongo::LogstreamBuilder::severityCast(DLEVEL))) { \
    } else                                                                                       \
    ::mongo::logger::LogstreamBuilder(::mongo::logger::globalLogDomain(),                        \
                                      ::mongo::getThreadName(),                                  \
                                      ::mongo::LogstreamBuilder::severityCast(DLEVEL),           \
                                      (COMPONENT1))

#define MONGO_LOG_COMPONENT3(DLEVEL, COMPONENT1, COMPONENT2, COMPONENT3)               \
    if (!(::mongo::logger::globalLogDomain())                                          \
             ->shouldLog((COMPONENT1),                                                 \
                         (COMPONENT2),                                                 \
                         (COMPONENT3),                                                 \
                         ::mongo::LogstreamBuilder::severityCast(DLEVEL))) {           \
    } else                                                                             \
    ::mongo::logger::LogstreamBuilder(::mongo::logger::globalLogDomain(),              \
                                      ::mongo::getThreadName(),                        \
                                      ::mongo::LogstreamBuilder::severityCast(DLEVEL), \
                                      (COMPONENT1))

/**
 * Rotates the log files.  Returns true if all logs rotate successfully.
 *
 * renameFiles - true means we rename files, false means we expect the file to be renamed
 *               externally
 *
 * logrotate on *nix systems expects us not to rename the file, it is expected that the program
 * simply open the file again with the same name.
 * We expect logrotate to rename the existing file before we rotate, and so the next open
 * we do should result in a file create.
 */
bool rotateLogs(bool renameFiles);

/** output the error # and error message with prefix.
    handy for use as parm in uassert/massert.
    */
std::string errnoWithPrefix(StringData prefix);

extern Tee* const warnings;            // Things put here go in serverStatus
extern Tee* const startupWarningsLog;  // Things put here get reported in MMS

std::string errnoWithDescription(int errorcode = -1);
std::pair<int, std::string> errnoAndDescription();

/**
 * Write the current context (backtrace), along with the optional "msg".
 */
void logContext(const char* msg = NULL);

}  // namespace mongo

#endif  // MONGO_UTIL_LOG_H_
