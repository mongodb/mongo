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

namespace aligned_detail {
template <size_t step>
constexpr size_t roundUpByStep(size_t value) {
    return (((value + (step - 1)) / step) * step);
}
}  // namespace aligned_detail

/**
 * A wrapper holding a `T` value aligned to `alignof(T)` or
 * `minAlign`, whichever is greater (i.e. more strict). The value is
 * accessed with a pointer-like syntax. Additionally the object will
 * be placed in a buffer no smaller than minStorageSize, of which the
 * contained T object may use no more than maxObjectSize bytes.
 */
template <typename T,
          size_t minAlign = alignof(T),
          size_t minStorageSize = sizeof(T),
          size_t maxObjectSize = minStorageSize>
class Aligned {
public:
    using value_type = T;

    static_assert(sizeof(value_type) <= maxObjectSize);

    static constexpr size_t kAlignment = std::max(alignof(value_type), minAlign);
    static constexpr size_t kStorageSize = std::max(sizeof(value_type), minStorageSize);

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
    static_assert((minAlign & (minAlign - 1)) == 0, "alignments must be a power of two");

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

    std::aligned_storage_t<kStorageSize, kAlignment> _storage;
};

/**
 * Swap the values. Defined out-of-class to work around
 * https://gcc.gnu.org/bugzilla/show_bug.cgi?id=89612
 *
 * TODO: It should be possible to swap Aligned<T> vs Aligned<U> if T
 * and U can be swapped, as well to swap instances with varying
 * alignment, padding, etc. However, defining that generic swap
 * results in ambiguities.
 */
template <typename T, auto... Pack>
void swap(Aligned<T, Pack...>& a, Aligned<T, Pack...>& b) noexcept(std::is_nothrow_swappable_v<T>) {
    using std::swap;
    swap(*a, *b);
}

/**
 * A `CacheExclusive` object is Aligned to the destructive
 * interference size, ensuring that it will start at an address that
 * will not exhibit false sharing with any objects that precede it in
 * memory. Additionally, the storage for the object is padded to a
 * sufficient multiple of the destructive interference size to ensure
 * that it will not exhibit false sharing with any other objects that
 * follow it in memory. However, it is not assured that the embedded T
 * object will internally exhibit true sharing with all of itself, as
 * the contained object is permitted to be larger than the
 * constructive interference size.
 */
template <typename T>
using CacheExclusive =
    Aligned<T,
            stdx::hardware_destructive_interference_size,
            aligned_detail::roundUpByStep<stdx::hardware_destructive_interference_size>(sizeof(T))>;


/**
 * A `CacheCombined` object is Aligned to the constructive
 * interference size, ensuring that it will start at an address that
 * can exhibit true sharing for some forward extent. Additionally, the
 * size of the object is constrained to ensure that all of its state
 * is eligible for true sharing within the same region of constructive
 * interference. However, there is no guarantee that the object will
 * not exhibit false sharing with objects that either precede or
 * follow it in memory, unless those objects are themselves protected
 * from false sharing.
 */
template <typename T>
using CacheCombined = Aligned<T,
                              stdx::hardware_constructive_interference_size,
                              sizeof(T),
                              stdx::hardware_constructive_interference_size>;


/**
 * A `CacheCombinedExclusive` object is Aligned to the destructive
 * interference size, ensuring that it will start at an address that
 * will not exhibit false sharing with objects that precede it in
 * memory. Additionally, the storage for the object is padded to the
 * destructive interference size, ensuring that it will not exhibit
 * false sharing with objects that follow it. Finally, the size of the
 * object is constrained to the constructive interference size,
 * ensuring that the object will internally exhibit true sharing.
 */
template <typename T>
using CacheCombinedExclusive = Aligned<T,
                                       stdx::hardware_destructive_interference_size,
                                       stdx::hardware_destructive_interference_size,
                                       stdx::hardware_constructive_interference_size>;

}  // namespace mongo
