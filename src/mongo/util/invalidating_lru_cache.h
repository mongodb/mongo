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
 * Type indicating that the specific cache instance does not support causal consistency. To be used
 * as the default 'Time' parameter to the 'InvalidatingLRUCache' template, indicating that the cache
 * is not causally consistent.
 */
struct CacheNotCausallyConsistent {
    bool operator==(const CacheNotCausallyConsistent&) const {
        return true;
    }
    bool operator!=(const CacheNotCausallyConsistent&) const {
        return false;
    }
    bool operator>(const CacheNotCausallyConsistent&) const {
        return false;
    }
    bool operator>=(const CacheNotCausallyConsistent&) const {
        return true;
    }
    bool operator<(const CacheNotCausallyConsistent&) const {
        return false;
    }
    bool operator<=(const CacheNotCausallyConsistent&) const {
        return true;
    }
};

/**
 * Helper for determining if a given type is CacheNotCausallyConsistent or not.
 */
template <typename T>
struct isCausallyConsistentImpl : std::true_type {};

template <>
struct isCausallyConsistentImpl<CacheNotCausallyConsistent> : std::false_type {};

template <class T>
inline constexpr bool isCausallyConsistent = isCausallyConsistentImpl<T>::value;

/**
 * Specifies the desired causal consistency for calls to 'get' (and 'acquire', respectively in the
 * ReadThroughCache, which is its main consumer).
 */
enum class CacheCausalConsistency {
    // Provides the fastest acquire semantics, where if the cache already contains a
    // (non-invalidated) value cached, it will be immediately returned. Otherwise, the 'acquire'
    // call will block.
    kLatestCached,

    // Provides a causally-consistent semantics with respect to a previous call to
    // 'advanceTimeInStore', where if the cache's (non-invalidated) value has time == timeInStore,
    // the value will be immediately returned. Otherwise, the 'acquire' call will block.
    kLatestKnown,
};

/**
 * Extension built on top of 'LRUCache', which provides thread-safety, introspection and most
 * importantly the ability to invalidate each entry and/or associate a logical timestamp in order to
 * indicate to potential callers that the entry should not be used anymore.
 *
 * The type for 'Time' must support 'operator <' and its default constructor 'Time()' must provide
 * the lowest possible value for the time.
 */
template <typename Key, typename Value, typename Time = CacheNotCausallyConsistent>
class InvalidatingLRUCache {
    /**
     * Data structure representing the values stored in the cache.
     */
    struct StoredValue {
        /**
         * The 'owningCache' and 'key' values can be nullptr/boost::none in order to support the
         * detached mode of ValueHandle below.
         */
        StoredValue(InvalidatingLRUCache* owningCache,
                    uint64_t epoch,
                    boost::optional<Key>&& key,
                    Value&& value,
                    const Time& time,
                    const Time& timeInStore)
            : owningCache(owningCache),
              epoch(epoch),
              key(std::move(key)),
              value(std::move(value)),
              time(time),
              timeInStore(timeInStore),
              isValid(time == timeInStore) {
            invariant(time <= timeInStore);
        }

        ~StoredValue() {
            if (!owningCache)
                return;

            stdx::unique_lock<Latch> ul(owningCache->_mutex);
            auto& evictedCheckedOutValues = owningCache->_evictedCheckedOutValues;
            auto it = evictedCheckedOutValues.find(*key);

            // The lookup above can encounter the following cases:
            //
            // 1) The 'key' is not on the evictedCheckedOutValues map, because a second value for it
            // was inserted, which was also evicted and all its handles expired (so it got removed)
            if (it == evictedCheckedOutValues.end())
                return;
            auto storedValue = it->second.lock();
            // 2) There are no more references to 'key', but it is stil on the map, which means
            // either we are running its destrutor, or some other thread is running the destructor
            // of a different epoch. In either case it is fine to remove the 'it' because we are
            // under a mutex.
            if (!storedValue) {
                evictedCheckedOutValues.erase(it);
                return;
            }
            ul.unlock();
            // 3) The value for 'key' is for a different epoch, in which case we must dereference
            // the '.lock()'ed storedValue outside of the mutex in order to avoid reentrancy while
            // holding a mutex.
            invariant(storedValue->epoch != epoch);
        }

