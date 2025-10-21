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
#include "mongo/base/string_data.h"
#include "mongo/bson/util/builder.h"
#include "mongo/logv2/attribute_storage.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_domain.h"
#include "mongo/logv2/log_options.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/util/errno_util.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <tuple>

#include <boost/optional.hpp>
#include <fmt/format.h>

namespace mongo::logv2 {

// Whether there is a doLogImpl call currently on this thread's stack.
bool loggingInProgress();

// Write message to stderr in a signal-safe manner.
void signalSafeWriteToStderr(StringData message);
namespace detail {

using GetTenantIDFn = std::function<std::string()>;
void setGetTenantIDCallback(GetTenantIDFn&& fn);

using LogCounterCallback = std::function<void()>;
// Must be called before other threads start logging
void setLogCounterCallback(LogCounterCallback);

void doLogImpl(int32_t id,
               LogSeverity const& severity,
               LogOptions const& options,
               StringData message,
               TypeErasedAttributeStorage const& attrs);

void doUnstructuredLogImpl(LogSeverity const& severity,  // NOLINT
                           LogOptions const& options,
                           StringData message,
                           TypeErasedAttributeStorage const& attrs);


// doLogUnpacked overloads require the arguments to be flattened attributes
template <size_t N, typename... Args>
void doLogUnpacked(int32_t id,
                   LogSeverity const& severity,
                   LogOptions const& options,
                   const char (&msg)[N],
                   const NamedArg<Args>&... args) {
    auto attributes = AttributeStorage(args...);

    doLogImpl(id, severity, options, msg, attributes);
}

template <size_t N>
void doLogUnpacked(int32_t id,
                   LogSeverity const& severity,
                   LogOptions const& options,
                   const char (&msg)[N],
                   const DynamicAttributes& dynamicAttrs) {
    doLogImpl(id, severity, options, msg, dynamicAttrs);
}

// Args may be raw attributes or CombinedAttr's here. We need to flatten any combined attributes
// into just raw attributes for doLogUnpacked. We do this building flat tuples for every argument,
// concatenating them into a single tuple that we can expand again using apply.
template <size_t N, typename... Args>
void doLog(int32_t id,
           LogSeverity const& severity,
           LogOptions const& options,
           const char (&msg)[N],
           const Args&... args) {
    std::apply([&](auto&&... tup) { doLogUnpacked(id, severity, options, msg, tup...); },
               std::tuple_cat(toFlatAttributesTupleRef(args)...));
}

}  // namespace detail

}  // namespace mongo::logv2
