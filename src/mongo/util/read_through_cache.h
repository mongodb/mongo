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
     * This method is an extension of ThreadPoolInterface::schedule, which in addition creates a
     * client and an operation context and executes the specified 'work' under that environment. The
     * difference is that instead of passing a status to 'work' in order to indicate an in-line
     * execution, the function will throw without actually calling 'work' (see 'schedule' for more
     * details on in-line execution).
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
    CancelToken _asyncWork(WorkWithOpContext work);

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
     * If 'key' is found in the cache, returns a set ValueHandle (its operator bool will be true).
     * Otherwise, either causes the blocking 'lookup' below to be asynchronously invoked to fetch
     * 'key' from the backing store (or joins an already scheduled invocation) and returns a future
     * which will be signaled when the lookup completes.
     *
     * If the lookup is successful and 'key' is found in the store, it will be cached (so subsequent
     * lookups won't have to re-fetch it) and the future will be set. If 'key' is not found in the
     * backing store, returns a not-set ValueHandle (it's bool operator will be false). If 'lookup'
     * fails, the future will be set to the appropriate exception and nothing will be cached,
     * meaning that subsequent calls to 'acquireAsync' will kick-off 'lookup' again.
     *
     * NOTES:
     *  The returned value may be invalid by the time the caller gets access to it if invalidate is
     *  called for 'key'.
     */
    SharedSemiFuture<ValueHandle> acquireAsync(const Key& key) {
        // Fast path
        if (auto cachedValue = _cache.get(key))
            return {std::move(cachedValue)};

        stdx::unique_lock ul(_mutex);

        // Re-check the cache under a mutex, before kicking-off the asynchronous lookup
        if (auto cachedValue = _cache.get(key))
            return {std::move(cachedValue)};

        // Join an in-progress lookup if one has already been scheduled
        if (auto it = _inProgressLookups.find(key); it != _inProgressLookups.end())
            return it->second->sharedPromise.getFuture();

        // Schedule an asynchronous lookup for the key and then loop around and wait for it to
        // complete

        auto [kickOffAsyncLookupPromise, f] = makePromiseFuture<void>();

        auto emplaceResult =
            _inProgressLookups.emplace(key, std::make_unique<InProgressLookup>(key));
        invariant(emplaceResult.second /* emplaced */);
        auto& inProgressLookup = *emplaceResult.first->second;
        auto sharedFutureToReturn = inProgressLookup.sharedPromise.getFuture();
        ul.unlock();

        // Construct the future chain before scheduling the async work so it doesn't execute inline
        // if it so happens that the async work completes by the time the future is constructed, or
        // if it executes inline due to the task executor being shut down.
        std::move(f)
            .then([this, &inProgressLookup] {
                stdx::unique_lock ul(_mutex);
                return _asyncLookupWhileInvalidated(std::move(ul), inProgressLookup);
            })
            .getAsync([this, key](StatusWith<Value> swValue) {
                stdx::unique_lock ul(_mutex);
                auto it = _inProgressLookups.find(key);
                invariant(it != _inProgressLookups.end());
                auto inProgressLookup = std::move(it->second);
                _inProgressLookups.erase(it);

                StatusWith<ValueHandle> swValueHandle(ErrorCodes::InternalError,
                                                      "ReadThroughCache");
                if (swValue.isOK()) {
                    swValueHandle = ValueHandle(_cache.insertOrAssignAndGet(
                        key,
                        {std::move(swValue.getValue()),
                         _serviceContext->getFastClockSource()->now()}));
                } else if (swValue == ErrorCodes::ReadThroughCacheKeyNotFound) {
                    swValueHandle = ValueHandle();
                } else {
                    swValueHandle = swValue.getStatus();
                }

                ul.unlock();

                inProgressLookup->sharedPromise.setFromStatusWith(std::move(swValueHandle));
            });

        kickOffAsyncLookupPromise.emplaceValue();

        return sharedFutureToReturn;
    }

    /**
     * A blocking variant of 'acquireAsync' above - refer to it for more details.
     *
     * NOTES:
     *  This is a potentially blocking method.
     */
    ValueHandle acquire(OperationContext* opCtx, const Key& key) {
        return acquireAsync(key).get(opCtx);
    }

    /**
     * Invalidates the given 'key' and immediately replaces it with a new value.
     */
    ValueHandle insertOrAssignAndGet(const Key& key, Value&& newValue, Date_t updateWallClockTime) {
        stdx::lock_guard lg(_mutex);
        if (auto it = _inProgressLookups.find(key); it != _inProgressLookups.end())
            it->second->invalidate(lg);
        return _cache.insertOrAssignAndGet(key, {std::move(newValue), updateWallClockTime});
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
    void invalidate(const Key& key) {
        stdx::lock_guard lg(_mutex);
        if (auto it = _inProgressLookups.find(key); it != _inProgressLookups.end())
            it->second->invalidate(lg);
        _cache.invalidate(key);
    }

    template <typename Pred>
    void invalidateIf(const Pred& predicate) {
        stdx::lock_guard lg(_mutex);
        for (auto& entry : _inProgressLookups) {
            if (predicate(entry.first))
                entry.second->invalidate(lg);
        }
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
     * ReadThroughCache constructor, to be called by sub-classes, which implement 'lookup'.
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
                     int cacheSize)
        : ReadThroughCacheBase(mutex, service, threadPool), _cache(cacheSize) {}

    ~ReadThroughCache() {
        invariant(_inProgressLookups.empty());
    }

private:
    /**
     * Provide the value for a key when there is a cache miss.  Sub-classes must implement this
     * function appropriately.  Throw a uassertion to indicate an error while looking up the value,
     * or return value for this key, or boost::none if this key has no value.
     */
    virtual boost::optional<Value> lookup(OperationContext* opCtx, const Key& key) = 0;

    // Refer to the comments on '_asyncLookupWhileInvalidated' for more detail on how this structure
    // is used.
    struct InProgressLookup {
        InProgressLookup(Key key) : key(std::move(key)) {}

        void invalidate(WithLock) {
            invalidated = true;
            if (cancelToken)
                cancelToken->tryCancel();
        }

        Key key;
        SharedPromise<ValueHandle> sharedPromise;

        bool invalidated;
        boost::optional<CancelToken> cancelToken;
    };
    using InProgressLookupsMap = stdx::unordered_map<Key, std::unique_ptr<InProgressLookup>>;

    /**
     * This method is expected to be called with a constructed InProgressLookup object, emplaced on
     * '_inProgressLookups' (represented by the 'inProgressLookup' argument). It implements an
     * asynchronous "while (invalidated)" loop over the in-progress key referenced by
     * 'inProgressLookup', which *must* be kept valid by the caller until the returned Future
     * completes.
     *
     * The returned Future will be complete when that loop exists and will contain the latest value
     * (or error) returned by 'lookup'.
     *
     * If thought of sequentially, the loop looks like this:
     *
     * while (true) {
     *     inProgressLookup.invalidated = false;
     *     inProgressLookup.cancelToken.reset();
     *     valueOrError = lookup(key);
     *     if (!inProgressLookup.invalidated)
     *          return valueOrError;    // signals the future
     * }
     */
    Future<Value> _asyncLookupWhileInvalidated(stdx::unique_lock<Mutex> ul,
                                               InProgressLookup& inProgressLookup) noexcept {
        auto [promise, f] = makePromiseFuture<Value>();
        auto p = std::make_shared<Promise<Value>>(std::move(promise));

        // Construct the future chain before scheduling the async work so it doesn't execute inline
        auto future =
            std::move(f).onCompletion([this, &inProgressLookup](StatusWith<Value> swValue) {
                stdx::unique_lock ul(_mutex);
                if (!inProgressLookup.invalidated)
                    return Future<Value>::makeReady(uassertStatusOK(std::move(swValue)));

                inProgressLookup.cancelToken.reset();
                return _asyncLookupWhileInvalidated(std::move(ul), inProgressLookup);
            });

        invariant(!inProgressLookup.cancelToken);
        inProgressLookup.invalidated = false;
        try {
            inProgressLookup.cancelToken.emplace(_asyncWork([ this, p, &inProgressLookup ](
                OperationContext * opCtx, const Status& status) mutable noexcept {
                p->setWith([&]() mutable {
                    uassertStatusOK(status);
                    auto value = lookup(opCtx, inProgressLookup.key);
                    uassert(ErrorCodes::ReadThroughCacheKeyNotFound,
                            "Internal only: key not found",
                            value);
                    return std::move(*value);
                });
            }));
        } catch (const ExceptionForCat<ErrorCategory::CancelationError>& ex) {
            // The thread pool is being shut down, so this is an inline execution
            invariant(!inProgressLookup.invalidated);
            invariant(!inProgressLookup.cancelToken);

            ul.unlock();
            p->setError(ex.toStatus());
        }

        return std::move(future);
    };

    // Contains all the currently cached keys. This structure is self-synchronising and doesn't
    // require a mutex. However, on cache miss it is accessed under '_mutex', which is safe, because
    // _cache's mutex itself is at level 0.
    //
    // NOTE: From destruction order point of view, because keys first "start" in
    // '_inProgressLookups' and then move on to '_cache' the order of these two fields is important.
    Cache _cache;

    // Keeps track of all the keys, which were attempted to be 'acquireAsync'-ed, weren't found in
    // the cache and are currently in the process of being looked up from the backing store. A
    // single key may only be on this map or in '_cache', but never in both.
    //
    // This map is protected by '_mutex'.
    InProgressLookupsMap _inProgressLookups;
};

}  // namespace mongo
