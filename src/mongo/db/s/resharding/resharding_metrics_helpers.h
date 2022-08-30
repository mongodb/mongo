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

#include "mongo/db/s/resharding/coordinator_document_gen.h"
#include "mongo/db/s/resharding/donor_document_gen.h"
#include "mongo/db/s/resharding/recipient_document_gen.h"
#include "mongo/db/s/sharding_data_transform_instance_metrics.h"
#include <type_traits>

namespace mongo {

namespace resharding_metrics {

template <class T>
inline constexpr bool isStateDocument =
    std::disjunction_v<std::is_same<T, ReshardingRecipientDocument>,
                       std::is_same<T, ReshardingCoordinatorDocument>,
                       std::is_same<T, ReshardingDonorDocument>>;

template <typename T>
inline constexpr bool isState = std::disjunction_v<std::is_same<T, RecipientStateEnum>,
                                                   std::is_same<T, CoordinatorStateEnum>,
                                                   std::is_same<T, DonorStateEnum>>;

template <typename T>
inline constexpr auto getState(const T& document) {
    static_assert(isStateDocument<T>);
    if constexpr (std::is_same_v<T, ReshardingCoordinatorDocument>) {
        return document.getState();
    } else {
        return document.getMutableState().getState();
    }
}

template <typename T>
inline constexpr ShardingDataTransformMetrics::Role getRoleForStateDocument() {
    static_assert(isStateDocument<T>);
    using Role = ShardingDataTransformMetrics::Role;
    if constexpr (std::is_same_v<T, ReshardingCoordinatorDocument>) {
        return Role::kCoordinator;
    } else if constexpr (std::is_same_v<T, ReshardingDonorDocument>) {
        return Role::kDonor;
    } else if constexpr (std::is_same_v<T, ReshardingRecipientDocument>) {
        return Role::kRecipient;
    }
    MONGO_UNREACHABLE;
}

void onCriticalSectionError(OperationContext* opCtx, const StaleConfigInfo& info) noexcept;

template <typename T>
std::string getMetricsPrefix() {
    static_assert(isStateDocument<T>);
    return T::kMetricsFieldName + ".";
}

template <typename T>
std::string getIntervalPrefix(const StringData& intervalFieldName) {
    return getMetricsPrefix<T>() + intervalFieldName + ".";
}

template <typename T>
std::string getIntervalStartFieldName(const StringData& intervalFieldName) {
    return getIntervalPrefix<T>(intervalFieldName) + ReshardingMetricsTimeInterval::kStartFieldName;
}

template <typename T>
std::string getIntervalEndFieldName(const StringData& intervalFieldName) {
    return getIntervalPrefix<T>(intervalFieldName) + ReshardingMetricsTimeInterval::kStopFieldName;
}

}  // namespace resharding_metrics

}  // namespace mongo
