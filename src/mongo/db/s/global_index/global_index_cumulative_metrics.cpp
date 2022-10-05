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

#include "mongo/db/s/global_index/global_index_cumulative_metrics.h"

namespace mongo {
namespace global_index {

namespace {
constexpr auto kGlobalIndex = "globalIndex";
}

GlobalIndexCumulativeMetrics::GlobalIndexCumulativeMetrics()
    : ShardingDataTransformCumulativeMetrics(
          kGlobalIndex, std::make_unique<GlobalIndexCumulativeMetricsFieldNameProvider>()),
      _fieldNames(
          static_cast<const GlobalIndexCumulativeMetricsFieldNameProvider*>(getFieldNames())) {}

StringData GlobalIndexCumulativeMetrics::fieldNameFor(
    CoordinatorStateEnum state, const GlobalIndexCumulativeMetricsFieldNameProvider* provider) {
    switch (state) {
        case CoordinatorStateEnum::kInitializing:
            return provider->getForCountInstancesInRoleNameStateNStateName();

        case CoordinatorStateEnum::kPreparingToDonate:
            return provider->getForCountInstancesInRoleNameStateNStateName();

        case CoordinatorStateEnum::kCloning:
            return provider->getForCountInstancesInRoleNameStateNStateName();

        case CoordinatorStateEnum::kApplying:
            return provider->getForCountInstancesInRoleNameStateNStateName();

        case CoordinatorStateEnum::kBlockingWrites:
            return provider->getForCountInstancesInRoleNameStateNStateName();

        case CoordinatorStateEnum::kAborting:
            return provider->getForCountInstancesInRoleNameStateNStateName();

        case CoordinatorStateEnum::kCommitting:
            return provider->getForCountInstancesInRoleNameStateNStateName();

        default:
            uasserted(6438604,
                      str::stream()
                          << "no field name for coordinator state " << static_cast<int32_t>(state));
            break;
    }

    MONGO_UNREACHABLE;
}

StringData GlobalIndexCumulativeMetrics::fieldNameFor(
    DonorStateEnum state, const GlobalIndexCumulativeMetricsFieldNameProvider* provider) {
    switch (state) {
        case DonorStateEnum::kPreparingToDonate:
            return provider->getForCountInstancesInRoleNameStateNStateName();

        case DonorStateEnum::kDonatingInitialData:
            return provider->getForCountInstancesInRoleNameStateNStateName();

        case DonorStateEnum::kDonatingOplogEntries:
            return provider->getForCountInstancesInRoleNameStateNStateName();

        case DonorStateEnum::kPreparingToBlockWrites:
            return provider->getForCountInstancesInRoleNameStateNStateName();

        case DonorStateEnum::kError:
            return provider->getForCountInstancesInRoleNameStateNStateName();

        case DonorStateEnum::kBlockingWrites:
            return provider->getForCountInstancesInRoleNameStateNStateName();

        case DonorStateEnum::kDone:
            return provider->getForCountInstancesInRoleNameStateNStateName();

        default:
            uasserted(6438704,
                      str::stream()
                          << "no field name for donor state " << static_cast<int32_t>(state));
            break;
    }

    MONGO_UNREACHABLE;
}

StringData GlobalIndexCumulativeMetrics::fieldNameFor(
    RecipientStateEnum state, const GlobalIndexCumulativeMetricsFieldNameProvider* provider) {
    switch (state) {
        case RecipientStateEnum::kAwaitingFetchTimestamp:
            return provider->getForCountInstancesInRoleNameStateNStateName();

        case RecipientStateEnum::kCreatingCollection:
            return provider->getForCountInstancesInRoleNameStateNStateName();

        case RecipientStateEnum::kCloning:
            return provider->getForCountInstancesInRoleNameStateNStateName();

        case RecipientStateEnum::kApplying:
            return provider->getForCountInstancesInRoleNameStateNStateName();

        case RecipientStateEnum::kError:
            return provider->getForCountInstancesInRoleNameStateNStateName();

        case RecipientStateEnum::kStrictConsistency:
            return provider->getForCountInstancesInRoleNameStateNStateName();

        case RecipientStateEnum::kDone:
            return provider->getForCountInstancesInRoleNameStateNStateName();

        default:
            uasserted(6438904,
                      str::stream()
                          << "no field name for recipient state " << static_cast<int32_t>(state));
            break;
    }

    MONGO_UNREACHABLE;
}

}  // namespace global_index
}  // namespace mongo
