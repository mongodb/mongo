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

#include <boost/optional.hpp>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/client_strand.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include "mongo/util/time_support.h"

namespace mongo {

enum UseMultiServiceSchema : bool {};

/**
 * BSON Collector interface
 *
 * Provides an interface to collect BSONObjs from system providers
 */
class FTDCCollectorInterface {
    FTDCCollectorInterface(const FTDCCollectorInterface&) = delete;
    FTDCCollectorInterface& operator=(const FTDCCollectorInterface&) = delete;

public:
    virtual ~FTDCCollectorInterface() = default;

    /**
     * Name of the collector
     *
     * Used to stamp before and after dates to measure time to collect.
     */
    virtual std::string name() const = 0;

    /**
     * Some collectors can be optionally enabled at runtime but otherwise do not return any data.
     *
     * Returns true if this collector has data to collect.
     */
    virtual bool hasData() const {
        return true;
    }

    /**
     * Collect a sample.
     *
     * If a collector fails to collect data, it should update builder with the result of the
     * failure.
     */
    virtual void collect(OperationContext* opCtx, BSONObjBuilder& builder) = 0;

protected:
    FTDCCollectorInterface() = default;
};

/**
 * Collector filter, used to disable specific collectors at runtime,
 * e.g. when a daemon takes an Arbiter role.
 *
 * The filter function is run every time stats are collected, and
 * collection is skipped if the filter returns false.
 */
class FilteredFTDCCollector : public FTDCCollectorInterface {
public:
    FilteredFTDCCollector(std::function<bool()> pred, std::unique_ptr<FTDCCollectorInterface> coll)
        : _pred(std::move(pred)), _coll(std::move(coll)) {}

    std::string name() const override {
        return _coll->name();
    }

    bool hasData() const override {
        return _pred() && _coll->hasData();
    }

    void collect(OperationContext* opCtx, BSONObjBuilder& builder) override {
        _coll->collect(opCtx, builder);
    }

private:
    std::function<bool()> _pred;
    std::unique_ptr<FTDCCollectorInterface> _coll;
};

/**
 * Interface to manage the set of BSON collectors
 *
 * Not Thread-Safe. Locking is owner's responsibility.
 */
class FTDCCollectorCollection {
    FTDCCollectorCollection(const FTDCCollectorCollection&) = delete;
    FTDCCollectorCollection& operator=(const FTDCCollectorCollection&) = delete;

public:
    virtual ~FTDCCollectorCollection() = default;

    /**
     * Add a metric collector to the collection.
     * Must be called before collect. Cannot be called after collect is called.
     *
     * If the collector has characteristics of the process depending of the acting role, it requires
     * passing a ClusterRole::ShardServer or a ClusterRole::RouterServer role. On the other hand, if
     * the collector is composed by indicators that are specific to the underlying hardware or to
     * the process, it requires a ClusterRole::None.
     */
    virtual void add(std::unique_ptr<FTDCCollectorInterface> collector, ClusterRole role) = 0;

    /**
     * Checks if all collectors associated with each ClusterRole is empty.
     */
    bool empty();
    virtual bool empty(ClusterRole role) = 0;

    /**
     * Collect a sample from all collectors. Called after all adding is complete.
     * Returns a tuple of a sample, and the time at which collecting started.
     *
     * Sample schema:
     * {
     *    "start" : Date_t,    <- Time at which all collecting started
     *    "name" : {           <- name is from name() in FTDCCollectorInterface
     *       "start" : Date_t, <- Time at which name() collection started
     *       "data" : { ... }  <- data comes from collect() in FTDCCollectorInterface
     *       "end" : Date_t,   <- Time at which name() collection ended
     *    },
     *    ...
     *    "end" : Date_t,      <- Time at which all collecting ended
     * }
     */
    std::tuple<BSONObj, Date_t> collect(Client* client, UseMultiServiceSchema multiServiceSchema);

protected:
    FTDCCollectorCollection() = default;

private:
    virtual void _collect(OperationContext* opCtx, ClusterRole role, BSONObjBuilder* builder) = 0;
};

class SampleCollectorCache {
public:
    using DeferredSampleEntry = boost::optional<SemiFuture<BSONObj>>;
    using SampleCollectFn = std::function<void(OperationContext*, BSONObjBuilder*)>;

    struct SampleCollector {
        boost::intrusive_ptr<ClientStrand> clientStrand;
        DeferredSampleEntry updatedValue;
        SampleCollectFn collectFn;
        ClusterRole role;
        int timesSkipped;
        bool hasData;
    };

    SampleCollectorCache(ClusterRole role,
                         Milliseconds maxSampleWaitMS,
                         size_t minThreads,
                         size_t maxThreads);

    ~SampleCollectorCache();

    /**
     * Registers a new SampleCollector.
     */
    void addCollector(StringData name, bool hasData, ClusterRole role, SampleCollectFn&& fn);

    /**
     * Refreshes the data in each SampleCollector and writes the results to builder.
     */
    void refresh(OperationContext* opCtx, BSONObjBuilder* builder);

    void updateSampleTimeout(Milliseconds newValue) {
        _maxSampleWaitMS.store(newValue);
    }

    void updateMinThreads(size_t newValue) {
        stdx::lock_guard lk(_mutex);
        _minThreads = newValue;

        _shutdownPool_inlock(lk);
        _startNewPool(newValue, _maxThreads);
    }

    void updateMaxThreads(size_t newValue) {
        stdx::lock_guard lk(_mutex);
        _maxThreads = newValue;

        _shutdownPool_inlock(lk);
        _startNewPool(_minThreads, newValue);
    }

private:
    void _shutdownPool_inlock(WithLock) {
        _pool->shutdown();
        _pool->join();
    }

