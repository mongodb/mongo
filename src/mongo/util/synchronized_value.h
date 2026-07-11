// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <mutex>

[[MONGO_MOD_PUBLIC]];

namespace mongo {

/**
 * Provides mutex guarded access to an object.
 *
 * The protected object can be accessed by either:
 * 1. auto tmp = sv.synchronize()
 *    This is useful if you need to do multiple operations on it
 * 2. operator* or operator->
 *    Creates a temporary that holds the lock for the duration of the complete expression.
 * 3. get() - makes a copy of the object while the lock is held
 *
 * Inspired by https://isocpp.org/files/papers/n4033.html and boost::synchronized_value
 */
template <typename T>
class synchronized_value {
public:
    using value_type = T;

    template <typename SV>
    class UpdateGuard {
        template <typename SV_>
        using RequiresMutable = std::enable_if_t<!std::is_const_v<SV_>>;

    public:
        explicit UpdateGuard(SV& sv) : _sv(sv), _lock(_sv._mutex) {}

        // Accessors, always available.
        const value_type& operator*() const {
            return _sv._value;
        }
        const value_type* operator->() const {
            return &_sv._value;
        }
        operator value_type() const {
            return **this;
        }

        // Mutators, only available if non-const.
        template <typename SV_ = SV, typename = RequiresMutable<SV_>>
        value_type& operator*() {
            return _sv._value;
        }

        template <typename SV_ = SV, typename = RequiresMutable<SV_>>
        value_type* operator->() {
            return &_sv._value;
        }

        template <typename SV_ = SV, typename = RequiresMutable<SV_>>
        UpdateGuard& operator=(const value_type& v) {
            _sv._value = v;
            return *this;
        }

        template <typename SV_ = SV, typename = RequiresMutable<SV_>>
        UpdateGuard& operator=(value_type&& v) {
            _sv._value = std::move(v);
            return *this;
        }

    private:
        SV& _sv;
        std::unique_lock<std::mutex> _lock;
    };

    synchronized_value() = default;
    explicit synchronized_value(value_type value) : _value(std::move(value)) {}

    synchronized_value(const synchronized_value&) = delete;
    synchronized_value& operator=(const synchronized_value&) = delete;

    auto synchronize() const {
        return UpdateGuard<std::remove_reference_t<decltype(*this)>>{*this};
    }

    auto synchronize() {
        return UpdateGuard<std::remove_reference_t<decltype(*this)>>{*this};
    }

    /** Lock and return a holder to the value and lock. Const or non-const. */
    auto operator->() const {
        return synchronize();
    }
    auto operator*() const {
        return synchronize();
    }
    /** Return a copy of the protected object. */
    value_type get() const {
        return *synchronize();
    }

    /** Mutators */
    auto operator->() {
        return synchronize();
    }
    auto operator*() {
        return synchronize();
    }

    /** Support assigning from the contained value */
    synchronized_value& operator=(const value_type& v) {
        *synchronize() = v;
        return *this;
    }
    synchronized_value& operator=(value_type&& v) {
        *synchronize() = std::move(v);
        return *this;
    }

    friend bool operator==(const synchronized_value& a, const synchronized_value& b) {
        std::scoped_lock lk(a._mutex, b._mutex);
        return a._value == b._value;
    }
    friend bool operator==(const synchronized_value& a, const value_type& b) {
        return **a == b;
    }
    friend bool operator==(const value_type& a, const synchronized_value& b) {
        return b == a;
    }

    friend bool operator!=(const synchronized_value& a, const synchronized_value& b) {
        return !(a == b);
    }
    friend bool operator!=(const synchronized_value& a, const value_type& b) {
        return !(a == b);
    }
    friend bool operator!=(const value_type& a, const synchronized_value& b) {
        return !(a == b);
    }

private:
    value_type _value;  ///< guarded by _mutex
    mutable std::mutex _mutex;
};

}  // namespace mongo
