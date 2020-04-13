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

#include <memory>
#include <vector>

#include "mongo/platform/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/lru_cache.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

/**
 * Extension over 'LRUCache', which provides thread-safety, introspection and most importantly the
 * ability to mark entries as invalid to indicate to potential callers that they should not be used
 * anymore.
 */
template <typename Key, typename Value>
class InvalidatingLRUCache {
    /**
     * Data structure representing the values stored in the cache.
     */
    struct StoredValue {
        /**
         * The 'owningCache' and 'key' values can be nullptr/boost::none in order to support the
         * detached mode of ValueHandle below.
         */
        StoredValue(InvalidatingLRUCache* in_owningCache,
                    boost::optional<Key> in_key,
                    Value&& in_value)
            : owningCache(in_owningCache), key(in_key), value(std::move(in_value)) {}

        ~StoredValue() {
            if (owningCache) {
                stdx::lock_guard<Latch> lg(owningCache->_mutex);
                owningCache->_evictedCheckedOutValues.erase(*key);
            }
        }

        // The cache which stores this key/value pair
        InvalidatingLRUCache* owningCache;

        // The key/value pair. See the comments on the constructor about why the key is optional.
        boost::optional<Key> key;
        Value value;

        // Initially set to true to indicate that the entry is valid and can be read without
        // synchronisation. Transitions to false only once, under `_mutex` in order to mark the
        // entry as invalid.
        AtomicWord<bool> isValid{true};
    };
    using Cache = LRUCache<Key, std::shared_ptr<StoredValue>>;

public:
    using key_type = typename Cache::key_type;
    using mapped_type = typename Cache::mapped_type;

    /**
     * The 'cacheSize' parameter specifies the maximum size of the cache before the least recently
     * used entries start getting evicted. It is allowed to be zero, in which case no entries will
     * actually be cached, which is only meaningful for the behaviour of `insertOrAssignAndGet`.
     */
    explicit InvalidatingLRUCache(size_t cacheSize) : _cache(cacheSize) {}

    ~InvalidatingLRUCache() {
        invariant(_evictedCheckedOutValues.empty());
    }

    /**
     * Wraps the entries returned from the cache.
     */
    class ValueHandle {
    public:
        // The two constructors below are present in order to offset the fact that the cache doesn't
        // support pinning items. Their only usage must be in the authorization mananager for the
        // internal authentication user.
        explicit ValueHandle(Value&& value)
            : _value(std::make_shared<StoredValue>(nullptr, boost::none, std::move(value))) {}

        ValueHandle() = default;

        operator bool() const {
            return bool(_value);
        }

        bool isValid() const {
            return _value->isValid.loadRelaxed();
        }

        Value* get() {
            invariant(bool(*this));
            return &_value->value;
        }

        const Value* get() const {
            invariant(bool(*this));
            return &_value->value;
        }

        Value& operator*() {
            return *get();
        }

        const Value& operator*() const {
            return *get();
        }

        Value* operator->() {
            return get();
        }

        const Value* operator->() const {
            return get();
        }

    private:
        friend class InvalidatingLRUCache;

        explicit ValueHandle(std::shared_ptr<StoredValue> value) : _value(std::move(value)) {}

        std::shared_ptr<StoredValue> _value;
    };

    /**
     * Inserts or updates a key with a new value. If 'key' was checked-out at the time this method
     * was called, it will become invalidated.
     */
    void insertOrAssign(const Key& key, Value&& value) {
        LockGuardWithPostUnlockDestructor guard(_mutex);
        _invalidate(&guard, key, _cache.find(key));
        if (auto evicted = _cache.add(
                key, std::make_shared<StoredValue>(this, key, std::forward<Value>(value)))) {
            const auto& evictedKey = evicted->first;
            auto& evictedValue = evicted->second;

            if (evictedValue.use_count() != 1) {
                invariant(_evictedCheckedOutValues.emplace(evictedKey, evictedValue).second);
            } else {
                invariant(evictedValue.use_count() == 1);

                // Since the cache had the only reference to the evicted value, there could be
                // nobody who has that key checked out who might be interested in listening for it
                // getting invalidated, so we can safely discard it without adding it to the
                // _evictedCheckedOutValues map
            }

            // evictedValue must always be handed-off to guard so that the destructor never runs run
            // while the mutex is held
            guard.releasePtr(std::move(evictedValue));
        }
    }