    void _startNewPool(size_t minThreads, size_t maxThreads);

    std::map<std::string, SampleCollector> _sampleCollectors;

    ClusterRole _role;
    Atomic<Milliseconds> _maxSampleWaitMS;
    size_t _minThreads;
    size_t _maxThreads;

    stdx::mutex _mutex;
    std::unique_ptr<ThreadPool> _pool;
};

class AsyncFTDCCollectorCollectionSet {
public:
    AsyncFTDCCollectorCollectionSet(ClusterRole role,
                                    Milliseconds maxSampleWaitMS,
                                    size_t minThreads,
                                    size_t maxThreads)
        : _role(role),
          _collectorCache(std::make_unique<SampleCollectorCache>(
              role, std::move(maxSampleWaitMS), minThreads, maxThreads)) {}

    void addCollector(std::unique_ptr<FTDCCollectorInterface> collector, ClusterRole role);

    void collect(OperationContext* opCtx, BSONObjBuilder* builder);

    bool empty() const {
        return _collectors.empty();
    }

    void updateSampleTimeout(Milliseconds newValue) {
        _collectorCache->updateSampleTimeout(newValue);
    }

    void updateMinThreads(size_t newValue) {
        _collectorCache->updateMinThreads(newValue);
    }

    void updateMaxThreads(size_t newValue) {
        _collectorCache->updateMaxThreads(newValue);
    }

private:
    ClusterRole _role;
    std::vector<std::unique_ptr<FTDCCollectorInterface>> _collectors;

    // This must be declared after _collectors so that it is destructed first.
    std::unique_ptr<SampleCollectorCache> _collectorCache;
};

class AsyncFTDCCollectorCollection : public FTDCCollectorCollection {
public:
    AsyncFTDCCollectorCollection(Milliseconds maxSampleWait, size_t minThreads, size_t maxThreads)
        : _none(ClusterRole::None, maxSampleWait, minThreads, maxThreads),
          _router(ClusterRole::RouterServer, maxSampleWait, minThreads, maxThreads),
          _shard(ClusterRole::ShardServer, maxSampleWait, minThreads, maxThreads) {}

    AsyncFTDCCollectorCollectionSet& operator[](ClusterRole role) {
        if (role.hasExclusively(ClusterRole::None))
            return _none;
        if (role.hasExclusively(ClusterRole::RouterServer))
            return _router;
        if (role.hasExclusively(ClusterRole::ShardServer))
            return _shard;
        MONGO_UNREACHABLE;
    }

    AsyncFTDCCollectorCollectionSet& getSet(ClusterRole role) {
        return (*this)[role];
    }

    void add(std::unique_ptr<FTDCCollectorInterface> collector, ClusterRole role) override;

    bool empty(ClusterRole role) override {
        return getSet(role).empty();
    }

    void updateSampleTimeout(Milliseconds newValue) {
        _forEach([&](AsyncFTDCCollectorCollectionSet& set) { set.updateSampleTimeout(newValue); });
    }

    void updateMinThreads(size_t newValue) {
        _forEach([&](AsyncFTDCCollectorCollectionSet& set) { set.updateMinThreads(newValue); });
    }

    void updateMaxThreads(size_t newValue) {
        _forEach([&](AsyncFTDCCollectorCollectionSet& set) { set.updateMaxThreads(newValue); });
    }

private:
    void _collect(OperationContext* opCtx, ClusterRole role, BSONObjBuilder* builder) override;

    void _forEach(std::function<void(AsyncFTDCCollectorCollectionSet&)> f);

    AsyncFTDCCollectorCollectionSet _none;    // Contains collectors & cache for none.
    AsyncFTDCCollectorCollectionSet _router;  // Contains collectors & cache for router.
    AsyncFTDCCollectorCollectionSet _shard;   // Contains collectors & cache for shard.
};


class SyncFTDCCollectorCollectionSet {
public:
    /**
     * Returns the sequence of collectors for the specified `role`.
     * The `role` must be exactly one of None, ShardServer, or RouterServer.
     */
    std::vector<std::unique_ptr<FTDCCollectorInterface>>& operator[](ClusterRole role) {
        if (role.hasExclusively(ClusterRole::None))
            return _none;
        if (role.hasExclusively(ClusterRole::ShardServer))
            return _shard;
        if (role.hasExclusively(ClusterRole::RouterServer))
            return _router;
        MONGO_UNREACHABLE;
    }

private:
    std::vector<std::unique_ptr<FTDCCollectorInterface>> _none;
    std::vector<std::unique_ptr<FTDCCollectorInterface>> _shard;
    std::vector<std::unique_ptr<FTDCCollectorInterface>> _router;
};

class SyncFTDCCollectorCollection : public FTDCCollectorCollection {
    SyncFTDCCollectorCollection(const SyncFTDCCollectorCollection&) = delete;
    SyncFTDCCollectorCollection& operator=(const SyncFTDCCollectorCollection&) = delete;

public:
    SyncFTDCCollectorCollection() = default;

    void add(std::unique_ptr<FTDCCollectorInterface> collector, ClusterRole role) override;

    bool empty(ClusterRole role) override {
        return _collectors[role].empty();
    }

private:
    void _collect(OperationContext* opCtx, ClusterRole role, BSONObjBuilder* builder) override;

private:
    // collection of collectors
    SyncFTDCCollectorCollectionSet _collectors;
};

}  // namespace mongo
