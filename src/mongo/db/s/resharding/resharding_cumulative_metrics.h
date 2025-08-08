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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/s/metrics/cumulative_metrics_state_tracker.h"
#include "mongo/db/s/resharding/resharding_metrics_common.h"
#include "mongo/db/s/resharding/resharding_metrics_observer.h"
#include "mongo/db/service_context.h"
#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/util/functional.h"

#include <mutex>

#include <boost/optional/optional.hpp>

namespace mongo {

class ReshardingCumulativeMetrics {
public:
    using Role = ReshardingMetricsCommon::Role;
    using StateTracker =
        CumulativeMetricsStateTracker<CoordinatorStateEnum, DonorStateEnum, RecipientStateEnum>;
    using AnyState = StateTracker::AnyState;

    struct MetricsComparer {
        inline bool operator()(const ReshardingMetricsObserver* a,
                               const ReshardingMetricsObserver* b) const {
            auto aTime = a->getStartTimestamp();
            auto bTime = b->getStartTimestamp();
            if (aTime == bTime) {
                return a->getUuid() < b->getUuid();
            }
            return aTime < bTime;
        }
    };
    using MetricsSet = std::set<const ReshardingMetricsObserver*, MetricsComparer>;

    /**
     * RAII type that takes care of deregistering the observer once it goes out of scope.
     */
    class ScopedObserver {
    public:
        ScopedObserver(ReshardingCumulativeMetrics* metrics,
                       Role role,
                       MetricsSet::iterator observerIterator);
        ScopedObserver(const ScopedObserver&) = delete;
        ScopedObserver& operator=(const ScopedObserver&) = delete;

        ~ScopedObserver();

    private:
        ReshardingCumulativeMetrics* const _metrics;
        const Role _role;
        const MetricsSet::iterator _observerIterator;
    };

    using UniqueScopedObserver = std::unique_ptr<ScopedObserver>;
    friend ScopedObserver;

    static ReshardingCumulativeMetrics* getForResharding(ServiceContext* context);
    static ReshardingCumulativeMetrics* getForMoveCollection(ServiceContext* context);
    static ReshardingCumulativeMetrics* getForBalancerMoveCollection(ServiceContext* context);
    static ReshardingCumulativeMetrics* getForUnshardCollection(ServiceContext* context);

    ReshardingCumulativeMetrics();
    ReshardingCumulativeMetrics(const std::string& rootName);

    [[nodiscard]] UniqueScopedObserver registerInstanceMetrics(
        const ReshardingMetricsObserver* metrics);
    int64_t getOldestOperationHighEstimateRemainingTimeMillis(Role role) const;
    int64_t getOldestOperationLowEstimateRemainingTimeMillis(Role role) const;
    size_t getObservedMetricsCount() const;
    size_t getObservedMetricsCount(Role role) const;

    void onStarted();
    void onSuccess();
    void onFailure();
    void onCanceled();

    void setLastOpEndingChunkImbalance(int64_t imbalanceCount);

    void onReadDuringCriticalSection();
    void onWriteDuringCriticalSection();
    void onWriteToStashedCollections();

    void onCloningRemoteBatchRetrieval(Milliseconds elapsed);
    void onInsertsDuringCloning(int64_t count, int64_t bytes, const Milliseconds& elapsedTime);

    void onInsertApplied();
    void onUpdateApplied();
    void onDeleteApplied();
    void onOplogEntriesFetched(int64_t numEntries);
    void onOplogEntriesApplied(int64_t numEntries);

    void onBatchRetrievedDuringOplogFetching(Milliseconds elapsed);
    void onLocalInsertDuringOplogFetching(const Milliseconds& elapsedTime);
    void onBatchRetrievedDuringOplogApplying(const Milliseconds& elapsedTime);
    void onOplogLocalBatchApplied(Milliseconds elapsed);

    template <typename T>
    void onStateTransition(boost::optional<T> before, boost::optional<T> after) {
        _stateTracker.onStateTransition(before, after);
    }

    static boost::optional<StringData> fieldNameFor(AnyState state);
    void reportForServerStatus(BSONObjBuilder* bob) const;