        // Copy and move constructors need to be deleted in order to avoid having to make the
        // destructor to account for the object having been moved
        StoredValue(StoredValue&) = delete;
        StoredValue& operator=(StoredValue&) = delete;
        StoredValue(StoredValue&&) = delete;
        StoredValue& operator=(StoredValue&&) = delete;

        // The cache which stores this key/value pair
        InvalidatingLRUCache* const owningCache;

        // Identity associated with this value. See the destructor for its usage.
        const uint64_t epoch;

        // The key/value pair. See the comments on the constructor about why the key is optional.
        const boost::optional<Key> key;
        Value value;

        // Timestamp associated with the current 'value'. The semantics of the time is entirely up
        // to the user of the cache, but it must be monotonically increasing for the same key.
        const Time time;

        // Timestamp which the store has indicated as available for 'key' (through a call to
        // 'advanceTimeInStore'). Starts as equal to 'time' and always moves forward, under
        // '_mutex'.
        Time timeInStore;

        // Can be read without synchronisation. Transitions to false only once, under `_mutex` in
        // order to mark the entry as invalid either as a result of 'invalidate' or
        // 'advanceTimeInStore'.
        AtomicWord<bool> isValid;
    };
    using Cache = LRUCache<Key, std::shared_ptr<StoredValue>>;

public:
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
        // The three constructors below are present in order to offset the fact that the cache
        // doesn't support pinning items. Their only usage must be in the authorization mananager
        // for the internal authentication user.
        explicit ValueHandle(Value&& value)
            : _value(std::make_shared<StoredValue>(
                  nullptr, 0, boost::none, std::move(value), Time(), Time())) {}

        explicit ValueHandle(Value&& value, const Time& t)
            : _value(
                  std::make_shared<StoredValue>(nullptr, 0, boost::none, std::move(value), t, t)) {}

        ValueHandle() = default;

        operator bool() const {
            return bool(_value);
        }

        bool isValid() const {
            invariant(bool(*this));
            return _value->isValid.loadRelaxed();
        }

