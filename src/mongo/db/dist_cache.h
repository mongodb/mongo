/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <boost/optional.hpp>

#include "mongo/bson/oid.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/invalidating_lru_cache.h"

namespace mongo {

class OperationContext;

/**
 * A server's cache of cluster-wide information.
 */
template <typename Key, typename UnderlyingValue>
class DistCache {
public:
    /**
     * Wrapper around Value that adds an atomic bool to indicate validity.
     *
     * Every Value object is owned by a DistCache.  The DistCache is the only one that should
     * construct, modify, or delete a Value object.  All other consumers of Value only get const
     * access to the underlying value.  The DistCache is responsible for maintaining the reference
     * count on all Value objects it gives out and must not mutate any Value objects with a non-zero
     * reference count (except to call invalidate()).  Any consumer of a Value object should check
     * isValid() before using it, and if it has been invalidated, it should return the object to the
     * DistCache and fetch a new Value object instance for this Key from the DistCache.
     */
    class Value {
        Value(const Value&) = delete;
        Value& operator=(const Value&) = delete;

    public:
        explicit Value(UnderlyingValue&& underlyingValue)
            : _underlyingValue(std::move(underlyingValue)) {}

        UnderlyingValue& operator*() {
            return _underlyingValue;
        }

        const UnderlyingValue& operator*() const {
            return _underlyingValue;
        }

        UnderlyingValue* operator->() {
            return &_underlyingValue;
        }

        const UnderlyingValue* operator->() const {
            return &_underlyingValue;
        }

        /**
         * Returns true if this copy of information about this value is still valid. If this returns
         * false, this object should no longer be used and should be returned to the
         * DistCache and a new Value object for this Key should be requested.
         */
        bool isValid() const {
            return _isValid.loadRelaxed();
        }

    private:
        friend class DistCache;

        /**
         * Marks this instance of the Value object as invalid, most likely because the underlying
         * value has been updated and needs to be reloaded by the DistCache.
         *
         * This method should *only* be called by the DistCache.
         */
        void _invalidate() {
            _isValid.store(false);
        }

        // The underlying value.
        UnderlyingValue _underlyingValue;

        // Indicates whether the value has been marked as invalid by the DistCache.
        AtomicWord<bool> _isValid{true};
    };

    using ValueHandle = std::shared_ptr<Value>;

    DistCache(const DistCache&) = delete;
    DistCache& operator=(const DistCache&) = delete;

    virtual ~DistCache() = default;

    /**
     * Returns the cache generation identifier.
     */
    OID getCacheGeneration() {
        stdx::lock_guard<Latch> lk(_cacheWriteMutex);
        return _fetchGeneration;
    }

    /**
     * Returns a ValueHandle for the given Key. If there is no entry for this key, then returns
     * boost::none. If the cache already has a value for this key, it returns a handle from the
     * cache, otherwise it obtains the value from the Lookup - this may block for a long time.
     *
     * The returned value may be invalid by the time the caller gets access to it.
     */
    boost::optional<ValueHandle> acquire(OperationContext* opCtx, const Key& key) {
        boost::optional<ValueHandle> cachedValue = _cache.get(key);

        // The _cache is thread-safe, so if we can get a Value out of the cache we don't need to
        // take any locks!
        if (cachedValue) {
            invariant(*cachedValue);
            return *cachedValue;
        }

        // Otherwise make sure we have the locks we need and check whether and wait on another
        // thread is fetching into the cache
        CacheGuard guard(this);

        while (!(cachedValue = _cache.get(key)) && guard.otherUpdateInFetchPhase()) {
            guard.wait();
        }

        if (cachedValue) {
            invariant(*cachedValue);
            return *cachedValue;
        }

        // If there's still no value in the cache, then we need to go and get it. Take the slow
        // path.

        guard.beginFetchPhase();

        auto underlyingValue = lookup(opCtx, key);
        if (!underlyingValue)
            return boost::none;
        auto value = std::make_unique<Value>(std::move(*underlyingValue));

        // All this does is re-acquire the _cacheWriteMutex if we don't hold it already - a caller
        // may also call endFetchPhase() after this returns.
        guard.endFetchPhase();

        ValueHandle ret;
        if (guard.isSameCacheGeneration()) {
            ret = _cache.insertOrAssignAndGet(key, std::move(value));
        } else {
            // If the cache generation changed while this thread was in fetch mode, the data
            // associated with the value may now be invalid, so we must mark it as such.  The caller
            // may still opt to use the information for a short while, but not indefinitely.
            value->_invalidate();
            ret = ValueHandle(std::move(value));
        }

        return ret;
    }

