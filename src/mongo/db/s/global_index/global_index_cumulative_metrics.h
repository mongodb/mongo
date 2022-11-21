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

#include "mongo/db/s/cumulative_metrics_state_holder.h"
#include "mongo/db/s/global_index/global_index_cumulative_metrics_field_name_provider.h"
#include "mongo/db/s/sharding_data_transform_cumulative_metrics.h"

namespace mongo {
namespace global_index {

class GlobalIndexCumulativeMetrics : public ShardingDataTransformCumulativeMetrics {
public:
    // TODO: Replace with actual Global Index Cumulative Metrics state enums by role
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

    enum class RecipientStateEnum : int32_t {
        kUnused = -1,
        kCloning,
        kReadyToCommit,
        kDone,
        kNumStates
    };

    GlobalIndexCumulativeMetrics();

    template <typename T>
    void onStateTransition(boost::optional<T> before, boost::optional<T> after);
    static StringData fieldNameFor(CoordinatorStateEnum state,
                                   const GlobalIndexCumulativeMetricsFieldNameProvider* provider);
    static StringData fieldNameFor(RecipientStateEnum state,
                                   const GlobalIndexCumulativeMetricsFieldNameProvider* provider);

private:
    template <typename T>
    const AtomicWord<int64_t>* getStateCounter(T state) const;
    const GlobalIndexCumulativeMetricsFieldNameProvider* _fieldNames;

    CumulativeMetricsStateHolder<CoordinatorStateEnum,
                                 static_cast<size_t>(CoordinatorStateEnum::kNumStates)>
        _coordinatorStateList;
    CumulativeMetricsStateHolder<RecipientStateEnum,
                                 static_cast<size_t>(RecipientStateEnum::kNumStates)>
        _recipientStateList;

    template <typename T>
    auto getStateListForRole() const {
        if constexpr (std::is_same<T, CoordinatorStateEnum>::value) {
            return &_coordinatorStateList;
        } else if constexpr (std::is_same<T, RecipientStateEnum>::value) {
            return &_recipientStateList;
        } else {
            MONGO_UNREACHABLE;
        }
    }

    template <typename T>
    auto getMutableStateListForRole() {
        if constexpr (std::is_same<T, CoordinatorStateEnum>::value) {
            return &_coordinatorStateList;
        } else if constexpr (std::is_same<T, RecipientStateEnum>::value) {
            return &_recipientStateList;
        } else {
            MONGO_UNREACHABLE;
        }
    }
};

template <typename T>
void GlobalIndexCumulativeMetrics::onStateTransition(boost::optional<T> before,
                                                     boost::optional<T> after) {
    getMutableStateListForRole<T>()->onStateTransition(before, after);
}

template <typename T>
const AtomicWord<int64_t>* GlobalIndexCumulativeMetrics::getStateCounter(T state) const {
    return getStateListForRole<T>()->getStateCounter(state);
}

}  // namespace global_index
}  // namespace mongo
