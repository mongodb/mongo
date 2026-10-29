// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/repl/clang_checked/lockable_concepts.h"
#include "mongo/db/repl/clang_checked/thread_safety_annotations.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/modules.h"

#include <system_error>

namespace [[MONGO_MOD_PUBLIC]] mongo {
namespace clang_checked {

struct adopt_lock_t {
    explicit adopt_lock_t() = default;
};
constexpr inline adopt_lock_t adopt_lock;

struct defer_lock_t {
    explicit defer_lock_t() = default;
};
constexpr inline defer_lock_t defer_lock;

struct try_to_lock_t {
    explicit try_to_lock_t() = default;
};
constexpr inline try_to_lock_t try_to_lock;

/**
 * lock_guard is analagous to std::lock_guard and is compatible with
 * CheckedMutex defined in checked_mutex.h.
 */
template <BaseLockable MutexT>
class MONGO_LOCKING_SCOPED_CAPABILITY lock_guard {
public:
    using mutex_type = MutexT;

    explicit lock_guard(mutex_type& mu) MONGO_LOCKING_ACQUIRE(mu) : _mu(mu) {
        _mu.lock();
    }

    lock_guard(mutex_type& mu, adopt_lock_t) MONGO_LOCKING_ACQUIRE(mu) : _mu(mu) {}

    lock_guard(lock_guard&&) = delete;
    lock_guard& operator=(lock_guard&&) = delete;

    ~lock_guard() MONGO_LOCKING_RELEASE() {
        _mu.unlock();
    }

    operator WithLock() {
        // We know that we hold the lock.
        return WithLock::withoutLock();
    }

private:
    mutex_type& _mu;
};

/**
 * unique_lock is analagous to std::unique_lock and is compatible with
 * CheckedMutex defined in checked_mutex.h.
 */
template <BaseLockable MutexT>
class MONGO_LOCKING_SCOPED_CAPABILITY unique_lock {
public:
    using mutex_type = MutexT;

    unique_lock() noexcept : _mu(nullptr), _locked(true) {}

    explicit unique_lock(mutex_type& mu) MONGO_LOCKING_ACQUIRE(mu) : _mu(&mu), _locked(true) {
        _mu->lock();
    }

    unique_lock(mutex_type& mu, defer_lock_t) noexcept : _mu(&mu) {}

    unique_lock(mutex_type& mu, try_to_lock_t) MONGO_LOCKING_TRY_ACQUIRE(true, mu) : _mu(&mu) {
        _locked = _mu->try_lock();
    }

    unique_lock(mutex_type& mu, adopt_lock_t) MONGO_LOCKING_ACQUIRE(mu) : _mu(&mu), _locked(true) {
        invariant(_mu->owns_lock());
    }

    unique_lock(unique_lock&& other) noexcept
        : _mu{std::exchange(other._mu, nullptr)}, _locked{std::exchange(other._locked, false)} {}

    unique_lock& operator=(unique_lock&& other) noexcept MONGO_LOCKING_NO_THREAD_SAFETY_ANALYSIS {
        if (this != &other) {
            if (_locked) {
                _mu->unlock();
            }
            _mu = std::exchange(other._mu, nullptr);
            _locked = std::exchange(other._locked, false);
        }
        return *this;
    }

    ~unique_lock() MONGO_LOCKING_RELEASE() {
        if (_locked) {
            _mu->unlock();
        }
    }

    void lock() MONGO_LOCKING_ACQUIRE(_mu) {
        assertHasMutex();
        _mu->lock();
        _locked = true;
    }

    bool try_lock()
    requires TryLockable<mutex_type>
    MONGO_LOCKING_TRY_ACQUIRE(true, _mu) {
        assertHasMutex();
        if (_locked) {
            return false;
        }
        _locked = _mu->try_lock();
        return _locked;
    }

    void unlock() MONGO_LOCKING_RELEASE() {
        assertHasMutex();
        _mu->unlock();
        _locked = false;
    }

    bool owns_lock() const noexcept {
        return _locked;
    }

    mutex_type* mutex() const noexcept {
        return _mu;
    }

    explicit operator bool() const noexcept {
        return _locked;
    }

    void swap(unique_lock& other) noexcept {
        using std::swap;
        swap(_mu, other._mu);
        swap(_locked, other._locked);
    }

    friend void swap(unique_lock& lhs, unique_lock& rhs) {
        lhs.swap(rhs);
    }

    operator WithLock() {
        invariant(owns_lock());
        return WithLock::withoutLock();
    }

private:
    void assertHasMutex() {
        if (MONGO_unlikely(_mu == nullptr)) {
            std::error_code ec = std::make_error_code(std::errc::operation_not_permitted);
            throw std::system_error(ec, "No mutex!");
        }
    }

    mutable mutex_type* _mu;

    bool _locked;
};

}  // namespace clang_checked
}  // namespace mongo