        const Time& getTime() const {
            invariant(bool(*this));
            return _value->time;
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
     *
     * The 'time' parameter is mandatory for causally-consistent caches, but not needed otherwise
     * (since the time never changes).
     */
    void insertOrAssign(const Key& key, Value&& value) {
        MONGO_STATIC_ASSERT_MSG(
            !isCausallyConsistent<Time>,
            "Time must be passed to insertOrAssign on causally consistent caches");
        insertOrAssign(key, std::move(value), Time());
    }

    void insertOrAssign(const Key& key, Value&& value, const Time& time) {
        LockGuardWithPostUnlockDestructor guard(_mutex);
        Time currentTime, currentTimeInStore;
        _invalidate(&guard, key, _cache.find(key), &currentTime, &currentTimeInStore);
        if (auto evicted =
                _cache.add(key,
                           std::make_shared<StoredValue>(this,
                                                         ++_epoch,
                                                         key,
                                                         std::forward<Value>(value),
                                                         time,
                                                         std::max(time, currentTimeInStore)))) {
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
     *
     * The 'time' parameter is mandatory for causally-consistent caches, but not needed otherwise
     * (since the time never changes).
     */
    ValueHandle insertOrAssignAndGet(const Key& key, Value&& value) {
        MONGO_STATIC_ASSERT_MSG(
            !isCausallyConsistent<Time>,
            "Time must be passed to insertOrAssignAndGet on causally consistent caches");
        return insertOrAssignAndGet(key, std::move(value), Time());
    }

    ValueHandle insertOrAssignAndGet(const Key& key, Value&& value, const Time& time) {
        LockGuardWithPostUnlockDestructor guard(_mutex);
        Time currentTime, currentTimeInStore;
        _invalidate(&guard, key, _cache.find(key), &currentTime, &currentTimeInStore);
        if (auto evicted =
                _cache.add(key,
                           std::make_shared<StoredValue>(this,
                                                         ++_epoch,
                                                         key,
                                                         std::forward<Value>(value),
                                                         time,
                                                         std::max(time, currentTimeInStore)))) {
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
    ValueHandle get(
        const Key& key,
        CacheCausalConsistency causalConsistency = CacheCausalConsistency::kLatestCached) {
        stdx::lock_guard<Latch> lg(_mutex);
        std::shared_ptr<StoredValue> storedValue;
        if (auto it = _cache.find(key); it != _cache.end()) {
            storedValue = it->second;
        } else if (auto it = _evictedCheckedOutValues.find(key);
                   it != _evictedCheckedOutValues.end()) {
            storedValue = it->second.lock();
        }

        if (causalConsistency == CacheCausalConsistency::kLatestKnown && storedValue &&
            storedValue->time < storedValue->timeInStore)
            return ValueHandle(nullptr);
        return ValueHandle(std::move(storedValue));
    }

    /**
     * Indicates to the cache that the backing store contains a new value for the specified key,
     * with a timestamp of 'newTimeInStore'.
     *
     * Any already returned ValueHandles will start returning isValid = false. Subsequent calls to
     * 'get' with a causal consistency of 'kLatestCached' will continue to return the value, which
     * is currently cached, but with isValid = false. Calls to 'get' with a causal consistency of
     * 'kLatestKnown' will return no value. It is up to the caller to this function to subsequently
     * either 'insertOrAssign' a new value for the 'key', or to call 'invalidate'.
     */
    void advanceTimeInStore(const Key& key, const Time& newTimeInStore) {
        stdx::lock_guard<Latch> lg(_mutex);
        std::shared_ptr<StoredValue> storedValue;
        if (auto it = _cache.find(key); it != _cache.end()) {
            storedValue = it->second;
        } else if (auto it = _evictedCheckedOutValues.find(key);
                   it != _evictedCheckedOutValues.end()) {
            storedValue = it->second.lock();
        }

        if (!storedValue)
            return;

        if (newTimeInStore > storedValue->timeInStore) {
            storedValue->timeInStore = newTimeInStore;
            storedValue->isValid.store(false);
        }
    }

    /**
     * If 'key' is in the store, returns its currently cached value and its latest 'timeInStore',
     * which can either be from the time of insertion or from the latest call to
     * 'advanceTimeInStore'. Otherwise, returns a nullptr ValueHandle and Time().
     */
    std::pair<ValueHandle, Time> getCachedValueAndTimeInStore(const Key& key) {
        stdx::lock_guard<Latch> lg(_mutex);
        std::shared_ptr<StoredValue> storedValue;
        if (auto it = _cache.find(key); it != _cache.end()) {
            storedValue = it->second;
        } else if (auto it = _evictedCheckedOutValues.find(key);
                   it != _evictedCheckedOutValues.end()) {
            storedValue = it->second.lock();
        }

        if (storedValue) {
            auto timeInStore = storedValue->timeInStore;
            return {ValueHandle(std::move(storedValue)), timeInStore};
        }

        return {ValueHandle(nullptr), Time()};
    }

    /**
     * Marks 'key' as invalid if it is found in the cache (whether checked-out or not).
     *
     * Any already returned ValueHandles will start returning isValid = false. Subsequent calls to
     * 'get' will *not* return value for 'key' until the next call to 'insertOrAssign'.
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
                     typename Cache::iterator it,
                     Time* outTime = nullptr,
                     Time* outTimeInStore = nullptr) {
        if (it != _cache.end()) {
            auto& storedValue = it->second;
            storedValue->isValid.store(false);
            if (outTime)
                *outTime = storedValue->time;
            if (outTimeInStore)
                *outTimeInStore = storedValue->timeInStore;
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
            if (outTime)
                *outTime = evictedValue->time;
            if (outTimeInStore)
                *outTimeInStore = evictedValue->timeInStore;
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

    // An always-incrementing counter from which to obtain "identities" for each value stored in the
    // cache, so that different instantiations for the same key can be differentiated
    uint64_t _epoch{0};

    Cache _cache;
};

}  // namespace mongo
