/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/s/sharding_data_transform_cumulative_metrics_field_name_provider.h"
#include "mongo/db/s/sharding_data_transform_metrics_observer_interface.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/mutex.h"
#include "mongo/util/duration.h"
#include "mongo/util/functional.h"
#include <set>

namespace mongo {

class ShardingDataTransformCumulativeMetrics {
public:
    using NameProvider = ShardingDataTransformCumulativeMetricsFieldNameProvider;
    using Role = ShardingDataTransformMetrics::Role;
    using InstanceObserver = ShardingDataTransformMetricsObserverInterface;
    using DeregistrationFunction = unique_function<void()>;

    struct MetricsComparer {
        inline bool operator()(const InstanceObserver* a, const InstanceObserver* b) const {
            auto aTime = a->getStartTimestamp();
            auto bTime = b->getStartTimestamp();
            if (aTime == bTime) {
                return a->getUuid() < b->getUuid();
            }
            return aTime < bTime;
        }
    };
    using MetricsSet = std::set<const InstanceObserver*, MetricsComparer>;

    /**
     * RAII type that takes care of deregistering the observer once it goes out of scope.
     */
    class ScopedObserver {
    public:
        ScopedObserver(ShardingDataTransformCumulativeMetrics* metrics,
                       Role role,
                       MetricsSet::iterator observerIterator);
        ScopedObserver(const ScopedObserver&) = delete;
        ScopedObserver& operator=(const ScopedObserver&) = delete;

        ~ScopedObserver();

    private:
        ShardingDataTransformCumulativeMetrics* const _metrics;
        const Role _role;
        const MetricsSet::iterator _observerIterator;
    };

    using UniqueScopedObserver = std::unique_ptr<ScopedObserver>;
    friend ScopedObserver;

    static ShardingDataTransformCumulativeMetrics* getForResharding(ServiceContext* context);
    static ShardingDataTransformCumulativeMetrics* getForGlobalIndexes(ServiceContext* context);


    ShardingDataTransformCumulativeMetrics(const std::string& rootSectionName,
                                           std::unique_ptr<NameProvider> fieldNameProvider);
    virtual ~ShardingDataTransformCumulativeMetrics() = default;
    [[nodiscard]] UniqueScopedObserver registerInstanceMetrics(const InstanceObserver* metrics);
    int64_t getOldestOperationHighEstimateRemainingTimeMillis(Role role) const;
    int64_t getOldestOperationLowEstimateRemainingTimeMillis(Role role) const;
    size_t getObservedMetricsCount() const;
    size_t getObservedMetricsCount(Role role) const;
    void reportForServerStatus(BSONObjBuilder* bob) const;

    void onStarted();
    void onSuccess();
    void onFailure();
    void onCanceled();

    void setLastOpEndingChunkImbalance(int64_t imbalanceCount);

    void onReadDuringCriticalSection();
    void onWriteDuringCriticalSection();
    void onWriteToStashedCollections();

    void onCloningTotalRemoteBatchRetrieval(Milliseconds elapsed);
    void onInsertsDuringCloning(int64_t count, int64_t bytes, const Milliseconds& elapsedTime);

protected:
    const ShardingDataTransformCumulativeMetricsFieldNameProvider* getFieldNames() const;

    virtual void reportActive(BSONObjBuilder* bob) const;
    virtual void reportOldestActive(BSONObjBuilder* bob) const;
    virtual void reportLatencies(BSONObjBuilder* bob) const;
    virtual void reportCurrentInSteps(BSONObjBuilder* bob) const;

private:
    enum EstimateType { kHigh, kLow };

    MetricsSet& getMetricsSetForRole(Role role);
    const MetricsSet& getMetricsSetForRole(Role role) const;
    const InstanceObserver* getOldestOperation(WithLock, Role role) const;
    int64_t getOldestOperationEstimateRemainingTimeMillis(Role role, EstimateType type) const;
    boost::optional<Milliseconds> getEstimate(const InstanceObserver* op, EstimateType type) const;

    MetricsSet::iterator insertMetrics(const InstanceObserver* metrics, MetricsSet& set);
    void deregisterMetrics(const Role& role, const MetricsSet::iterator& metrics);

    mutable Mutex _mutex;
    const std::string _rootSectionName;
    std::unique_ptr<NameProvider> _fieldNames;
    std::vector<MetricsSet> _instanceMetricsForAllRoles;
    AtomicWord<bool> _operationWasAttempted;

    AtomicWord<int64_t> _countStarted{0};
    AtomicWord<int64_t> _countSucceeded{0};
    AtomicWord<int64_t> _countFailed{0};
    AtomicWord<int64_t> _countCancelled{0};

    AtomicWord<int64_t> _totalBatchRetrievedDuringClone{0};
    AtomicWord<int64_t> _totalBatchRetrievedDuringCloneMillis{0};
    AtomicWord<int64_t> _documentsProcessed{0};
    AtomicWord<int64_t> _bytesWritten{0};

    AtomicWord<int64_t> _lastOpEndingChunkImbalance{0};
    AtomicWord<int64_t> _readsDuringCriticalSection{0};
    AtomicWord<int64_t> _writesDuringCriticalSection{0};

    AtomicWord<int64_t> _collectionCloningTotalLocalBatchInserts{0};
    AtomicWord<int64_t> _collectionCloningTotalLocalInsertTimeMillis{0};
    AtomicWord<int64_t> _writesToStashedCollections{0};
};

}  // namespace mongo
