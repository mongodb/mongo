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
#include "mongo/db/s/sharding_data_transform_metrics_observer_interface.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/mutex.h"
#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/util/duration.h"
#include "mongo/util/functional.h"
#include <set>

namespace mongo {

class ShardingDataTransformCumulativeMetrics {
public:
    enum class CoordinatorStateEnum : int32_t {
        kUnused = -1,
        kInitializing,
        kPreparingToDonate,
        kCloning,
        kApplying,
        kBlockingWrites,
        kAborting,
        kCommitting,
        kDone,
        kNumStates
    };

    using Role = ShardingDataTransformMetrics::Role;
    using InstanceObserver = ShardingDataTransformMetricsObserverInterface;
    using DeregistrationFunction = unique_function<void()>;


    static ShardingDataTransformCumulativeMetrics* getForResharding(ServiceContext* context);
    static ShardingDataTransformCumulativeMetrics* getForGlobalIndexes(ServiceContext* context);

    ShardingDataTransformCumulativeMetrics(const std::string& rootSectionName);
    [[nodiscard]] DeregistrationFunction registerInstanceMetrics(const InstanceObserver* metrics);
    int64_t getOldestOperationHighEstimateRemainingTimeMillis(Role role) const;
    int64_t getOldestOperationLowEstimateRemainingTimeMillis(Role role) const;
    size_t getObservedMetricsCount() const;
    size_t getObservedMetricsCount(Role role) const;
    void reportForServerStatus(BSONObjBuilder* bob) const;

    void onStarted();
    void onCompletion(ReshardingOperationStatusEnum status);

    void setLastOpEndingChunkImbalance(int64_t imbalanceCount);

    /**
     * The before can be boost::none to represent the initial state transition and
     * after can be boost::none to represent cases where it is no longer active.
     */
    void onCoordinatorStateTransition(boost::optional<CoordinatorStateEnum> before,
                                      boost::optional<CoordinatorStateEnum> after);

    static const char* fieldNameFor(CoordinatorStateEnum state);
    void onInsertsDuringCloning(int64_t count, const Milliseconds& elapsedTime);
    void onRemoteBatchRetrievedDuringOplogFetching(int64_t count, const Milliseconds& elapsedTime);
    void onLocalInsertDuringOplogFetching(const Milliseconds& elapsedTime);
    void onBatchRetrievedDuringOplogApplying(int64_t count, const Milliseconds& elapsedTime);

private:
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

    void reportActive(BSONObjBuilder* bob) const;
    void reportOldestActive(BSONObjBuilder* bob) const;
    void reportLatencies(BSONObjBuilder* bob) const;
    void reportCurrentInSteps(BSONObjBuilder* bob) const;

    MetricsSet& getMetricsSetForRole(Role role);
    const MetricsSet& getMetricsSetForRole(Role role) const;
    const InstanceObserver* getOldestOperation(WithLock, Role role) const;
    const AtomicWord<int64_t>* getCoordinatorStateCounter(CoordinatorStateEnum state) const;
    AtomicWord<int64_t>* getMutableCoordinatorStateCounter(CoordinatorStateEnum state);

    MetricsSet::iterator insertMetrics(const InstanceObserver* metrics, MetricsSet& set);

    mutable Mutex _mutex;
    const std::string _rootSectionName;
    std::vector<MetricsSet> _instanceMetricsForAllRoles;
    AtomicWord<bool> _operationWasAttempted;

    AtomicWord<int64_t> _countStarted{0};
    AtomicWord<int64_t> _countSucceeded{0};
    AtomicWord<int64_t> _countFailed{0};
    AtomicWord<int64_t> _countCancelled{0};

    AtomicWord<int64_t> _lastOpEndingChunkImbalance{0};

    std::array<AtomicWord<int64_t>, static_cast<size_t>(CoordinatorStateEnum::kNumStates)>
        _coordinatorStateList;

    AtomicWord<int64_t> _collectionCloningTotalLocalInserts{0};
    AtomicWord<int64_t> _collectionCloningTotalLocalInsertTimeMillis{0};
    AtomicWord<int64_t> _oplogFetchingTotalRemoteBatchesRetrieved{0};
    AtomicWord<int64_t> _oplogFetchingTotalRemoteBatchesRetrievalTimeMillis{0};
    AtomicWord<int64_t> _oplogFetchingTotalLocalInserts{0};
    AtomicWord<int64_t> _oplogFetchingTotalLocalInsertTimeMillis{0};
    AtomicWord<int64_t> _oplogApplyingTotalBatchesRetrieved{0};
    AtomicWord<int64_t> _oplogApplyingTotalBatchesRetrievalTimeMillis{0};
};

}  // namespace mongo