    /**
     * Same as 'insertOrAssign' above, but also immediately checks-out the newly inserted value and
     * returns it. See the 'get' method below for the semantics of checking-out a value.
     *
     * For caches of size zero, this method will not cache the passed-in value, but it will be
     * returned and the `get` method will continue returning it until all returned handles are
     * destroyed.
     */
    ValueHandle insertOrAssignAndGet(const Key& key, Value&& value) {
        LockGuardWithPostUnlockDestructor guard(_mutex);
        _invalidate(&guard, key, _cache.find(key));
        if (auto evicted = _cache.add(
                key, std::make_shared<StoredValue>(this, key, std::forward<Value>(value)))) {
            const auto& evictedKey = evicted->first;
            auto& evictedValue = evicted->second;

            if (evictedValue.use_count() != 1) {
                invariant(_evictedCheckedOutValues.emplace(evictedKey, evictedValue).second);
            } else {
                invariant(evictedValue.use_count() == 1);

                if (evictedKey == key) {
                    // This handles the zero cache size case where the inserted value was
                    // immediately evicted. Because it still needs to be tracked for invalidation
                    // purposes, we need to add it to the _evictedCheckedOutValues map.
                    invariant(_evictedCheckedOutValues.emplace(evictedKey, evictedValue).second);
                    return ValueHandle(std::move(evictedValue));
                } else {
                    // Since the cache had the only reference to the evicted value, there could be
                    // nobody who has that key checked out who might be interested in listening for
                    // it getting invalidated, so we can safely discard it without adding it to the
                    // _evictedCheckedOutValues map
                }
            }

            // evictedValue must always be handed-off to guard so that the destructor never runs
            // while the mutex is held
            guard.releasePtr(std::move(evictedValue));
        }

        auto it = _cache.find(key);
        invariant(it != _cache.end());
        return ValueHandle(it->second);
    }

    /**
     * Returns the specified key, if found in the cache. Checking-out the value does not pin it and
     * it could still get evicted if the cache is under pressure. The returned handle must be
     * destroyed before the owning cache object itself is destroyed.
     */
    ValueHandle get(const Key& key) {
        stdx::lock_guard<Latch> lg(_mutex);
        if (auto it = _cache.find(key); it != _cache.end())
            return ValueHandle(it->second);

        if (auto it = _evictedCheckedOutValues.find(key); it != _evictedCheckedOutValues.end())
            return ValueHandle(it->second.lock());

        return ValueHandle(nullptr);
    }

    /**
     * Marks 'key' as invalid if it is found in the cache (whether checked-out or not).
     */
    void invalidate(const Key& key) {
        LockGuardWithPostUnlockDestructor guard(_mutex);
        _invalidate(&guard, key, _cache.find(key));
    }

    /**
     * Performs the same logic as 'invalidate' above of all items in the cache, which match the
     * predicate.
     */
    template <typename Pred>
    void invalidateIf(Pred predicate) {
        LockGuardWithPostUnlockDestructor guard(_mutex);
        for (auto it = _cache.begin(); it != _cache.end();) {
            if (predicate(it->first, &it->second->value)) {
                auto itToInvalidate = it++;
                _invalidate(&guard, itToInvalidate->first, itToInvalidate);
            } else {
                it++;
            }
        }

        for (auto it = _evictedCheckedOutValues.begin(); it != _evictedCheckedOutValues.end();) {
            if (auto storedValue = it->second.lock()) {
                if (predicate(it->first, &storedValue->value)) {
                    _invalidate(&guard, (it++)->first, _cache.end());
                } else {
                    it++;
                }
            } else {
                it++;
            }
        }
    }

