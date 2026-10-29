// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/util/builder.h"
#include "mongo/logv2/attribute_storage.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_domain.h"
#include "mongo/logv2/log_options.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <tuple>

#include <boost/optional.hpp>
#include <fmt/format.h>

namespace mongo::logv2 {

// Whether there is a doLogImpl call currently on this thread's stack.
[[MONGO_MOD_PUBLIC]] bool loggingInProgress();

// Write message to stderr in a signal-safe manner.
[[MONGO_MOD_PUBLIC]] void signalSafeWriteToStderr(std::string_view message);
namespace detail {

using GetTenantIDFn = std::function<std::string()>;
[[MONGO_MOD_NEEDS_REPLACEMENT]] void setGetTenantIDCallback(GetTenantIDFn&& fn);

using LogCounterCallback = std::function<void()>;
// Must be called before other threads start logging
void setLogCounterCallback(LogCounterCallback);

[[MONGO_MOD_NEEDS_REPLACEMENT]] void doLogImpl(int32_t id,
                                               LogSeverity const& severity,
                                               LogOptions const& options,
                                               std::string_view message,
                                               TypeErasedAttributeStorage const& attrs,
                                               bool devStacktraces = false);

void doUnstructuredLogImpl(LogSeverity const& severity,  // NOLINT
                           LogOptions const& options,
                           std::string_view message,
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
[[MONGO_MOD_PUBLIC]] void doLog(int32_t id,
                                LogSeverity const& severity,
                                LogOptions const& options,
                                const char (&msg)[N],
                                const Args&... args) {
    std::apply([&](auto&&... tup) { doLogUnpacked(id, severity, options, msg, tup...); },
               std::tuple_cat(toFlatAttributesTupleRef(args)...));
}

}  // namespace detail

}  // namespace mongo::logv2
