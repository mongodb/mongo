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

#include <type_traits>
#include <utility>

namespace mongo {

/**
 * A helper class for defining reliable static duration variables that are safe at
 * shutdown. It's also useful as a greppable identifier to emphasize, track, and
 * whitelist such objects across the codebase. This is a way to make an object that
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
