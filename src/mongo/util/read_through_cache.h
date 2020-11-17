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
     * This method is an extension of ThreadPoolInterface::schedule, with the following additions:
     *  - Creates a client and an operation context and executes the specified 'work' under that
     * environment
     *  - Returns a CancelToken, which can be used to attempt to cancel 'work'
     *
     * If the task manages to get canceled before it is executed (through a call to tryCancel),
     * 'work' will be invoked out-of-line with a non-OK status, set to error code
     * ReadThroughCacheLookupCanceled.
     */
    class CancelToken {
    public:
        struct TaskInfo;
        CancelToken(std::shared_ptr<TaskInfo> info);
        CancelToken(CancelToken&&);
        ~CancelToken();

        void tryCancel();

    private:
        std::shared_ptr<TaskInfo> _info;
    };
    using WorkWithOpContext = unique_function<void(OperationContext*, const Status&)>;
    CancelToken _asyncWork(WorkWithOpContext work) noexcept;

    Date_t _now();

    // Service context under which this cache has been instantiated (used for access to service-wide
    // functionality, such as client/operation context creation)
    ServiceContext* const _serviceContext;

    // Thread pool to be used for invoking the blocking 'lookup' calls
    ThreadPoolInterface& _threadPool;

    // Used to protect the shared state in the child ReadThroughCache template below. Has a lock
    // level of 3, meaning that while held, it is only allowed to take '_cancelTokenMutex' below and
    // the Client lock.
    Mutex& _mutex;

    // Used to protect calls to 'tryCancel' above. Has a lock level of 2, meaning what while held,
    // it is only allowed to take the Client lock.
    Mutex _cancelTokenMutex = MONGO_MAKE_LATCH("ReadThroughCacheBase::_cancelTokenMutex");
};

template <typename Result, typename Key, typename Value, typename Time>
struct ReadThroughCacheLookup {
    using Fn = unique_function<Result(
        OperationContext*, const Key&, const Value& cachedValue, const Time& timeInStore)>;
};

template <typename Result, typename Key, typename Value>
struct ReadThroughCacheLookup<Result, Key, Value, CacheNotCausallyConsistent> {
    using Fn = unique_function<Result(OperationContext*, const Key&, const Value& cachedValue)>;
};

/**
 * Implements an (optionally) causally consistent read-through cache from Key to Value, built on top
 * of InvalidatingLRUCache.
 *
 * Causal consistency is provided by requiring the backing store to asociate every Value it returns
 * with a logical timestamp of type Time.
 */
template <typename Key, typename Value, typename Time = CacheNotCausallyConsistent>
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
    using Cache = InvalidatingLRUCache<Key, StoredValue, Time>;

