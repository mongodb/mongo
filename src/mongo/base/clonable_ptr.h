/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/platform/compiler.h"
#include "mongo/util/modules.h"

#include <compare>
#include <concepts>
#include <cstddef>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>

MONGO_MOD_PUBLIC;

namespace mongo {
struct DefaultCloner {
    decltype(auto) operator()(const auto& obj) const {
        return obj.clone();
    }
};

/**
 * A smart-pointer type which functions like a `std::unique_ptr` with the added
 * ability to create new copies of the pointee on copy construction.
 *
 * The default `Cloner` assumes that `T` is a "clonable" type (described
 * below).
 *
 * `Cloner` types are invocable, taking a parameter by reference and producing
 * a clone, i.e. a dynamically allocated copy, of it. A `cloner` must be copyable.
 *
 * A "clonable" type can be dynamically copied using `x.clone()`.
 * The return type of `clone()` is unspecified, but must be
 * something that can be used to initialize a `unique_ptr`.
 *
 * class ExampleClonable {
 * public:
 *     virtual ~ExampleClonable();
 *     std::unique_ptr<ExampleClonable> clone() const;
 * };
 *
 * Eventually (C++26), this could be somewhat replaced by `std::polymorphic`.
 */
template <typename T, typename Cloner = DefaultCloner>
class clonable_ptr {
public:
    clonable_ptr() = default;

    explicit(false) clonable_ptr(std::nullptr_t) {}
    clonable_ptr(std::nullptr_t, Cloner cloner)
        : clonable_ptr{PassKey{}, nullptr, std::move(cloner)} {}

    clonable_ptr(const clonable_ptr& o)
        : _data{_cloneWithFunction(o, o._cloner)}, _cloner{o._cloner} {}

    /** If the copied `clonable_ptr` wants to use a different cloner. */
    clonable_ptr(clonable_ptr p, Cloner cloner)
        : _data{std::move(p._data)}, _cloner{std::move(cloner)} {}

    /** Constructible from unique_ptr, with or without a cloner. */
    template <typename U>
    requires std::convertible_to<U*, T*>
    explicit(false) clonable_ptr(std::unique_ptr<U> p) : _data{std::move(p)} {}

    template <typename U>
    requires std::convertible_to<U*, T*>
    clonable_ptr(std::unique_ptr<U> p, Cloner cloner)
        : _data{std::move(p)}, _cloner{std::move(cloner)} {}

    clonable_ptr& operator=(const clonable_ptr& o) {
        if (this != &o) {
            _data = _cloneWithFunction(o, o._cloner);
            _cloner = o._cloner;
        }
        return *this;
    }

    template <typename U>
    requires std::convertible_to<U*, T*>
    clonable_ptr& operator=(std::unique_ptr<U> p) {
        return *this = clonable_ptr{std::move(p), _cloner};
    }

    clonable_ptr& operator=(std::nullptr_t) {
        reset();
        return *this;
    }

    clonable_ptr(clonable_ptr&&) = default;
    clonable_ptr& operator=(clonable_ptr&&) = default;

    T& operator*() const {
        return *_data;
    }

    T* operator->() const {
        return _data.operator->();
    }

    explicit operator bool() const noexcept {
        return !!_data;
    }

    T* get() const {
        return _data.get();
    }

    void reset() {
        _data.reset();
    }

    void reset(T* p) {
        _data.reset(p);
    }

    bool operator==(const clonable_ptr& p) const {
        return _data == p._data;
    }
    auto operator<=>(const clonable_ptr& p) const {
        return _data <=> p._data;
    }

    bool operator==(std::nullptr_t) const {
        return _data == nullptr;
    }
    auto operator<=>(std::nullptr_t) const {
        return _data <=> nullptr;
    }

private:
    struct PassKey {
        explicit PassKey() = default;
    };

    static std::unique_ptr<T> _cloneWithFunction(const clonable_ptr& src, const Cloner& cloner) {
        return std::unique_ptr<T>{src ? cloner(*src) : nullptr};
    }

    clonable_ptr(PassKey, std::unique_ptr<T> p, Cloner f)
        : _data{std::move(p)}, _cloner{std::move(f)} {}

    std::unique_ptr<T> _data;
    MONGO_COMPILER_NO_UNIQUE_ADDRESS Cloner _cloner;
};

}  // namespace mongo