    void onStarted(bool isSameKeyResharding, const UUID& reshardingUUID);
    void onSuccess(bool isSameKeyResharding, const UUID& reshardingUUID);
    void onFailure(bool isSameKeyResharding, const UUID& reshardingUUID);
    void onCanceled(bool isSameKeyResharding, const UUID& reshardingUUID);

private:
    enum EstimateType { kHigh, kLow };

    int64_t getInsertsApplied() const;
    int64_t getUpdatesApplied() const;
    int64_t getDeletesApplied() const;
    int64_t getOplogEntriesFetched() const;
    int64_t getOplogEntriesApplied() const;

    int64_t getOplogFetchingTotalRemoteBatchesRetrieved() const;
    int64_t getOplogFetchingTotalRemoteBatchesRetrievalTimeMillis() const;
    int64_t getOplogFetchingTotalLocalInserts() const;
    int64_t getOplogFetchingTotalLocalInsertTimeMillis() const;
    int64_t getOplogApplyingTotalBatchesRetrieved() const;
    int64_t getOplogApplyingTotalBatchesRetrievalTimeMillis() const;
    int64_t getOplogBatchApplied() const;
    int64_t getOplogBatchAppliedMillis() const;

    void reportCountsForAllStates(const StateTracker::StateFieldNameMap& names,
                                  BSONObjBuilder* bob) const;

    template <typename T>
    int64_t getCountInState(T state) const {
        return _stateTracker.getCountInState(state);
    }

    MetricsSet& getMetricsSetForRole(Role role);
    const MetricsSet& getMetricsSetForRole(Role role) const;
    const ReshardingMetricsObserver* getOldestOperation(WithLock, Role role) const;
    int64_t getOldestOperationEstimateRemainingTimeMillis(Role role, EstimateType type) const;
    boost::optional<Milliseconds> getEstimate(const ReshardingMetricsObserver* op,
                                              EstimateType type) const;

    MetricsSet::iterator insertMetrics(const ReshardingMetricsObserver* metrics, MetricsSet& set);
    void deregisterMetrics(const Role& role, const MetricsSet::iterator& metrics);

    void reportActive(BSONObjBuilder* bob) const;
    void reportOldestActive(BSONObjBuilder* bob) const;
    void reportLatencies(BSONObjBuilder* bob) const;
    void reportCurrentInSteps(BSONObjBuilder* bob) const;

    const std::string _rootSectionName;
    mutable stdx::mutex _mutex;
    std::vector<MetricsSet> _instanceMetricsForAllRoles;

    StateTracker _stateTracker;

    AtomicWord<bool> _shouldReportMetrics;

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

    AtomicWord<int64_t> _insertsApplied{0};
    AtomicWord<int64_t> _updatesApplied{0};
    AtomicWord<int64_t> _deletesApplied{0};
    AtomicWord<int64_t> _oplogEntriesApplied{0};
    AtomicWord<int64_t> _oplogEntriesFetched{0};

    AtomicWord<int64_t> _oplogFetchingTotalRemoteBatchesRetrieved{0};
    AtomicWord<int64_t> _oplogFetchingTotalRemoteBatchesRetrievalTimeMillis{0};
    AtomicWord<int64_t> _oplogFetchingTotalLocalInserts{0};
    AtomicWord<int64_t> _oplogFetchingTotalLocalInsertTimeMillis{0};
    AtomicWord<int64_t> _oplogApplyingTotalBatchesRetrieved{0};
    AtomicWord<int64_t> _oplogApplyingTotalBatchesRetrievalTimeMillis{0};
    AtomicWord<int64_t> _oplogBatchApplied{0};
    AtomicWord<int64_t> _oplogBatchAppliedMillis{0};

    AtomicWord<int64_t> _countSameKeyStarted{0};
    AtomicWord<int64_t> _countSameKeySucceeded{0};
    AtomicWord<int64_t> _countSameKeyFailed{0};
    AtomicWord<int64_t> _countSameKeyCancelled{0};

    std::set<UUID> _activeReshardingOperations;
    std::mutex _activeReshardingOperationsMutex;
};

}  // namespace mongo
