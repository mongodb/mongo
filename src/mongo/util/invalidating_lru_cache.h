/**
 * Copyright (C) 2018 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects for
 * all of the code used other than as permitted herein. If you modify file(s)
 * with this exception, you may extend this exception to your version of the
 * file(s), but you are not obligated to do so. If you do not wish to do so,
 * delete this exception statement from your version. If you delete this
 * exception statement from all source files in the program, then also delete
 * it in the license file.
 */

#pragma once

#include <memory>
#include <vector>

#include <boost/optional.hpp>

#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/lru_cache.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
/**
 * This class implements an LRU cache that stores Key -> std::unique_ptr<Value> and will return
 * std::shared_ptr<Value> for a given Key. The returned shared_ptr will be returned to the cache
 * when the last copy is destroyed, if the cache was not invalidated between the time it was
 * created and when it was destroyed.
 *
 * Additionally, the owner of the cache can invalidate both items in the LRU cache and items that
 * have been checked out of the cache either by Key or by a predicate function. The lifetime of
 * the checked out shared_ptrs are not effected by invalidation.
 *
 * The Invalidator callback must have the signature of void(const Key& key, Value*) and must not
 * throw.
 */
template <typename Key, typename Value, typename Invalidator>
class InvalidatingLRUCache {
public:
    explicit InvalidatingLRUCache(size_t maxCacheSize, Invalidator invalidator)
        : _cache(maxCacheSize), _invalidator(std::move(invalidator)) {}

    /*
     * Inserts or updates a key with a std::unique_ptr<Value>. The cache will be invalidated if
     * the Key was already in the cache and was active at the time it was updated.
     */
    void insertOrAssign(const Key& key, std::unique_ptr<Value> value) {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _invalidateWithLock(lk, key);
        _cache.add(key, std::move(value));
    }

    /*
     * Inserts or updates a key with a std::unique_ptr<Value> and immediate marks it as active,
     * returning a shared_ptr to value to the caller.
     */
    std::shared_ptr<Value> insertOrAssignAndGet(const Key& key, std::unique_ptr<Value> value) {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _invalidateWithLock(lk, key);
        auto ret = std::shared_ptr<Value>(value.release(), _makeDeleterWithLock(key, _generation));
        bool inserted;
        std::tie(std::ignore, inserted) = _active.emplace(key, ret);
        fassert(50893, inserted);
        return ret;
    }

    /*
     * Invalidates cached and active items by key. If the Key was not in the cache, then the cache
     * will not be invalidated.
     */
    void invalidate(const Key& key) {
        stdx::unique_lock<stdx::mutex> lk(_mutex);
        return _invalidateWithLock(lk, key);
    }

    /*
     * Invalidates any cached or active items if the predicate returns true. The cache will only
     * be invalidated if at least one active item matches the predicate.
     */
    template <typename Pred>
    void invalidateIf(Pred predicate) {
        stdx::lock_guard<stdx::mutex> lk(_mutex);

        for (auto it = _cache.begin(); it != _cache.end();) {
            if (predicate(it->first, it->second.get())) {
                auto toErase = it++;
                _cache.erase(toErase);
            } else {
                it++;
            }
        }

        auto it = _active.begin();
        while (it != _active.end()) {
            auto& kv = *it;
            auto value = kv.second.lock();
            if (value && !predicate(kv.first, value.get())) {
                it++;
                continue;
            }

            _generation++;
            _invalidator(value.get());
            it = _active.erase(it);
        }
    }

