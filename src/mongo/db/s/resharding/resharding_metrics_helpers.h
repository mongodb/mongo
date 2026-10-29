// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/s/resharding/coordinator_document_gen.h"
#include "mongo/db/s/resharding/donor_document_gen.h"
#include "mongo/db/s/resharding/recipient_document_gen.h"
#include "mongo/db/s/resharding/resharding_metrics_common.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <string>
#include <string_view>
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
inline constexpr ReshardingMetricsCommon::Role getRoleForStateDocument() {
    static_assert(isStateDocument<T>);
    using Role = ReshardingMetricsCommon::Role;
    if constexpr (std::is_same_v<T, ReshardingCoordinatorDocument>) {
        return Role::kCoordinator;
    } else if constexpr (std::is_same_v<T, ReshardingDonorDocument>) {
        return Role::kDonor;
    } else if constexpr (std::is_same_v<T, ReshardingRecipientDocument>) {
        return Role::kRecipient;
    }
    MONGO_UNREACHABLE;
}

[[MONGO_MOD_PUBLIC]] void onCriticalSectionError(OperationContext* opCtx,
                                                 const StaleConfigInfo& info);

template <typename T>
std::string getMetricsPrefix() {
    static_assert(isStateDocument<T>);
    return std::string{T::kMetricsFieldName} + ".";
}

template <typename T>
std::string getIntervalPrefix(std::string_view intervalFieldName) {
    return getMetricsPrefix<T>() + std::string{intervalFieldName} + ".";
}

template <typename T>
std::string getIntervalStartFieldName(std::string_view intervalFieldName) {
    return getIntervalPrefix<T>(intervalFieldName) +
        std::string{ReshardingMetricsTimeInterval::kStartFieldName};
}

template <typename T>
std::string getIntervalEndFieldName(std::string_view intervalFieldName) {
    return getIntervalPrefix<T>(intervalFieldName) +
        std::string{ReshardingMetricsTimeInterval::kStopFieldName};
}

}  // namespace resharding_metrics

}  // namespace mongo
