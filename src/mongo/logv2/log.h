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

#include <boost/preprocessor/variadic/size.hpp>

#include "mongo/base/status.h"
#include "mongo/bson/util/builder.h"
#include "mongo/logv2/attribute_argument_set.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_domain.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/util/errno_util.h"

// Provide log component in global scope so that MONGO_LOG will always have a valid component.
// Global log component will be kDefault unless overridden by MONGO_LOGV2_DEFAULT_COMPONENT.
#if defined(MONGO_LOGV2_DEFAULT_COMPONENT)
const ::mongo::logv2::LogComponent MongoLogV2DefaultComponent_component =
    MONGO_LOGV2_DEFAULT_COMPONENT;
#else
#error \
    "mongo/logv2/log.h requires MONGO_LOGV2_DEFAULT_COMPONENT to be defined. " \
       "Please see http://www.mongodb.org/about/contributors/reference/server-logging-rules/ "
#endif  // MONGO_LOGV2_DEFAULT_COMPONENT

// include log_detail.h and log_options.h after MONGO_LOGV2_DEFAULT_COMPONENT gets set
#include "mongo/logv2/log_detail.h"
#include "mongo/logv2/log_options.h"

namespace mongo {

#define LOGV2_IMPL_0(SEVERITY, OPTIONS, ID, MESSAGE)                      \
    do {                                                                  \
        logv2::detail::doLog(SEVERITY, OPTIONS, ID, FMT_STRING(MESSAGE)); \
    } while (false)

#define LOGV2_IMPL_1(SEVERITY, OPTIONS, ID, MESSAGE, ...) \
    logv2::detail::doLog(SEVERITY, OPTIONS, ID, FMT_STRING(MESSAGE), __VA_ARGS__)

#define LOGV2_1(MESSAGE, ...)                         \
    LOGV2_IMPL_1(::mongo::logv2::LogSeverity::Info(), \
                 ::mongo::logv2::LogOptions{},        \
                 ::mongo::StringData{},               \
                 MESSAGE,                             \
                 __VA_ARGS__)

#define LOGV2_OPTIONS_1(OPTIONS, MESSAGE, ...) \
    LOGV2_IMPL_1(                              \
        ::mongo::logv2::LogSeverity::Info(), OPTIONS, ::mongo::StringData{}, MESSAGE, __VA_ARGS__)

#define LOGV2_STABLE(ID, MESSAGE, ...)              \
    LOGV2_IMPL(::mongo::logv2::LogSeverity::Info(), \
               ::mongo::logv2::LogOptions{},        \
               ID,                                  \
               MESSAGE,                             \
               __VA_ARGS__)

#define LOGV2_WARNING_OPTIONS(OPTIONS, MESSAGE, ...)   \
    LOGV2_IMPL(::mongo::logv2::LogSeverity::Warning(), \
               OPTIONS,                                \
               ::mongo::StringData{},                  \
               MESSAGE,                                \
               __VA_ARGS__)

#define LOGV2_WARNING(MESSAGE, ...)                    \
    LOGV2_IMPL(::mongo::logv2::LogSeverity::Warning(), \
               ::mongo::logv2::LogOptions{},           \
               ::mongo::StringData{},                  \
               MESSAGE,                                \
               __VA_ARGS__)

#define LOGV2_WARNING_STABLE(ID, MESSAGE, ...)         \
    LOGV2_IMPL(::mongo::logv2::LogSeverity::Warning(), \
               ::mongo::logv2::LogOptions{},           \
               ID,                                     \
               MESSAGE,                                \
               __VA_ARGS__)

#define LOGV2_DEBUG_OPTIONS_1(DLEVEL, OPTIONS, MESSAGE, ...)                                      \
    do {                                                                                          \
        auto debugRecord = (OPTIONS).domain().openRecord(                                         \
            ::mongo::logv2::LogSeverity::Debug(DLEVEL), (OPTIONS).component(), (OPTIONS).tags()); \
        if (debugRecord.impl()) {                                                                 \
            logv2::detail::doLogRecord(                                                           \
                std::move(debugRecord), (OPTIONS).domain(), FMT_STRING(MESSAGE), __VA_ARGS__);    \
        }                                                                                         \
    } while (false)

#define LOGV2_DEBUG_1(DLEVEL, MESSAGE, ...) \
    LOGV2_DEBUG_OPTIONS_1(DLEVEL, ::mongo::logv2::LogOptions{}, MESSAGE, __VA_ARGS__)

#define LOGV2_DEBUG_OPTIONS_INLINE_1(DLEVEL, OPTIONS, MESSAGE, ...)                       \
    do {                                                                                  \
        auto severity = ::mongo::logv2::LogSeverity::Debug(DLEVEL);                       \
        if (::mongo::logv2::LogManager::global().getGlobalDomain().settings().shouldLog(  \
                MongoLogV2DefaultComponent_component, severity)) {                        \
            LOGV2_IMPL_1(severity, OPTIONS, ::mongo::StringData{}, MESSAGE, __VA_ARGS__); \
        }                                                                                 \
    } while (false)

#define LOGV2_DEBUG_INLINE_1(DLEVEL, MESSAGE, ...) \
    LOGV2_DEBUG_OPTIONS_INLINE_1(DLEVEL, ::mongo::logv2::LogOptions{}, MESSAGE, __VA_ARGS__)

#if BOOST_PP_VARIADICS_MSVC
#define LOGV2(MESSAGE, ...) LOGV2_1(MESSAGE, __VA_ARGS__)
#define LOGV2_OPTIONS(OPTIONS, MESSAGE, ...) LOGV2_OPTIONS_1(OPTIONS, MESSAGE, __VA_ARGS__)
#define LOGV2_DEBUG(DLEVEL, MESSAGE, ...) LOGV2_DEBUG_1(DLEVEL, MESSAGE, __VA_ARGS__)
#define LOGV2_DEBUG_INLINE(DLEVEL, MESSAGE, ...) LOGV2_DEBUG_INLINE_1(DLEVEL, MESSAGE, __VA_ARGS__)
#else
#define LOGV2_0(MESSAGE)                              \
    LOGV2_IMPL_0(::mongo::logv2::LogSeverity::Info(), \
                 ::mongo::logv2::LogOptions{},        \
                 ::mongo::StringData{},               \
                 MESSAGE)

#define LOGV2_OPTIONS_0(OPTIONS, MESSAGE) \
    LOGV2_IMPL_0(::mongo::logv2::LogSeverity::Info(), OPTIONS, ::mongo::StringData{}, MESSAGE)

#define LOGV2_DEBUG_OPTIONS_0(DLEVEL, OPTIONS, MESSAGE)                                           \
    do {                                                                                          \
        auto debugRecord = (OPTIONS).domain().openRecord(                                         \
            ::mongo::logv2::LogSeverity::Debug(DLEVEL), (OPTIONS).component(), (OPTIONS).tags()); \
        if (debugRecord.impl()) {                                                                 \
            logv2::detail::doLogRecord(                                                           \
                std::move(debugRecord), (OPTIONS).domain(), FMT_STRING(MESSAGE));                 \
        }                                                                                         \
    } while (false)

#define LOGV2_DEBUG_0(DLEVEL, MESSAGE) \
    LOGV2_DEBUG_OPTIONS_0(DLEVEL, ::mongo::logv2::LogOptions{}, MESSAGE)

#define LOGV2_DEBUG_OPTIONS_INLINE_0(DLEVEL, OPTIONS, MESSAGE)                           \
    do {                                                                                 \
        auto severity = ::mongo::logv2::LogSeverity::Debug(DLEVEL);                      \
        if (::mongo::logv2::LogManager::global().getGlobalDomain().settings().shouldLog( \
                MongoLogV2DefaultComponent_component, severity)) {                       \
            LOGV2_IMPL_0(severity, OPTIONS, ::mongo::StringData{}, MESSAGE);             \
        }                                                                                \
    } while (false)

#define LOGV2_DEBUG_INLINE_0(DLEVEL, MESSAGE) \
    LOGV2_DEBUG_OPTIONS_INLINE_0(DLEVEL, ::mongo::logv2::LogOptions{}, MESSAGE)

#define LOGV2(...) \
    BOOST_PP_CAT(LOGV2_, BOOST_PP_GREATER(BOOST_PP_VARIADIC_SIZE(__VA_ARGS__), 1))(__VA_ARGS__)
#define LOGV2_OPTIONS(...)                                                                 \
    BOOST_PP_CAT(LOGV2_OPTIONS_, BOOST_PP_GREATER(BOOST_PP_VARIADIC_SIZE(__VA_ARGS__), 2)) \
    (__VA_ARGS__)
#define LOGV2_DEBUG(...)                                                                 \
    BOOST_PP_CAT(LOGV2_DEBUG_, BOOST_PP_GREATER(BOOST_PP_VARIADIC_SIZE(__VA_ARGS__), 2)) \
    (__VA_ARGS__)
#define LOGV2_DEBUG_OPTIONS(...)                                                                 \
    BOOST_PP_CAT(LOGV2_DEBUG_OPTIONS_, BOOST_PP_GREATER(BOOST_PP_VARIADIC_SIZE(__VA_ARGS__), 3)) \
    (__VA_ARGS__)
#define LOGV2_DEBUG_INLINE(...)                                                                 \
    BOOST_PP_CAT(LOGV2_DEBUG_INLINE_, BOOST_PP_GREATER(BOOST_PP_VARIADIC_SIZE(__VA_ARGS__), 2)) \
    (__VA_ARGS__)
#define LOGV2_DEBUG_OPTIONS_INLINE(...)                                    \
    BOOST_PP_CAT(LOGV2_DEBUG_OPTIONS_INLINE_,                              \
                 BOOST_PP_GREATER(BOOST_PP_VARIADIC_SIZE(__VA_ARGS__), 3)) \
    (__VA_ARGS__)

#endif  // BOOST_PP_VARIADICS_MSVC

}  // namespace mongo

#endif  // MONGO_UTIL_LOGV2_H_
