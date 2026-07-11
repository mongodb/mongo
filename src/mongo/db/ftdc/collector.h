// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/client_strand.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <unordered_set>
#include <vector>

#include <boost/optional.hpp>

namespace mongo {

/**
 * BSON Collector interface
 *
 * Provides an interface to collect BSONObjs from system providers
 */
class [[MONGO_MOD_OPEN]] FTDCCollectorInterface {
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
     */
    virtual void add(std::unique_ptr<FTDCCollectorInterface> collector) = 0;

    virtual bool empty() = 0;

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
    std::tuple<BSONObj, Date_t> collect(Client* client,
                                        std::vector<std::pair<std::string, int>>& sectionSizes);

protected:
    FTDCCollectorCollection() = default;

private:
    virtual void _collect(OperationContext* opCtx,
                          BSONObjBuilder* builder,
                          std::vector<std::pair<std::string, int>>& sectionSizes) = 0;
};

class SampleCollectorCache {
public:
    using DeferredSampleEntry = boost::optional<SemiFuture<BSONObj>>;
    using SampleCollectFn = std::function<void(OperationContext*, BSONObjBuilder*)>;

    struct SampleCollector {
        boost::intrusive_ptr<ClientStrand> clientStrand;
        DeferredSampleEntry updatedValue;
        SampleCollectFn collectFn;
        int timesSkipped;
        bool hasData;
    };

    SampleCollectorCache(Milliseconds maxSampleWaitMS, size_t minThreads, size_t maxThreads);

    ~SampleCollectorCache();

    /**
     * Registers a new SampleCollector.
     */
    void addCollector(const std::string& name, bool hasData, SampleCollectFn&& fn);

    /**
     * Refreshes the data in each SampleCollector and writes the results to builder.
     */
    void refresh(OperationContext* opCtx, BSONObjBuilder* builder);

    void updateSampleTimeout(Milliseconds newValue) {
        _maxSampleWaitMS.store(newValue);
    }

    void updateMinThreads(size_t newValue) {
        std::lock_guard lk(_mutex);
        _minThreads = newValue;

        _shutdownPool_inlock(lk);
        _startNewPool(newValue, _maxThreads);
    }

    void updateMaxThreads(size_t newValue) {
        std::lock_guard lk(_mutex);
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

    Atomic<Milliseconds> _maxSampleWaitMS;
    size_t _minThreads;
    size_t _maxThreads;

    std::mutex _mutex;
    std::unique_ptr<ThreadPool> _pool;
};

class AsyncFTDCCollectorCollection : public FTDCCollectorCollection {
public:
    AsyncFTDCCollectorCollection(Milliseconds maxSampleWait, size_t minThreads, size_t maxThreads)
        : _collectorCache(std::make_unique<SampleCollectorCache>(
              std::move(maxSampleWait), minThreads, maxThreads)) {}

    void add(std::unique_ptr<FTDCCollectorInterface> collector) override;

    bool empty() override {
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
    void _collect(OperationContext* opCtx,
                  BSONObjBuilder* builder,
                  std::vector<std::pair<std::string, int>>& sectionsSize) override;

    std::vector<std::unique_ptr<FTDCCollectorInterface>> _collectors;

    // This must be declared after _collectors so that it is destructed first.
    std::unique_ptr<SampleCollectorCache> _collectorCache;
};

class SyncFTDCCollectorCollection : public FTDCCollectorCollection {
    SyncFTDCCollectorCollection(const SyncFTDCCollectorCollection&) = delete;
    SyncFTDCCollectorCollection& operator=(const SyncFTDCCollectorCollection&) = delete;

public:
    SyncFTDCCollectorCollection() = default;

    void add(std::unique_ptr<FTDCCollectorInterface> collector) override;

    bool empty() override {
        return _collectors.empty();
    }

private:
    void _collect(OperationContext* opCtx,
                  BSONObjBuilder* builder,
                  std::vector<std::pair<std::string, int>>& sectionsSize) override;

private:
    // collection of collectors
    std::vector<std::unique_ptr<FTDCCollectorInterface>> _collectors;
    stdx::unordered_set<std::string> _collectorNames;
};

}  // namespace mongo