public:
    template <typename T>
    static constexpr bool IsComparable = Cache::template IsComparable<T>;

    /**
     * Common type for values returned from the cache.
     */
    class ValueHandle {
    public:
        // The three constructors below are present in order to offset the fact that the cache
        // doesn't support pinning items. Their only usage must be in the authorization mananager
        // for the internal authentication user.
        ValueHandle(Value&& value) : _valueHandle({std::move(value), Date_t::min()}) {}
        ValueHandle(Value&& value, const Time& t)
            : _valueHandle({std::move(value), Date_t::min()}, t) {}
        ValueHandle() = default;

        operator bool() const {
            return bool(_valueHandle);
        }

        bool isValid() const {
            return _valueHandle.isValid();
        }

        const Time& getTime() const {
            return _valueHandle.getTime();
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
     * Signature for a blocking function to provide the value for a key when there is a cache miss.
     *
     * The implementation must throw a uassertion to indicate an error while looking up the value,
     * return boost::none if the key is not found, or return an actual value.
     *
     * See the comments on 'advanceTimeInStore' for additional requirements that this function must
     * fulfill with respect to causal consistency.
     */
    struct LookupResult {
        // The 't' parameter is mandatory for causally-consistent caches, but not needed otherwise
        // (since the time never changes). Using a default of '= CacheNotCausallyConsistent()'
        // allows non-causally-consistent users to not have to pass a second parameter, but would
        // fail compilation if causally-consistent users forget to pass it.
        explicit LookupResult(boost::optional<Value>&& v, Time t = CacheNotCausallyConsistent())
            : v(std::move(v)), t(std::move(t)) {}
        LookupResult(LookupResult&&) = default;
        LookupResult& operator=(LookupResult&&) = default;

        // If boost::none, it means the '_lookupFn' did not find the key in the store
        boost::optional<Value> v;

        // If value is boost::none, specifies the time which was passed to '_lookupFn', effectively
        // meaning, at least as of 'time', there was no entry in the store for the key. Otherwise
        // contains the time that the store returned for the 'value'.
        Time t;
    };

    using LookupFn = typename ReadThroughCacheLookup<LookupResult, Key, ValueHandle, Time>::Fn;

    // Exposed publicly so it can be unit-tested indepedently of the usages in this class. Must not
    // be used independently.
    class InProgressLookup;

    /**
     * If 'key' is found in the cache and it fulfills the requested 'causalConsistency', returns a
     * set ValueHandle (its operator bool will be true). Otherwise, either causes the blocking
     * 'LookupFn' to be asynchronously invoked to fetch 'key' from the backing store or joins an
     * already scheduled invocation) and returns a future which will be signaled when the lookup
     * completes.
     *
     * If the lookup is successful and 'key' is found in the store, it will be cached (so subsequent
     * lookups won't have to re-fetch it) and the future will be set. If 'key' is not found in the
     * backing store, returns a not-set ValueHandle (it's bool operator will be false). If 'lookup'
     * fails, the future will be set to the appropriate exception and nothing will be cached,
     * meaning that subsequent calls to 'acquireAsync' will kick-off 'lookup' again.
     *
     * NOTES:
     *  The returned value may be invalid by the time the caller gets to access it, if 'invalidate'
     *  is called for 'key'.
     */
    TEMPLATE(typename KeyType)
    REQUIRES(IsComparable<KeyType>&& std::is_constructible_v<Key, KeyType>)
    SharedSemiFuture<ValueHandle> acquireAsync(
        const KeyType& key,
        CacheCausalConsistency causalConsistency = CacheCausalConsistency::kLatestCached) {

        // Fast path
        if (auto cachedValue = _cache.get(key, causalConsistency))
            return {std::move(cachedValue)};

        stdx::unique_lock ul(_mutex);

        // Re-check the cache under a mutex, before kicking-off the asynchronous lookup
        if (auto cachedValue = _cache.get(key, causalConsistency))
            return {std::move(cachedValue)};

        // Join an in-progress lookup if one has already been scheduled
        if (auto it = _inProgressLookups.find(key); it != _inProgressLookups.end())
            return it->second->addWaiter(ul);

        // Schedule an asynchronous lookup for the key
        auto [cachedValue, timeInStore] = _cache.getCachedValueAndTimeInStore(key);
        auto [it, emplaced] = _inProgressLookups.emplace(
            key,
            std::make_unique<InProgressLookup>(
                *this, Key(key), ValueHandle(std::move(cachedValue)), std::move(timeInStore)));
        invariant(emplaced);
        auto& inProgressLookup = *it->second;
        auto sharedFutureToReturn = inProgressLookup.addWaiter(ul);

        ul.unlock();

        _doLookupWhileNotValid(Key(key), Status(ErrorCodes::Error(461540), "")).getAsync([](auto) {
        });

        return sharedFutureToReturn;
    }

    /**
     * A blocking variant of 'acquireAsync' above - refer to it for more details.
     *
     * NOTES:
     *  This is a potentially blocking method.
     */
    TEMPLATE(typename KeyType)
    REQUIRES(IsComparable<KeyType>)
    ValueHandle acquire(
        OperationContext* opCtx,
        const KeyType& key,
        CacheCausalConsistency causalConsistency = CacheCausalConsistency::kLatestCached) {
        return acquireAsync(key, causalConsistency).get(opCtx);
    }

    /**
     * Acquires the latest value from the cache, or an empty ValueHandle if the key is not present
     * in the cache.
     *
     * Doesn't attempt to lookup, and so doesn't block.
     */
    TEMPLATE(typename KeyType)
    REQUIRES(IsComparable<KeyType>)
    ValueHandle peekLatestCached(const KeyType& key) {
        return {_cache.get(key, CacheCausalConsistency::kLatestCached)};
    }

    /**
     * Invalidates the given 'key' and immediately replaces it with a new value.
     *
     * The 'time' parameter is mandatory for causally-consistent caches, but not needed otherwise
     * (since the time never changes).
     */
    void insertOrAssign(const Key& key, Value&& newValue, Date_t updateWallClockTime) {
        stdx::lock_guard lg(_mutex);
        if (auto it = _inProgressLookups.find(key); it != _inProgressLookups.end())
            it->second->invalidateAndCancelCurrentLookupRound(lg);
        _cache.insertOrAssign(key, {std::move(newValue), updateWallClockTime});
    }

    void insertOrAssign(const Key& key,
                        Value&& newValue,
                        Date_t updateWallClockTime,
                        const Time& time) {
        stdx::lock_guard lg(_mutex);
        if (auto it = _inProgressLookups.find(key); it != _inProgressLookups.end())
            it->second->invalidateAndCancelCurrentLookupRound(lg);
        _cache.insertOrAssign(key, {std::move(newValue), updateWallClockTime}, time);
    }

    /**
     * Invalidates the given 'key' and immediately replaces it with a new value, returning a handle
     * to the new value.
     *
     * The 'time' parameter is mandatory for causally-consistent caches, but not needed otherwise
     * (since the time never changes).
     */
    ValueHandle insertOrAssignAndGet(const Key& key, Value&& newValue, Date_t updateWallClockTime) {
        stdx::lock_guard lg(_mutex);
        if (auto it = _inProgressLookups.find(key); it != _inProgressLookups.end())
            it->second->invalidateAndCancelCurrentLookupRound(lg);
        return _cache.insertOrAssignAndGet(key, {std::move(newValue), updateWallClockTime});
    }

    ValueHandle insertOrAssignAndGet(const Key& key,
                                     Value&& newValue,
                                     Date_t updateWallClockTime,
                                     const Time& time) {
        stdx::lock_guard lg(_mutex);
        if (auto it = _inProgressLookups.find(key); it != _inProgressLookups.end())
            it->second->invalidateAndCancelCurrentLookupRound(lg);
        return _cache.insertOrAssignAndGet(key, {std::move(newValue), updateWallClockTime}, time);
    }

    /**
     * Indicates to the cache that the backing store has a newer version of 'key', corresponding to
     * 'newTime'. Subsequent calls to 'acquireAsync' with a causal consistency set to 'LatestKnown'
     * will block and perform refresh until the cached value reaches 'newTime'.
     *
     * With respect to causal consistency, the 'LookupFn' used for this cache must provide the
     * guarantee that if 'advanceTimeInStore' is called with a 'newTime', a subsequent call to
     * 'LookupFn' for 'key' must return at least 'newTime' or later.
     *
     * Returns true if the passed 'newTimeInStore' is grater than the time of the currently cached
     * value or if no value is cached for 'key'.
     */
    TEMPLATE(typename KeyType)
    REQUIRES(IsComparable<KeyType>)
    bool advanceTimeInStore(const KeyType& key, const Time& newTime) {
        stdx::lock_guard lg(_mutex);
        if (auto it = _inProgressLookups.find(key); it != _inProgressLookups.end())
            it->second->advanceTimeInStore(lg, newTime);
        return _cache.advanceTimeInStore(key, newTime);
    }

    /**
     * The invalidate methods below guarantee the following:
     *  - All affected keys already in the cache (or returned to callers) will be invalidated and
     *    removed from the cache
     *  - All affected keys, which are in the process of being loaded (i.e., acquireAsync has not
     *    yet completed) will be internally interrupted and rescheduled again, as if 'acquireAsync'
     *    was called *after* the call to invalidate
     *
     * In essence, the invalidate calls serve as a "barrier" for the affected keys.
     */
    TEMPLATE(typename KeyType)
    REQUIRES(IsComparable<KeyType>)
    void invalidate(const KeyType& key) {
        stdx::lock_guard lg(_mutex);
        if (auto it = _inProgressLookups.find(key); it != _inProgressLookups.end())
            it->second->invalidateAndCancelCurrentLookupRound(lg);
        _cache.invalidate(key);
    }

    /**
     * Invalidates all cached entries and in progress lookups with keys that matches the preidcate.
     */
    template <typename Pred>
    void invalidateKeyIf(const Pred& predicate) {
        stdx::lock_guard lg(_mutex);
        for (auto& entry : _inProgressLookups) {
            if (predicate(entry.first))
                entry.second->invalidateAndCancelCurrentLookupRound(lg);
        }
        _cache.invalidateIf([&](const Key& key, const StoredValue*) { return predicate(key); });
    }

    /**
     * Invalidates all cached entries with stored values that matches the preidcate.
     */
    template <typename Pred>
    void invalidateCachedValueIf(const Pred& predicate) {
        stdx::lock_guard lg(_mutex);
        _cache.invalidateIf(
            [&](const Key&, const StoredValue* value) { return predicate(value->value); });
    }

    void invalidateAll() {
        invalidateKeyIf([](const Key&) { return true; });
    }

    /**
     * Returns statistics information about the cache for reporting purposes.
     */
    std::vector<typename Cache::CachedItemInfo> getCacheInfo() const {
        return _cache.getCacheInfo();
    }

    /**
     * ReadThroughCache constructor.
     *
     * The 'mutex' is for the exclusive usage of the ReadThroughCache and must not be used in any
     * way by the implementing class. Having the mutex stored by the sub-class allows latch
     * diagnostics to be correctly associated with the sub-class (not the generic ReadThroughCache
     * class).
     *
     * The 'threadPool' can be used for other purposes, but it is mandatory that by the time this
     * object is destructed that it is shut down and joined so that there are no more asynchronous
     * loading activities going on.
     *
     * The 'cacheSize' parameter specifies the maximum size of the cache before the least recently
     * used entries start getting evicted. It is allowed to be zero, in which case no entries will
     * actually be cached, but it doesn't guarantee that every `acquire` call will result in an
     * invocation of `lookup`. Specifically, several concurrent invocations of `acquire` for the
     * same key may group together for a single `lookup`.
     */
    ReadThroughCache(Mutex& mutex,
                     ServiceContext* service,
                     ThreadPoolInterface& threadPool,
                     LookupFn lookupFn,
                     int cacheSize)
        : ReadThroughCacheBase(mutex, service, threadPool),
          _lookupFn(std::move(lookupFn)),
          _cache(cacheSize) {}

    ~ReadThroughCache() {
        invariant(_inProgressLookups.empty());
    }

private:
    using InProgressLookupsMap = stdx::unordered_map<Key,
                                                     std::unique_ptr<InProgressLookup>,
                                                     LruKeyHasher<Key>,
                                                     LruKeyComparator<Key>>;

    /**
     * This method implements an asynchronous "while (!valid)" loop over 'key', which must be on the
     * in-progress map.
     */
    Future<LookupResult> _doLookupWhileNotValid(Key key, StatusWith<LookupResult> sw) {
        stdx::unique_lock ul(_mutex);
        auto it = _inProgressLookups.find(key);
        invariant(it != _inProgressLookups.end());
        auto& inProgressLookup = *it->second;
        auto [promisesToSet, result, mustDoAnotherLoop] = [&] {
            // The thread pool is shutting down, so terminate the loop
            if (ErrorCodes::isCancelationError(sw.getStatus()))
                return std::make_tuple(inProgressLookup.getAllPromisesOnError(ul),
                                       StatusWith<ValueHandle>(sw.getStatus()),
                                       false);

            // There was a concurrent call to 'invalidate', so start all over
            if (!inProgressLookup.valid(ul))
                return std::make_tuple(
                    std::vector<std::unique_ptr<SharedPromise<ValueHandle>>>{},
                    StatusWith<ValueHandle>(Status(ErrorCodes::Error(461541), "")),
                    true);

            // Lookup resulted in an error, which is not cancelation
            if (!sw.isOK())
                return std::make_tuple(inProgressLookup.getAllPromisesOnError(ul),
                                       StatusWith<ValueHandle>(sw.getStatus()),
                                       false);

            // Value (or boost::none) was returned by lookup and there was no concurrent call to
            // 'invalidate'. Place the value on the cache and return the necessary promises to
            // signal (those which are waiting for time < time at the store).
            auto& result = sw.getValue();
            auto promisesToSet = inProgressLookup.getPromisesLessThanTime(ul, result.t);

            auto valueHandleToSet = [&] {
                if (result.v) {
                    ValueHandle valueHandle(
                        _cache.insertOrAssignAndGet(key, {std::move(*result.v), _now()}, result.t));
                    // In the case that 'key' was not present in the store up to this lookup's
                    // completion, it is possible that concurrent callers advanced the time in store
                    // further than what was returned by the lookup. Because of this, the time in
                    // the cache must be synchronised with that of the InProgressLookup.
                    _cache.advanceTimeInStore(key, inProgressLookup.minTimeInStore(ul));
                    return valueHandle;
                }

                _cache.invalidate(key);
                return ValueHandle();
            }();

            return std::make_tuple(std::move(promisesToSet),
                                   StatusWith<ValueHandle>(std::move(valueHandleToSet)),
                                   !inProgressLookup.empty(ul));
        }();

        if (!mustDoAnotherLoop)
            _inProgressLookups.erase(it);
        ul.unlock();

        // The only reason this loop pops the values as it goes and std::moves into the last value
        // is to support the CacheSizeZero unit-test, which requires that once the future it waits
        // on is set, it contains the last reference on the returned ValueHandle
        while (!promisesToSet.empty()) {
            auto p(std::move(promisesToSet.back()));
            promisesToSet.pop_back();

            if (promisesToSet.empty())
                p->setFrom(std::move(result));
            else
                p->setFrom(result);
        }

        return mustDoAnotherLoop
            ? inProgressLookup.asyncLookupRound().onCompletion(
                  [this, key = std::move(key)](auto sw) mutable {
                      return _doLookupWhileNotValid(std::move(key), std::move(sw));
                  })
            : Future<LookupResult>::makeReady(Status(ErrorCodes::Error(461542), ""));
    }

    // Blocking function which will be invoked to retrieve entries from the backing store
    const LookupFn _lookupFn;

    // Contains all the currently cached keys. This structure is self-synchronising and doesn't
    // require a mutex. However, on cache miss it is accessed under '_mutex', which is safe, because
    // _cache's mutex itself is at level 0.
    //
    // NOTE: From destruction order point of view, because keys first "start" in
    // '_inProgressLookups' and then move on to '_cache' the order of these two fields is important.
    Cache _cache;

    // Keeps track of all the keys, which were attempted to be 'acquireAsync'-ed, weren't found in
    // the cache and are currently in the process of being looked up from the backing store. A
    // single key may be missing from '_cache', or contain an old 'kLatestCached' and have an active
    // lookup on this map for 'kLatestKnown'.
    //
    // This map is protected by '_mutex'.
    InProgressLookupsMap _inProgressLookups;
};

/**
 * This class represents an in-progress lookup for a specific key and implements the guarantees of
 * the invalidation logic as described in the comments of 'ReadThroughCache::invalidate'.
 *
 * It is intended to be used in conjunction with the 'ReadThroughCache', which operates on it under
 * its '_mutex' and ensures there is always at most a single active instance at a time active for
 * each 'key'.
 *
 * The methods of this class are not thread-safe, unless indicated in the comments.
 *
 * Its lifecycle is intended to be like this:
 *
 * inProgressLookups.emplace(inProgress);
 * while (true) {
 *      result = inProgress.asyncLookupRound();
 *      if (!inProgress.valid()) {
 *          continue;
 *      }
 *
 *      inProgressLookups.remove(inProgress)
 *      cachedValues.insert(result);
 *      inProgress.signalWaiters(result);
 * }
 */
template <typename Key, typename Value, typename Time>
class ReadThroughCache<Key, Value, Time>::InProgressLookup {
public:
    InProgressLookup(ReadThroughCache& cache, Key key, ValueHandle cachedValue, Time minTimeInStore)
        : _cache(cache),
          _key(std::move(key)),
          _cachedValue(std::move(cachedValue)),
          _minTimeInStore(std::move(minTimeInStore)) {}

    Future<LookupResult> asyncLookupRound() {
        auto [promise, future] = makePromiseFuture<LookupResult>();

        stdx::lock_guard lg(_cache._mutex);
        _valid = true;
        _cancelToken.emplace(_cache._asyncWork([ this, promise = std::move(promise) ](
            OperationContext * opCtx, const Status& status) mutable noexcept {
            promise.setWith([&] {
                uassertStatusOK(status);
                if constexpr (std::is_same_v<Time, CacheNotCausallyConsistent>) {
                    return _cache._lookupFn(opCtx, _key, _cachedValue);
                } else {
                    auto minTimeInStore = [&] {
                        stdx::lock_guard lg(_cache._mutex);
                        return _minTimeInStore;
                    }();
                    return _cache._lookupFn(opCtx, _key, _cachedValue, minTimeInStore);
                }
            });
        }));

        return std::move(future);
    }

    SharedSemiFuture<ValueHandle> addWaiter(WithLock) {
        auto [it, unusedEmplaced] = _outstanding.try_emplace(
            _minTimeInStore, std::make_unique<SharedPromise<ValueHandle>>());
        return it->second->getFuture();
    }

    Time minTimeInStore(WithLock) const {
        return _minTimeInStore;
    }

    bool valid(WithLock) const {
        return _valid;
    }

    std::vector<std::unique_ptr<SharedPromise<ValueHandle>>> getAllPromisesOnError(WithLock) {
        std::vector<std::unique_ptr<SharedPromise<ValueHandle>>> ret;
        for (auto it = _outstanding.begin(); it != _outstanding.end();) {
            ret.emplace_back(std::move(it->second));
            it = _outstanding.erase(it);
        }
        return ret;
    }

    std::vector<std::unique_ptr<SharedPromise<ValueHandle>>> getPromisesLessThanTime(WithLock,
                                                                                     Time time) {
        invariant(_valid);
        std::vector<std::unique_ptr<SharedPromise<ValueHandle>>> ret;
        for (auto it = _outstanding.begin(); it != _outstanding.end();) {
            if (it->first > time)
                break;
            ret.emplace_back(std::move(it->second));
            it = _outstanding.erase(it);
        }
        return ret;
    }

    bool empty(WithLock) const {
        invariant(_valid);
        return _outstanding.empty();
    }

    void advanceTimeInStore(WithLock, const Time& newTime) {
        if (newTime > _minTimeInStore)
            _minTimeInStore = newTime;
    }

    void invalidateAndCancelCurrentLookupRound(WithLock) {
        _valid = false;
        if (_cancelToken)
            _cancelToken->tryCancel();
    }

private:
    // The owning cache, from which mutex, lookupFn, async task scheduling, etc. will be used. It is
    // the responsibility of the owning cache to join all outstanding lookups at destruction time.
    ReadThroughCache& _cache;

    const Key _key;

    bool _valid{false};
    boost::optional<CancelToken> _cancelToken;

    ValueHandle _cachedValue;
    Time _minTimeInStore;

    using TimeAndPromiseMap = std::map<Time, std::unique_ptr<SharedPromise<ValueHandle>>>;
    TimeAndPromiseMap _outstanding;
};

}  // namespace mongo
