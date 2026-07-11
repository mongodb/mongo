// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <type_traits>
#include <typeinfo>

[[MONGO_MOD_PUBLIC]];

namespace mongo {

/**
 * Will cast pointer 'a' of type A* to type B* if 'a' points to something that is _exactly_ a 'B'
 * (not a sub-class of B).
 */
template <class DerivedPtr, class Base>
auto exact_pointer_cast(Base* b) -> DerivedPtr {
    static_assert(std::is_pointer<DerivedPtr>::value);
    using Derived = typename std::remove_cv<typename std::remove_pointer<DerivedPtr>::type>::type;
    static_assert(std::is_final<Derived>::value);

    if (b == nullptr) {
        return nullptr;
    }

    if (typeid(*b) == typeid(Derived)) {
        return static_cast<DerivedPtr>(b);
    }

    return nullptr;
}
}  // namespace mongo
