// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/platform/atomic.h"
#include "mongo/platform/compiler.h"
#include "mongo/platform/rwmutex.h"
#include "mongo/util/modules.h"

#include <memory>
#include <mutex>
#include <shared_mutex>

[[MONGO_MOD_PUBLIC]];

namespace mongo {
namespace versioned_value_detail {
/**
 * The default policy that works for the most commonly used mutex types that we have anticipated.
 */
struct DefaultLockPolicy {
    auto makeSharedLock(std::mutex& m) const {
        return std::lock_guard(m);
    }

    auto makeExclusiveLock(std::mutex& m) const {
        return std::lock_guard(m);
    }

    auto makeSharedLock(RWMutex& m) const {
        return std::shared_lock(m);  // NOLINT
    }

    auto makeExclusiveLock(RWMutex& m) const {
        return std::lock_guard(m);
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
 * VersionedValue is a synchronization primitive for values that are accessed frequently, but
 * updated rarely. So long as the value remains unchanged, readers can hold on to their snapshots
 * and access the value without acquiring any locks (see `Snapshot`). Readers must make a new
 * snapshot once their current snapshot gets stale -- i.e. `isStale(mySnapshot)` returns `true`.
 *
 * When updates to the value are synchronized via an external mutex, users can call into
 * `unsafePeek` to read the latest configuration without holding onto a snapshot.
 *
 * You can also bring your own `MutexType` for synchronizing reads and writes. You may need to
 * define a new `LockPolicy` if your custom `MutexType` is not covered by the default policy above.
 *
 * Note that while updates to the versioned value via `update` are synchronized, updates to
 * individual snapshots via `refreshSnapshot` are not synchronized. Access to
 * `VersionedValue<...>::Snapshot` values must be synchronized by users of `VersionedValue`.
 */
template <typename ValueType,
          typename MutexType = std::mutex,
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
