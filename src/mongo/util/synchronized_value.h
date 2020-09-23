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

#include "mongo/platform/mutex.h"
#include "mongo/util/hierarchical_acquisition.h"

namespace mongo {

template <int level = 0>
struct LeveledSynchronizedValueMutexPolicy {
    using mutex_type = Mutex;
    static mutex_type construct() {
        return MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(level), "synchronized_value::_mutex");
    }
};

struct RawSynchronizedValueMutexPolicy {
    using mutex_type = stdx::mutex;  // NOLINT
    static mutex_type construct() {
        return {};
    }
};


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
template <typename T, typename MutexPolicy = LeveledSynchronizedValueMutexPolicy<>>
class synchronized_value {
public:
    using value_type = T;
    using mutex_policy_type = MutexPolicy;

    using mutex_type = typename MutexPolicy::mutex_type;

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
        stdx::unique_lock<mutex_type> _lock;
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
    auto operator-> () const {
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
    auto operator-> () {
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
    mutable mutex_type _mutex = mutex_policy_type::construct();
};

}  // namespace mongo
