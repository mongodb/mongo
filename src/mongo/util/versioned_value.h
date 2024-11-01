/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include <memory>
#include <shared_mutex>

#include "mongo/platform/atomic.h"
#include "mongo/platform/compiler.h"
#include "mongo/platform/rwmutex.h"
#include "mongo/stdx/mutex.h"

namespace mongo {
namespace versioned_value_detail {
/**
 * The default policy that works for the most commonly used mutex types that we have anticipated.
 */
struct DefaultLockPolicy {
    auto makeSharedLock(stdx::mutex& m) const {
        return stdx::lock_guard(m);
    }

    auto makeExclusiveLock(stdx::mutex& m) const {
        return stdx::lock_guard(m);
    }

    auto makeSharedLock(RWMutex& m) const {
        return std::shared_lock(m);  // NOLINT
    }

    auto makeExclusiveLock(RWMutex& m) const {
        return stdx::lock_guard(m);
    }

    auto makeSharedLock(WriteRarelyRWMutex& m) const {
        return m.readLock();
    }

    auto makeExclusiveLock(WriteRarelyRWMutex& m) const {
        return m.writeLock();
    }
};
}  // namespace versioned_value_detail

/**
 * The ideal synchronization primitive for values that are accessed frequently, but updated rarely.
 * So long as the value remains unchanged, readers can hold on to their snapshots and access the
 * value without acquiring any locks (see `Snapshot`). Readers must make a new snapshot once their
 * current snapshot gets stale -- i.e. `isStale(mySnapshot)` returns `true`.
 *
 * When updates to the value are synchronized via an external mutex, users can call into `peek` to
 * read the latest configuration without holding onto a snapshot.
 *
 * You can also bring your own `MutexType` for synchronizing reads and writes. You may need to
 * define a new `LockPolicy` if your custom `MutexType` is not covered by the default policy above.
 */
template <typename ValueType,
          typename MutexType = stdx::mutex,
          typename LockPolicy = versioned_value_detail::DefaultLockPolicy>
class VersionedValue {
public:
    using VersionType = uint64_t;

    /**
     * Holds a consistent snapshot of the versioned value. Snapshots remain valid regardless of
     * future updates to the versioned value, and can be renewed to track the most recent version.
     */
    class Snapshot {
    public:
        /**
         * Makes an empty snapshot -- empty snapshots are both stale and invalid (i.e. hold a null
         * reference and cannot be dereferenced).
         */
        Snapshot() = default;

        Snapshot(VersionType version, std::shared_ptr<ValueType> value)
            : _version(version), _value(std::move(value)) {}

        VersionType version() const noexcept {
            return _version;
        }

        const ValueType& operator*() const noexcept {
            invariant(!!_value, "Dereferencing an uninitialized snapshot!");
            return *_value;
        }

        const ValueType* operator->() const noexcept {
            return _value.get();
        }

        explicit operator bool() const noexcept {
            return !!_value;
        }

    private:
        VersionType _version = 0;
        std::shared_ptr<ValueType> _value;
    };

    VersionedValue() = default;

    /**
     * Note that the initial version must always be greater than zero so that we can consider
     * default constructed snapshots as stale.
     */
    explicit VersionedValue(std::shared_ptr<ValueType> initialValue)
        : _version(1), _current(std::move(initialValue)) {}

    bool isCurrent(const Snapshot& snapshot) const noexcept {
        return _version.load() == snapshot.version();
    }

    MONGO_COMPILER_NOINLINE Snapshot makeSnapshot() const {
        auto lk = _lockPolicy.makeSharedLock(_mutex);
        return {_version.load(), _current};
    }

    void update(std::shared_ptr<ValueType> newValue) {
        auto lk = _lockPolicy.makeExclusiveLock(_mutex);
        _version.fetchAndAdd(1);
        _current = std::move(newValue);
    }

    void refreshSnapshot(Snapshot& snapshot) const {
        if (MONGO_unlikely(!isCurrent(snapshot))) {
            snapshot = makeSnapshot();
        }
    }

    /**
     * Peek at the current value without using any synchronization. This is only thread-safe if the
     * caller utilizes other means (e.g. a higher level lock) to block other threads from accessing
     * the underlying value (i.e. `_current`). Always prefer using snapshots to access the value,
     * unless there are clear performance gains from skipping synchronization.
     */
    const ValueType& unsafePeek() const {
        invariant(_current, "Attempted to peek at uninitialized value!");
        return *_current;
    }

    const std::shared_ptr<ValueType>& getValue_forTest() const {
        return _current;
    }

private:
    MONGO_COMPILER_NO_UNIQUE_ADDRESS LockPolicy _lockPolicy{};
    mutable MutexType _mutex;
    Atomic<VersionType> _version{0};
    std::shared_ptr<ValueType> _current;
};

}  // namespace mongo
