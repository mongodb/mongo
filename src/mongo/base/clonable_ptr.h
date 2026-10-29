// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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

[[MONGO_MOD_PUBLIC]];

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
