// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_detail.h"
#include "mongo/util/modules.h"

#include <string_view>
#include <utility>

namespace mongo {
namespace logv2 {
namespace detail {
// We want to provide unique names even though we are not using the names.
template <std::size_t I>
inline constexpr char digit = "0123456789"[I];
template <std::size_t N>
inline constexpr const char attrName[4] = {'_', digit<(N / 10) % 10>, digit<N % 10>, '\0'};

template <std::size_t... Is, typename... Args>
auto argAttrs_(std::index_sequence<Is...>, const Args&... args) {
    return AttributeStorage((AttrUdl{attrName<Is>} = args)...);
}

template <typename... Args>
auto argAttrs(const Args&... args) {
    return argAttrs_(std::index_sequence_for<Args...>{}, args...);
}
}  // namespace detail

/**
 * Prototype-only unstructured logging, not allowed to commit to master
 *
 * Instead of named attributes as arguments, it accepts loggable types directly.
 * Will emit a log with a single formatted string as "msg", and no attributes.
 *
 * Ex:
 *     #include "mongo/logv2/log_debug.h"
 *     namespace mongo {
 *     void f(int x, double y) {
 *        logd("f called with x={}, y={}", x, y);
 *     }
 *     }  // namespace mongo
 */
template <typename... Args>
void logd(std::string_view message, const Args&... args) {  // NOLINT
    doUnstructuredLogImpl(LogSeverity::Log(),               // NOLINT
                          LogOptions{LogComponent::kDefault},
                          message,
                          detail::argAttrs(args...));
}

}  // namespace logv2

using logv2::logd;  // NOLINT

}  // namespace mongo
