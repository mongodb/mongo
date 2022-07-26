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

    enum class DonorStateEnum : int32_t {
        kUnused = -1,
        kPreparingToDonate,
        kDonatingInitialData,
        kDonatingOplogEntries,
        kPreparingToBlockWrites,
        kError,
        kBlockingWrites,
        kDone,
        kNumStates
    };

    enum class RecipientStateEnum : int32_t {
        kUnused = -1,
        kAwaitingFetchTimestamp,
        kCreatingCollection,
        kCloning,
        kApplying,
        kError,
        kStrictConsistency,
        kDone,
        kNumStates
    };

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

    ShardingDataTransformCumulativeMetrics(const std::string& rootSectionName);
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

    /**
     * The before can be boost::none to represent the initial state transition and
     * after can be boost::none to represent cases where it is no longer active.
     */
    template <typename T>
    void onStateTransition(boost::optional<T> before, boost::optional<T> after);

    void onReadDuringCriticalSection();
    void onWriteDuringCriticalSection();
    void onWriteToStashedCollections();

    void onInsertApplied();
    void onUpdateApplied();
    void onDeleteApplied();
    void onOplogEntriesFetched(int64_t numEntries, Milliseconds elapsed);
    void onOplogEntriesApplied(int64_t numEntries);
    void onCloningTotalRemoteBatchRetrieval(Milliseconds elapsed);
    void onOplogLocalBatchApplied(Milliseconds elapsed);

    static const char* fieldNameFor(CoordinatorStateEnum state);
    static const char* fieldNameFor(DonorStateEnum state);
    static const char* fieldNameFor(RecipientStateEnum state);

    void onInsertsDuringCloning(int64_t count, int64_t bytes, const Milliseconds& elapsedTime);
    void onLocalInsertDuringOplogFetching(const Milliseconds& elapsedTime);
    void onBatchRetrievedDuringOplogApplying(const Milliseconds& elapsedTime);

private:
    using CoordinatorStateArray =
        std::array<AtomicWord<int64_t>, static_cast<size_t>(CoordinatorStateEnum::kNumStates)>;
    using DonorStateArray =
        std::array<AtomicWord<int64_t>, static_cast<size_t>(DonorStateEnum::kNumStates)>;
    using RecipientStateArray =
        std::array<AtomicWord<int64_t>, static_cast<size_t>(RecipientStateEnum::kNumStates)>;

    void reportActive(BSONObjBuilder* bob) const;
    void reportOldestActive(BSONObjBuilder* bob) const;
    void reportLatencies(BSONObjBuilder* bob) const;
    void reportCurrentInSteps(BSONObjBuilder* bob) const;

    MetricsSet& getMetricsSetForRole(Role role);
    const MetricsSet& getMetricsSetForRole(Role role) const;
    const InstanceObserver* getOldestOperation(WithLock, Role role) const;

    template <typename T>
    const AtomicWord<int64_t>* getStateCounter(T state) const;
    template <typename T>
    AtomicWord<int64_t>* getMutableStateCounter(T state);

    CoordinatorStateArray* getStateArrayFor(CoordinatorStateEnum state);
    const CoordinatorStateArray* getStateArrayFor(CoordinatorStateEnum state) const;
    DonorStateArray* getStateArrayFor(DonorStateEnum state);
    const DonorStateArray* getStateArrayFor(DonorStateEnum state) const;
    RecipientStateArray* getStateArrayFor(RecipientStateEnum state);
    const RecipientStateArray* getStateArrayFor(RecipientStateEnum state) const;

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

    AtomicWord<int64_t> _insertsApplied{0};
    AtomicWord<int64_t> _updatesApplied{0};
    AtomicWord<int64_t> _deletesApplied{0};
    AtomicWord<int64_t> _oplogEntriesApplied{0};
    AtomicWord<int64_t> _oplogEntriesFetched{0};

    AtomicWord<int64_t> _totalBatchRetrievedDuringClone{0};
    AtomicWord<int64_t> _totalBatchRetrievedDuringCloneMillis{0};
    AtomicWord<int64_t> _oplogBatchApplied{0};
    AtomicWord<int64_t> _oplogBatchAppliedMillis{0};
    AtomicWord<int64_t> _documentsProcessed{0};
    AtomicWord<int64_t> _bytesWritten{0};

    AtomicWord<int64_t> _lastOpEndingChunkImbalance{0};
    AtomicWord<int64_t> _readsDuringCriticalSection{0};
    AtomicWord<int64_t> _writesDuringCriticalSection{0};

    CoordinatorStateArray _coordinatorStateList;
    DonorStateArray _donorStateList;
    RecipientStateArray _recipientStateList;

    AtomicWord<int64_t> _collectionCloningTotalLocalBatchInserts{0};
    AtomicWord<int64_t> _collectionCloningTotalLocalInsertTimeMillis{0};
    AtomicWord<int64_t> _oplogFetchingTotalRemoteBatchesRetrieved{0};
    AtomicWord<int64_t> _oplogFetchingTotalRemoteBatchesRetrievalTimeMillis{0};
    AtomicWord<int64_t> _oplogFetchingTotalLocalInserts{0};
    AtomicWord<int64_t> _oplogFetchingTotalLocalInsertTimeMillis{0};
    AtomicWord<int64_t> _oplogApplyingTotalBatchesRetrieved{0};
    AtomicWord<int64_t> _oplogApplyingTotalBatchesRetrievalTimeMillis{0};
    AtomicWord<int64_t> _writesToStashedCollections{0};
};

template <typename T>
void ShardingDataTransformCumulativeMetrics::onStateTransition(boost::optional<T> before,
                                                               boost::optional<T> after) {
    if (before) {
        if (auto counter = getMutableStateCounter(*before)) {
            counter->fetchAndSubtract(1);
        }
    }

    if (after) {
        if (auto counter = getMutableStateCounter(*after)) {
            counter->fetchAndAdd(1);
        }
    }
}

template <typename T>
const AtomicWord<int64_t>* ShardingDataTransformCumulativeMetrics::getStateCounter(T state) const {
    if (state == T::kUnused) {
        return nullptr;
    }

    invariant(static_cast<size_t>(state) < static_cast<size_t>(T::kNumStates));
    return &((*getStateArrayFor(state))[static_cast<size_t>(state)]);
}

template <typename T>
AtomicWord<int64_t>* ShardingDataTransformCumulativeMetrics::getMutableStateCounter(T state) {
    if (state == T::kUnused) {
        return nullptr;
    }

    invariant(static_cast<size_t>(state) < static_cast<size_t>(T::kNumStates));
    return &((*getStateArrayFor(state))[static_cast<size_t>(state)]);
}

}  // namespace mongo