    /*
     * Gets an item from the cache by Key, or returns boost::none if the item was not in the
     * cache.
     */
    boost::optional<std::shared_ptr<Value>> get(const Key& key) {
        stdx::unique_lock<stdx::mutex> lk(_mutex);
        auto myGeneration = _generation;

        auto cacheIt = _cache.find(key);
        // If the value is not in _cache, check whether it's in _active
        if (cacheIt == _cache.end()) {
            auto activeIt = _active.find(key);
            // If the key is not in active, return boost::none
            if (activeIt == _active.end())
                return boost::none;

            // If the key was in active, but the weak_ptr was expired, then count that as
            // "not being in the cache" and return boost::none.
            //
            // This is a race with the deleter of the shared_ptr's we return here. There is a
            // small possibility that we could needlessly invalidate the cache here by saying
            // a key is not in the cache, even though it is just about to be. However, this
            // should only be missing a perf optimization rather than causing a correctness
            // problem.
            auto ret = activeIt->second.lock();
            return ret ? boost::optional<std::shared_ptr<Value>>(ret) : boost::none;
        }

        // The value has been found in _cache, so we should convert it from a unique_ptr to a
        // shared_ptr and return that.
        std::unique_ptr<Value> ownedUser(cacheIt->second.release());
        _cache.erase(cacheIt);
        auto deleter = _makeDeleterWithLock(key, myGeneration);

        auto ret = std::shared_ptr<Value>(ownedUser.release(), std::move(deleter));
        bool inserted;
        std::tie(std::ignore, inserted) = _active.emplace(key, ret);

        fassert(50894, inserted);
        return ret;
    }

    /*
     * Represents an item in the cache.
     */
    struct CachedItemInfo {
        Key key;        // the key of the item in the cache
        bool active;    // Whether the item is currently active or in the LRU cache
        long useCount;  // The number of copies of the item (0 if active is false, otherwise >= 1)
    };

    /*
     * Returns a vector of info about items in the cache for testing/reporting purposes
     */
    std::vector<CachedItemInfo> getCacheInfo() const {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        std::vector<CachedItemInfo> ret;
        ret.reserve(_active.size() + _cache.size());

        for (const auto& kv : _active) {
            ret.push_back({kv.first, true, kv.second.use_count()});
        }

        for (const auto& kv : _cache) {
            ret.push_back({kv.first, false, 0});
        }

        return ret;
    }

private:
    template <typename T>
    struct hasIsValidMethod {
    private:
        using yes = std::true_type;
        using no = std::false_type;

        template <typename U>
        static auto test(int) -> decltype(std::declval<U>().isValid() == true, yes());

        template <typename>
        static no test(...);

    public:
        static constexpr bool value = std::is_same<decltype(test<T>(0)), yes>::value;
    };

    static_assert(hasIsValidMethod<Value>::value,
                  "Value type must have a method matching bool isValid()");

    void _invalidateWithLock(WithLock, const Key& key) {
        // Erase any cached user (one that hasn't been given out yet).
        _cache.erase(key);

        // Then invalidate any user we've already given out.
        auto it = _active.find(key);
        if (it == _active.end()) {
            return;
        }

        _generation++;
        auto value = it->second.lock();
        _active.erase(it);
        if (value) {
            _invalidator(value.get());
        }

        return;
    }

    /*
     * This makes a deleter for std::shared_ptr<Value> that will return Value to the _cache
     * on shared_ptr destruction.
     *
     * The value will only be returned to the cache if
     * * the Value is still valid
     * * the generation hasn't changed
     *
     * The deleter will always remove the weak_ptr in _active if
     * * the generation hasn't changed
     * * the weak_ptr in _active is expired
     */
    auto _makeDeleterWithLock(const Key& key, uint64_t myGeneration) -> auto {
        return [this, key, myGeneration](Value* d) {
            std::unique_ptr<Value> owned(d);
            stdx::lock_guard<stdx::mutex> lk(_mutex);
            auto it = _active.find(key);
            if (it != _active.end() && it->second.expired()) {
                _active.erase(it);
            }

            if (!owned->isValid() || myGeneration != _generation) {
                return;
            }

            _cache.add(key, std::move(owned));
        };
    }

    mutable stdx::mutex _mutex;

    // The generation count - items will not be returned to the cache if their generation count
    // does not match the current generation count
    uint64_t _generation = 0;

    // Items that have been checked out of the cache
    stdx::unordered_map<Key, std::weak_ptr<Value>> _active;

    // Items that are inactive but valid
    LRUCache<Key, std::unique_ptr<Value>> _cache;

    // The invalidator object to call when an item removed from the cache by invalidate() or
    // invalidateIf()
    Invalidator _invalidator;
};

}  // namespace mongo