    struct CachedItemInfo {
        Key key;            // The key of the item in the cache
        long int useCount;  // The number of callers of 'get', which still have the item checked-out
    };

    /**
     * Returns a vector of info about the valid items in the cache for reporting purposes. Any
     * entries, which have been invalidated will not be included, even if they are currently
     * checked-out.
     */
    std::vector<CachedItemInfo> getCacheInfo() const {
        stdx::lock_guard<Latch> lg(_mutex);

        std::vector<CachedItemInfo> ret;
        ret.reserve(_cache.size() + _evictedCheckedOutValues.size());

        for (const auto& kv : _cache) {
            const auto& value = kv.second;
            ret.push_back({kv.first, value.use_count() - 1});
        }

        for (const auto& kv : _evictedCheckedOutValues) {
            if (auto value = kv.second.lock())
                ret.push_back({kv.first, value.use_count() - 1});
        }

        return ret;
    }

private:
    /**
     * Used as means to ensure that any objects which are scheduled to be released from '_cache' or
     * the '_evictedCheckedOutValues' map will be destroyed outside of the cache's mutex. This is
     * necessary, because the destructor function also acquires '_mutex'.
     */
    class LockGuardWithPostUnlockDestructor {
    public:
        LockGuardWithPostUnlockDestructor(Mutex& mutex) : _ul(mutex) {}

        void releasePtr(std::shared_ptr<StoredValue>&& value) {
            _valuesToDestroy.emplace_back(std::move(value));
        }

    private:
        // Must be destroyed after '_ul' is destroyed so that any StoredValue destructors execute
        // outside of the cache's mutex
        std::vector<std::shared_ptr<StoredValue>> _valuesToDestroy;

        stdx::unique_lock<Latch> _ul;
    };

    /**
     * Invalidates the item in the cache pointed to by 'it' and, if 'key' is on the
     * '_evictedCheckedOutValues' map, invalidates it as well. The iterator may be _cache.end() and
     * the key may not exist, and after this call will no longer be valid and will not be in either
     * of the maps.
     */
    void _invalidate(LockGuardWithPostUnlockDestructor* guard,
                     const Key& key,
                     typename Cache::iterator it) {
        if (it != _cache.end()) {
            auto& storedValue = it->second;
            storedValue->isValid.store(false);
            guard->releasePtr(std::move(storedValue));
            _cache.erase(it);
            return;
        }

        auto itEvicted = _evictedCheckedOutValues.find(key);
        if (itEvicted == _evictedCheckedOutValues.end())
            return;

        // Locking the evicted pointer value could fail if the last shared reference is concurrently
        // released and drops to zero
        if (auto evictedValue = itEvicted->second.lock()) {
            evictedValue->isValid.store(false);
            guard->releasePtr(std::move(evictedValue));
        }

        _evictedCheckedOutValues.erase(itEvicted);
    }

    // Protects the state below
    mutable Mutex _mutex = MONGO_MAKE_LATCH("InvalidatingLRUCache::_mutex");

    // This map is used to track any values, which were evicted from the LRU cache below, while they
    // were checked out (i.e., their use_count > 1, where the 1 comes from the ownership by
    // '_cache'). The same key may only be in one of the maps - either '_cache' or here, but never
    // on both.
    //
    // It must be destroyed after the entries in '_cache' are destroyed, because their destructors
    // look-up into that map.
    using EvictedCheckedOutValuesMap = stdx::unordered_map<Key, std::weak_ptr<StoredValue>>;
    EvictedCheckedOutValuesMap _evictedCheckedOutValues;

    Cache _cache;
};

}  // namespace mongo
