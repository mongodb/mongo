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
#include "mongo/db/operation_context.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/concurrency/thread_pool_interface.h"
#include "mongo/util/functional.h"
#include "mongo/util/future.h"
#include "mongo/util/invalidating_lru_cache.h"

namespace mongo {

/**
 * Serves as a container of the non-templatised parts of the ReadThroughCache class below.
 */
class ReadThroughCacheBase {
    ReadThroughCacheBase(const ReadThroughCacheBase&) = delete;
    ReadThroughCacheBase& operator=(const ReadThroughCacheBase&) = delete;

protected:
    ReadThroughCacheBase(Mutex& mutex, ServiceContext* service, ThreadPoolInterface& threadPool);

    virtual ~ReadThroughCacheBase();

    /**
     * Type used to guard accesses and updates to the cache.
     *
     * Guard object for synchronizing accesses to data cached in ReadThroughCache instances.
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
         * Constructs a cache guard, locking the mutex that synchronizes ReadThroughCache accesses.
         */
        explicit CacheGuard(ReadThroughCacheBase* distCache)
            : _distCache(distCache), _cacheLock(distCache->_cacheWriteMutex) {}

        /**
         * Releases the mutex that synchronizes cache access, if held, and notifies any threads
         * waiting for their own opportunity to update the cache.
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
        ReadThroughCacheBase* const _distCache;

        stdx::unique_lock<Latch> _cacheLock;

        bool _isThisGuardInFetchPhase{false};
        OID _distCacheFetchGenerationAtFetchBegin;
    };

    friend class ReadThroughCacheBase::CacheGuard;

    /**
     * Creates a client and an operation context and executes the specified 'work' under that
     * environment.
     */
    using WorkWithOpContext = unique_function<void(OperationContext*)>;
    void _asyncWork(WorkWithOpContext work);

    /**
     * Updates _fetchGeneration to a new OID
     */
    void _updateCacheGeneration(const CacheGuard&);

    /**
     * Service context under which this cache has been instantiated (used for access to service-wide
     * functionality, such as client/operation context creation)
     */
    ServiceContext* const _serviceContext;

    /**
     * Thread pool, to be used for invoking the blocking loader work.
     */
    ThreadPoolInterface& _threadPool;

    /**
     * Protects _fetchGeneration and _isFetchPhaseBusy.  Manipulated via CacheGuard.
     */
    Mutex& _cacheWriteMutex;

    /**
     * Current generation of cached data.  Updated every time part of the cache gets
     * invalidated.  Protected by CacheGuard.
     */
    OID _fetchGeneration{OID::gen()};

    /**
     * True if there is an update to the _cache in progress, and that update is currently in
     * the "fetch phase", during which it does not hold the _cacheMutex.
     *
     * Manipulated via CacheGuard.
     */
    bool _isFetchPhaseBusy{false};

    /**
     * Condition used to signal that it is OK for another CacheGuard to enter a fetch phase.
     * Manipulated via CacheGuard.
     */
    stdx::condition_variable _fetchPhaseIsReady;
};

/**
 * Implements a generic read-through cache built on top of InvalidatingLRUCache.
 */
template <typename Key, typename Value>
class ReadThroughCache : public ReadThroughCacheBase {
    /**
     * Data structure wrapping and expanding on the values stored in the cache.
     */
    struct StoredValue {
        Value value;

        // Contains the wallclock time of when the value was fetched from the backing storage. This
        // value is not precise and should only be used for diagnostics purposes (i.e., it cannot be
        // relied on to perform any recency comparisons for example).
        Date_t updateWallClockTime;
    };
    using Cache = InvalidatingLRUCache<Key, StoredValue>;

public:
    using LookupFn = std::function<boost::optional<Value>(OperationContext*, const Key&)>;

    /**
     * Common type for values returned from the cache.
     */
    class ValueHandle {
    public:
        // The two constructors below are present in order to offset the fact that the cache doesn't
        // support pinning items. Their only usage must be in the authorization mananager for the
        // internal authentication user.
        ValueHandle(Value&& value) : _valueHandle({std::move(value), Date_t::min()}) {}
        ValueHandle() = default;

        operator bool() const {
            return bool(_valueHandle);
        }

        bool isValid() const {
            return _valueHandle.isValid();
        }

        Value* get() {
            return &_valueHandle->value;
        }

