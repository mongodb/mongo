// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/stdx/unordered_map.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"

[[MONGO_MOD_PUBLIC]];

namespace mongo {

/*
 * A class containing a list of waiters for a given key value to match an expected value.
 *
 * Multiple waiters on the same key are supported. If the expected key matches the last notified key
 * then this class will immediately return as the expected value matches the current value.
 */
template <typename Key>
requires(std::equality_comparable<Key>)
class WaiterList {
public:
    SharedSemiFuture<void> waitFor(const Key& key) {
        std::unique_lock lk(_mutex);
        if (key == _currentKeyValue) {
            return SemiFuture<void>::makeReady().share();
        }
        if (auto it = _waiters.find(key); it != _waiters.end()) {
            return it->second.getFuture();
        }
        auto [it, _] = _waiters.try_emplace(key);
        return it->second.getFuture();
    }

    void notifyWaiters(const Key& key) {
        std::unique_lock lk(_mutex);
        if (auto nh = _waiters.extract(key)) {
            nh.mapped().emplaceValue();
        }
        _currentKeyValue = key;
    }

    /**
     * Wake waiters based on a custom predicate. This is useful for example in cases where we want
     * to wake waiters on keys that only have partial ordering between them and comparably lesser
     * keys need to be woken.
     *
     * Using this will not update the last seen key value.
     */
    template <typename F>
    requires(std::predicate<F, const Key&>)
    void notifyWaitersBasedOnPredicate(F&& pred) {
        std::unique_lock lk(_mutex);
        auto it = _waiters.begin();
        auto end = _waiters.end();
        while (it != end) {
            if (pred(it->first)) {
                it->second.emplaceValue();
                _waiters.erase(it++);
            } else {
                it++;
            }
        }
    }

    /**
     * Cancels all pending waiters with a specific error code.
     */
    void cancelWaiters(const Status& error) {
        std::unique_lock lk(_mutex);
        for (auto& [_, waiterPromise] : _waiters) {
            waiterPromise.setError(error);
        }
        _waiters.clear();
    }

private:
    std::mutex _mutex;
    boost::optional<Key> _currentKeyValue;
    stdx::unordered_map<Key, SharedPromise<void>, absl::Hash<Key>> _waiters;
};
}  // namespace mongo
