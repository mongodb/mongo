// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/assert_util_core.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/modules.h"

#include <memory>
#include <type_traits>

[[MONGO_MOD_PUBLIC]];

namespace mongo {

/**
 * Similar to static_cast, but in debug builds uses RTTI to confirm that the cast
 * is legal at runtime.
 */
template <typename T, typename U>
T checked_cast(U&& u) {
    if constexpr (kDebugBuild) {
        if constexpr (std::is_pointer_v<std::remove_reference_t<U>>) {
            if (!u)
                return nullptr;
            T t = dynamic_cast<T>(u);
            invariant(t);
            return t;
        } else {
            return dynamic_cast<T>(std::forward<U>(u));
        }
    } else {
        return static_cast<T>(std::forward<U>(u));
    }
}

namespace checked_cast_detail {
template <typename T, typename U>
std::shared_ptr<T> checked_pointer_cast(U&& u) {
    if constexpr (kDebugBuild) {
        if (!u)
            return nullptr;
        std::shared_ptr<T> t = std::dynamic_pointer_cast<T>(std::forward<U>(u));
        invariant(t);
        return t;
    } else {
        return std::static_pointer_cast<T>(std::forward<U>(u));
    }
}
}  // namespace checked_cast_detail

/**
 * Similar to static_pointer_cast, but in debug builds uses RTTI to confirm that the cast
 * is legal at runtime.
 */
template <typename T, typename U>
std::shared_ptr<T> checked_pointer_cast(const std::shared_ptr<U>& u) {
    return checked_cast_detail::checked_pointer_cast<T>(u);
}

template <typename T, typename U>
std::shared_ptr<T> checked_pointer_cast(std::shared_ptr<U>&& u) {
    return checked_cast_detail::checked_pointer_cast<T>(std::move(u));
}

}  // namespace mongo
