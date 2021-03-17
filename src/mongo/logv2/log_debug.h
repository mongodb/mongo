/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include <array>
#include <fmt/format.h>
#include <utility>

#include "mongo/logv2/log_detail.h"

namespace mongo {
namespace logv2::logd_detail {

// We want to provide unique names even though we are not using the names.
template <std::size_t I>
constexpr auto digit = "0123456789"[I];
template <std::size_t N>
constexpr auto attrNameData =
    std::array{'d', 'u', 'm', 'm', 'y', digit<(N / 10) % 10>, digit<N % 10>};
template <std::size_t N>
constexpr auto attrName = fmt::string_view{attrNameData<N>.data(), attrNameData<N>.size()};

template <std::size_t... Is, typename... Args>
void logd(std::index_sequence<Is...>, StringData message, const Args&... args) {  // NOLINT
    using attrType = fmt::internal::udl_arg<char>;
    auto attributes = detail::makeAttributeStorage((attrType{attrName<Is>} = args)...);
    detail::doUnstructuredLogImpl(LogSeverity::Log(),  // NOLINT
                                  LogOptions{LogComponent::kDefault},
                                  message,
                                  attributes);
}
}  // namespace logv2::logd_detail

/**
 * Prototype-only unstructured logging, not allowed to commit to master
 *
 * Instead of named attributes as arguments, it accepts loggable types directly.
 * Will emit a log with a single formatted string as "msg", and no attributes.
 */
template <typename... Args>
void logd(StringData message, const Args&... args) {                                 // NOLINT
    logv2::logd_detail::logd(std::index_sequence_for<Args...>{}, message, args...);  // NOLINT
}

}  // namespace mongo
