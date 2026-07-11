// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <type_traits>
#include <utility>

[[MONGO_MOD_PUBLIC]];

namespace mongo {

/**
 * A helper class for defining reliable static duration variables that are safe at
 * shutdown. It's also useful as a greppable identifier to emphasize, track, and
 * allowlist such objects across the codebase. This is a way to make an object that
 * never dies and isn't allocated. T need not be destructible, as it is never destroyed.
 *
 * Notice that in every example, there's a `static` storage class specifier on the
 * defined variable. This is very important.
 *
 * The `StaticImmortal<T>` contains raw memory suitable for storing a `T`, and has a trivial
 * destructor. No action is taken at shutdown time, so the on-board `T` remains undestroyed.
 *
 * Initialization syntax examples:
 *   static StaticImmortal<Map> m{};
 *   static auto m = StaticImmortal<Map>();
 *
 *   // Explicitly specified `T`. Curly braces cannot be collapsed here.
 *   static StaticImmortal<Map> m{{{"hello", 123}, {"bye", 456}}};
 *   static StaticImmortal<Map> m = {{{"hello", 123}, {"bye", 456}}};
 *
 *   // `T` can also be deduced from the initializing value:
 *   static StaticImmortal m = Map{{"hello", 123}, {"bye", 456}};
 *   static StaticImmortal m = [] { return Map{{"hello", 123}, {"bye", 456}}; }();
 *
 *   // As with `optional<T>`, dereference operators `->` and `*` yield a reference to `T`.
 *   auto iter = m->find("hello");
 *   functionTakingAMapReference(*m);
 */
template <typename T>
class StaticImmortal {
public:
    using value_type = T;

    constexpr StaticImmortal() {
        _construct();
    }

    constexpr StaticImmortal(const value_type& obj) {
        _construct(obj);
    }

    constexpr StaticImmortal(value_type&& obj) {
        _construct(std::move(obj));
    }

    template <typename... A>
    explicit constexpr StaticImmortal(A&&... args) {
        _construct(std::forward<A>(args)...);
    }

    StaticImmortal(const StaticImmortal&) = delete;
    StaticImmortal(StaticImmortal&&) = delete;

    constexpr value_type& value() noexcept {
        return *reinterpret_cast<value_type*>(&_storage);
    }
    constexpr const value_type& value() const noexcept {
        return *reinterpret_cast<const value_type*>(&_storage);
    }

    /**
     * @{
     * Deref operators like optional.
     */
    constexpr value_type& operator*() noexcept {
        return value();
    }
    constexpr const value_type& operator*() const noexcept {
        return value();
    }
    constexpr value_type* operator->() noexcept {
        return &value();
    }
    constexpr const value_type* operator->() const noexcept {
        return &value();
    }
    /** @} */

private:
    using Storage = std::aligned_storage_t<sizeof(value_type), alignof(value_type)>;

    template <typename... A>
    constexpr void _construct(A&&... args) {
        new (&_storage) value_type(std::forward<A>(args)...);
    }

    Storage _storage;
};

}  // namespace mongo
