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

#include "mongo/db/s/resharding/resharding_cumulative_metrics_field_name_provider.h"
#include "mongo/db/s/sharding_data_transform_cumulative_metrics.h"

namespace mongo {

class ReshardingCumulativeMetrics : public ShardingDataTransformCumulativeMetrics {
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

    ReshardingCumulativeMetrics();

    static StringData fieldNameFor(CoordinatorStateEnum state,
                                   const ReshardingCumulativeMetricsFieldNameProvider* provider);
    static StringData fieldNameFor(DonorStateEnum state,
                                   const ReshardingCumulativeMetricsFieldNameProvider* provider);
    static StringData fieldNameFor(RecipientStateEnum state,
                                   const ReshardingCumulativeMetricsFieldNameProvider* provider);

    /**
     * The before can be boost::none to represent the initial state transition and
     * after can be boost::none to represent cases where it is no longer active.
     */
    template <typename T>
    void onStateTransition(boost::optional<T> before, boost::optional<T> after);

    void onInsertApplied();
    void onUpdateApplied();
    void onDeleteApplied();

    void onOplogEntriesFetched(int64_t numEntries, Milliseconds elapsed);
    void onOplogEntriesApplied(int64_t numEntries);
    void onLocalInsertDuringOplogFetching(const Milliseconds& elapsedTime);
    void onBatchRetrievedDuringOplogApplying(const Milliseconds& elapsedTime);
    void onOplogLocalBatchApplied(Milliseconds elapsed);

private:
    using CoordinatorStateArray =
        std::array<AtomicWord<int64_t>, static_cast<size_t>(CoordinatorStateEnum::kNumStates)>;
    using DonorStateArray =
        std::array<AtomicWord<int64_t>, static_cast<size_t>(DonorStateEnum::kNumStates)>;
    using RecipientStateArray =
        std::array<AtomicWord<int64_t>, static_cast<size_t>(RecipientStateEnum::kNumStates)>;

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

    virtual void reportActive(BSONObjBuilder* bob) const;
    virtual void reportLatencies(BSONObjBuilder* bob) const;
    virtual void reportCurrentInSteps(BSONObjBuilder* bob) const;

    const ReshardingCumulativeMetricsFieldNameProvider* _fieldNames;

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

    CoordinatorStateArray _coordinatorStateList;
    DonorStateArray _donorStateList;
    RecipientStateArray _recipientStateList;
};

template <typename T>
void ReshardingCumulativeMetrics::onStateTransition(boost::optional<T> before,
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
const AtomicWord<int64_t>* ReshardingCumulativeMetrics::getStateCounter(T state) const {
    if (state == T::kUnused) {
        return nullptr;
    }

    invariant(static_cast<size_t>(state) < static_cast<size_t>(T::kNumStates));
    return &((*getStateArrayFor(state))[static_cast<size_t>(state)]);
}

template <typename T>
AtomicWord<int64_t>* ReshardingCumulativeMetrics::getMutableStateCounter(T state) {
    if (state == T::kUnused) {
        return nullptr;
    }

    invariant(static_cast<size_t>(state) < static_cast<size_t>(T::kNumStates));
    return &((*getStateArrayFor(state))[static_cast<size_t>(state)]);
}

}  // namespace mongo
