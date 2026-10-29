// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <type_traits>
#include <utility>

namespace [[MONGO_MOD_PUBLIC]] mongo {
namespace stdx {

/** https://en.cppreference.com/w/cpp/utility/to_underlying */
template <typename E>
constexpr auto to_underlying(E e) noexcept {
    static_assert(std::is_enum_v<E>, "E is not an enumeration");
    return static_cast<std::underlying_type_t<E>>(e);
}

}  // namespace stdx
}  // namespace mongo