        const Value* get() const {
            return &_valueHandle->value;
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

        /**
         * See the comments for `StoredValue::updateWallClockTime` above.
         */
        Date_t updateWallClockTime() const {
            return _valueHandle->updateWallClockTime;
        }

    private:
        friend class ReadThroughCache;

        ValueHandle(typename Cache::ValueHandle&& valueHandle)
            : _valueHandle(std::move(valueHandle)) {}

        typename Cache::ValueHandle _valueHandle;
    };

    /**
     * If 'key' is found in the cache, returns a ValidHandle, otherwise invokes the blocking
     * 'lookup' method below to fetch the 'key' from the backing store. If the key is not found in
     * the backing store, returns a ValueHandle which defaults to not-set (it's bool operator is
     * false).
     *
     * NOTES:
     *  This is a potentially blocking method.
     *  The returned value may be invalid by the time the caller gets access to it.
     *  TODO SERVER-44978: needs to call acquireAsync and then get.
     */
    ValueHandle acquire(OperationContext* opCtx, const Key& key) {
        while (true) {
            auto cachedValue = _cache.get(key);
            if (cachedValue)
                return ValueHandle(std::move(cachedValue));

            // Otherwise make sure we have the locks we need and check whether and wait on another
            // thread is fetching into the cache
            CacheGuard guard(this);

            while (!(cachedValue = _cache.get(key)) && guard.otherUpdateInFetchPhase()) {
                guard.wait();
            }

            if (cachedValue)
                return ValueHandle(std::move(cachedValue));

            // If there's still no value in the cache, then we need to go and get it. Take the slow
            // path.
            guard.beginFetchPhase();

            auto value = lookup(opCtx, key);
            if (!value)
                return ValueHandle();

            // All this does is re-acquire the _cacheWriteMutex if we don't hold it already - a
            // caller may also call endFetchPhase() after this returns.
            guard.endFetchPhase();

            if (guard.isSameCacheGeneration())
                return ValueHandle(_cache.insertOrAssignAndGet(
                    key, {std::move(*value), _serviceContext->getFastClockSource()->now()}));

            // If the cache generation changed while this thread was in fetch mode, the data
            // associated with the value may now be invalid, so we will throw out the fetched value
            // and retry.
        }
    }

    /**
     * This is an async version of acquire.
     * TODO SERVER-44978: fix this method to make it actually async
     */
    SharedSemiFuture<ValueHandle> acquireAsync(const Key& key) {
        return Future<ValueHandle>::makeReady(acquire(nullptr, key)).share();
    }

    /**
     * Invalidates the given 'key' and immediately replaces it with a new value.
     */
    ValueHandle insertOrAssignAndGet(const Key& key, Value&& newValue, Date_t updateWallClockTime) {
        CacheGuard guard(this);
        _updateCacheGeneration(guard);
        return _cache.insertOrAssignAndGet(key, {std::move(newValue), updateWallClockTime});
    }

    /**
     * The invalidate methods below all marks the given value(s) as invalid and remove them from
     * cache, which means that a subsequent call to acquire will invoke 'lookup'.
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
        _cache.invalidateIf([&](const Key& key, const StoredValue*) { return predicate(key); });
    }

    void invalidateAll() {
        invalidateIf([](const Key&) { return true; });
    }

    /**
     * Returns statistics information about the cache for reporting purposes.
     */
    std::vector<typename Cache::CachedItemInfo> getCacheInfo() const {
        return _cache.getCacheInfo();
    }

protected:
    /**
     * ReadThroughCache constructor, to be called by sub-classes. The 'cacheSize' parameter
     * represents the maximum size of the cache and 'mutex' is for the exclusive use of the
     * ReadThroughCache, the sub-class should never actually use it (apart from passing it to this
     * constructor). Having the Mutex stored by the sub-class allows latch diagnostics to be
     * correctly associated with the sub-class (not the generic ReadThroughCache class).
     */
    ReadThroughCache(Mutex& mutex,
                     ServiceContext* service,
                     ThreadPoolInterface& threadPool,
                     int cacheSize)
        : ReadThroughCacheBase(mutex, service, threadPool), _cache(cacheSize) {}

private:
    /**
     * Provide the value for a key when there is a cache miss.  Sub-classes must implement this
     * function appropriately.  Throw a uassertion to indicate an error while looking up the value,
     * or return value for this key, or boost::none if this key has no value.
     */
    virtual boost::optional<Value> lookup(OperationContext* opCtx, const Key& key) = 0;

    Cache _cache;
};

}  // namespace mongo