    /**
     * Marks the given value as invalid and removes it from the cache.
     */
    void invalidate(const Key& key) {
        CacheGuard guard(this);
        _updateCacheGeneration(guard);
        _cache.invalidate(key);
    }

    template <typename Pred>
    void invalidateIf(const Pred& predicate) {
        CacheGuard guard(this);
        _updateCacheGeneration(guard);
        _cache.invalidateIf(
            [&](const Key& key, const Value* value) { return predicate(key, &(*(*value))); });
    }

    /**
     * Invalidates the given value and immediately replaces it with a new value.
     */
    ValueHandle revalidate(const Key& key, UnderlyingValue&& newUnderlyingValue) {
        CacheGuard guard(this);

        // Invalidate the old value.
        _cache.invalidate(key);

        // Put the new value into the cache.
        auto newValue = std::make_unique<Value>(std::move(newUnderlyingValue));
        ValueHandle ret = _cache.insertOrAssignAndGet(key, std::move(newValue));

        // Update the cache generation.
        _updateCacheGeneration(guard);

        return ret;
    }

    /**
     * Invalidates all Value objects in the cache and removes them from the cache.
     */
    void invalidateCache() {
        CacheGuard guard(this);
        _updateCacheGeneration(guard);
        _cache.invalidateIf([](const Key& a, const Value*) { return true; });
    }

protected:
    /**
     * DistCache constructor, to be called by sub-classes.  Accepts the initial size of the cache,
     * and a reference to a Mutex.  The Mutex is for the exclusive use of the DistCache, the
     * sub-class should never actually use it (apart from passing it to this constructor).  Having
     * the Mutex stored by the sub-class allows latch diagnostics to be correctly associated with
     * the sub-class (not the generic DistCache class).
     */
    DistCache(int cacheSize, Mutex& mutex)
        : _cache(cacheSize, Invalidator()), _cacheWriteMutex(mutex), _fetchGeneration(OID::gen()) {}

private:
    /**
     * Type used to guard accesses and updates to the cache.
     *
     * Guard object for synchronizing accesses to data cached in DistCache instances.
     * This guard allows one thread to access the cache at a time, and provides an exception-safe
     * mechanism for a thread to release the cache mutex while performing network or disk operations
     * while allowing other readers to proceed.
     *
     * There are two ways to use this guard.  One may simply instantiate the guard like a
     * std::lock_guard, and perform reads or writes of the cache.
     *
     * Alternatively, one may instantiate the guard, examine the cache, and then enter into an
     * update mode by first wait()ing until otherUpdateInFetchPhase() is false, and then
     * calling beginFetchPhase().  At this point, other threads may acquire the guard in the simple
     * manner and do reads, but other threads may not enter into a fetch phase.  During the fetch
     * phase, the thread should perform required network or disk activity to determine what update
     * it will make to the cache.  Then, it should call endFetchPhase(), to reacquire the cache
     * mutex.  At that point, the thread can make its modifications to the cache and let the guard
     * go out of scope.
     *
     * All updates by guards using a fetch-phase are totally ordered with respect to one another,
     * and all guards using no fetch phase are totally ordered with respect to one another, but
     * there is not a total ordering among all guard objects.
     *
     * The cached data has an associated counter, called the cache generation.  If the cache
     * generation changes while a guard is in fetch phase, the fetched data should not be stored
     * into the cache, because some invalidation event occurred during the fetch phase.
     */
    class CacheGuard {
        CacheGuard(const CacheGuard&) = delete;
        CacheGuard& operator=(const CacheGuard&) = delete;

    public:
        /**
         * Constructs a cache guard, locking the mutex that synchronizes DistCache accesses.
         */
        explicit CacheGuard(DistCache* distCache)
            : _isThisGuardInFetchPhase(false),
              _distCache(distCache),
              _cacheLock(distCache->_cacheWriteMutex) {}

