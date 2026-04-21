/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/stdx/unordered_map.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"

MONGO_MOD_PUBLIC;

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

private:
    std::mutex _mutex;
    boost::optional<Key> _currentKeyValue;
    stdx::unordered_map<Key, SharedPromise<void>, absl::Hash<Key>> _waiters;
};
}  // namespace mongo
