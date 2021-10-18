/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include <algorithm>
#include <new>
#include <type_traits>

#include "mongo/stdx/new.h"

namespace mongo {

/**
 * A wrapper holding a `T` value aligned to `alignof(T)` or `MinAlign`,
 * whichever is greater (i.e. more strict). The value is accessed with a
 * pointer-like syntax.
 */
template <typename T, size_t MinAlign>
class Aligned {
public:
    using value_type = T;
    static constexpr size_t alignment = std::max(alignof(T), MinAlign);

    template <typename... As>
    explicit Aligned(As&&... args) noexcept(std::is_nothrow_constructible_v<value_type, As&&...>) {
        new (_raw()) value_type(std::forward<As>(args)...);
    }

    Aligned(const Aligned& that) noexcept(std::is_nothrow_copy_constructible_v<value_type>) {
        new (_raw()) value_type(*that);
    }

    Aligned& operator=(const Aligned& that) noexcept(
        std::is_nothrow_copy_assignable_v<value_type>) {
        if (this != &that)
            **this = *that;
        return *this;
    }

    Aligned(Aligned&& that) noexcept(std::is_nothrow_move_constructible_v<value_type>) {
        new (_raw()) value_type(std::move(*that));
    }

    Aligned& operator=(Aligned&& that) noexcept(std::is_nothrow_move_assignable_v<value_type>) {
        if (this != &that)
            **this = std::move(*that);
        return *this;
    }

    ~Aligned() {
        _value().~value_type();
    }

    const value_type& operator*() const& noexcept {
        return _value();
    }

    value_type& operator*() & noexcept {
        return _value();
    }

    value_type&& operator*() && noexcept {
        return std::move(_value());
    }

    const value_type* operator->() const noexcept {
        return &_value();
    }

    value_type* operator->() noexcept {
        return &_value();
    }

private:
    static_assert((MinAlign & (MinAlign - 1)) == 0, "alignments must be a power of two");

    const value_type* _raw() const noexcept {
        return reinterpret_cast<const value_type*>(&_storage);
    }

    value_type* _raw() noexcept {
        return reinterpret_cast<value_type*>(&_storage);
    }

    const value_type& _value() const noexcept {
        return *stdx::launder(_raw());
    }

    value_type& _value() noexcept {
        return *stdx::launder(_raw());
    }

    std::aligned_storage_t<sizeof(value_type), alignment> _storage;
};

/**
 * Swap the values. Should not necessarily require they agreen on alignment.
 * defined out-of-class to work around https://gcc.gnu.org/bugzilla/show_bug.cgi?id=89612
 */
template <typename T, size_t MinAlign>
void swap(Aligned<T, MinAlign>& a,
          Aligned<T, MinAlign>& b) noexcept(std::is_nothrow_swappable_v<T>) {
    using std::swap;
    swap(*a, *b);
}

template <typename T>
using CacheAligned = Aligned<T, stdx::hardware_destructive_interference_size>;

}  // namespace mongo