        /**
         * Releases the mutex that synchronizes cache access, if held, and notifies
         * any threads waiting for their own opportunity to update the cache.
         */
        ~CacheGuard() {
            if (!_cacheLock.owns_lock()) {
                _cacheLock.lock();
            }
            if (_isThisGuardInFetchPhase) {
                invariant(otherUpdateInFetchPhase());
                _distCache->_isFetchPhaseBusy = false;
                _distCache->_fetchPhaseIsReady.notify_all();
            }
        }

        /**
         * Returns true if the distCache reports that it is in fetch phase.
         */
        bool otherUpdateInFetchPhase() const {
            return _distCache->_isFetchPhaseBusy;
        }

        /**
         * Waits on the _distCache->_fetchPhaseIsReady condition.
         */
        void wait() {
            invariant(!_isThisGuardInFetchPhase);
            _distCache->_fetchPhaseIsReady.wait(_cacheLock,
                                                [&] { return !otherUpdateInFetchPhase(); });
        }

        /**
         * Enters fetch phase, releasing the _distCache->_cacheMutex after recording the current
         * cache generation.
         */
        void beginFetchPhase() {
            invariant(!otherUpdateInFetchPhase());
            _isThisGuardInFetchPhase = true;
            _distCache->_isFetchPhaseBusy = true;
            _distCacheFetchGenerationAtFetchBegin = _distCache->_fetchGeneration;
            _cacheLock.unlock();
        }

        /**
         * Exits the fetch phase, reacquiring the _distCache->_cacheMutex.
         */
        void endFetchPhase() {
            _cacheLock.lock();
            // We do not clear _distCache->_isFetchPhaseBusy or notify waiters until
            // ~CacheGuard(), for two reasons.  First, there's no value to notifying the waiters
            // before you're ready to release the mutex, because they'll just go to sleep on the
            // mutex.  Second, in order to meaningfully check the preconditions of
            // isSameCacheGeneration(), we need a state that means "fetch phase was entered and now
            // has been exited."  That state is _isThisGuardInFetchPhase == true and
            // _lock.owns_lock() == true.
        }

        /**
         * Returns true if _distCache->_fetchGeneration remained the same while this guard was
         * in fetch phase.  Behavior is undefined if this guard never entered fetch phase.
         *
         * If this returns true, do not update the cached data with this
         */
        bool isSameCacheGeneration() const {
            invariant(_isThisGuardInFetchPhase);
            invariant(_cacheLock.owns_lock());
            return _distCacheFetchGenerationAtFetchBegin == _distCache->_fetchGeneration;
        }

    private:
        OID _distCacheFetchGenerationAtFetchBegin;
        bool _isThisGuardInFetchPhase;
        DistCache* _distCache;

        stdx::unique_lock<Latch> _cacheLock;
    };

    friend class DistCache::CacheGuard;

    class Invalidator {
    public:
        void operator()(Value* value) {
            value->_invalidate();
        }
    };

    /**
     * Provide the value for a key when there is a cache miss.  Sub-classes must implement this
     * function appropriately.  Throw a uassertion to indicate an error while looking up the value,
     * or return value for this key, o rboost::none if this key has no value.
     */
    virtual boost::optional<UnderlyingValue> lookup(OperationContext* opCtx, const Key& key) = 0;

    /**
     * Updates _fetchGeneration to a new OID
     */
    void _updateCacheGeneration(const CacheGuard&) {
        _fetchGeneration = OID::gen();
    }

    /**
     * The cache itself is "self-synchronising" (ie. thread-safe), and so does not need to be
     * protected by _cacheWriteMutex.
     */
    InvalidatingLRUCache<Key, Value, Invalidator> _cache;

    /**
     * Protects _fetchGeneration and _isFetchPhaseBusy.  Manipulated via CacheGuard.
     */
    Mutex& _cacheWriteMutex;

    /**
     * Current generation of cached data.  Updated every time part of the cache gets
     * invalidated.  Protected by CacheGuard.
     */
    OID _fetchGeneration;

    /**
     * True if there is an update to the _cache in progress, and that update is currently in
     * the "fetch phase", during which it does not hold the _cacheMutex.
     *
     * Manipulated via CacheGuard.
     */
    bool _isFetchPhaseBusy = false;

    /**
     * Condition used to signal that it is OK for another CacheGuard to enter a fetch phase.
     * Manipulated via CacheGuard.
     */
    stdx::condition_variable _fetchPhaseIsReady;
};

}  // namespace mongo
