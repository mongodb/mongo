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
#include "mongo/db/s/global_index_metrics.h"
#include "mongo/db/exec/document_value/document.h"


namespace mongo {
namespace {

inline GlobalIndexMetrics::State getDefaultState(GlobalIndexMetrics::Role role) {
    using Role = GlobalIndexMetrics::Role;
    switch (role) {
        case Role::kCoordinator:
            return GlobalIndexMetrics::GlobalIndexCoordinatorStateEnumPlaceholder::kUnused;
        case Role::kRecipient:
            return GlobalIndexMetrics::GlobalIndexRecipientStateEnumPlaceholder::kUnused;
        case Role::kDonor:
            return GlobalIndexMetrics::GlobalIndexDonorStateEnumPlaceholder::kUnused;
    }
    MONGO_UNREACHABLE;
}

// Returns the originalCommand with the createIndexes, key and unique fields added.
BSONObj createOriginalCommand(const NamespaceString& nss, BSONObj keyPattern, bool unique) {

    using Doc = Document;
    using Arr = std::vector<Value>;
    using V = Value;

    return Doc{{"originatingCommand",
                V{Doc{{"createIndexes", V{StringData{nss.toString()}}},
                      {"key", std::move(keyPattern)},
                      {"unique", V{unique}}}}}}
        .toBson();
}
}  // namespace

GlobalIndexMetrics::DonorState::DonorState(GlobalIndexDonorStateEnumPlaceholder enumVal)
    : _enumVal(enumVal) {}

GlobalIndexCumulativeMetrics::DonorStateEnum GlobalIndexMetrics::DonorState::toMetrics() const {
    using MetricsEnum = GlobalIndexCumulativeMetrics::DonorStateEnum;

    switch (_enumVal) {
        case GlobalIndexDonorStateEnumPlaceholder::kUnused:
            return MetricsEnum::kUnused;

        case GlobalIndexDonorStateEnumPlaceholder::kPreparingToDonate:
            return MetricsEnum::kPreparingToDonate;

        case GlobalIndexDonorStateEnumPlaceholder::kDonatingInitialData:
            return MetricsEnum::kDonatingInitialData;

        case GlobalIndexDonorStateEnumPlaceholder::kDonatingOplogEntries:
            return MetricsEnum::kDonatingOplogEntries;

        case GlobalIndexDonorStateEnumPlaceholder::kPreparingToBlockWrites:
            return MetricsEnum::kPreparingToBlockWrites;

        case GlobalIndexDonorStateEnumPlaceholder::kError:
            return MetricsEnum::kError;

        case GlobalIndexDonorStateEnumPlaceholder::kBlockingWrites:
            return MetricsEnum::kBlockingWrites;

        case GlobalIndexDonorStateEnumPlaceholder::kDone:
            return MetricsEnum::kDone;
        default:
            invariant(false, str::stream() << "Unexpected Global Index coordinator state: ");
            MONGO_UNREACHABLE;
    }
}

GlobalIndexMetrics::GlobalIndexDonorStateEnumPlaceholder GlobalIndexMetrics::DonorState::getState()
    const {
    return _enumVal;
}

GlobalIndexMetrics::RecipientState::RecipientState(GlobalIndexRecipientStateEnumPlaceholder enumVal)
    : _enumVal(enumVal) {}

GlobalIndexCumulativeMetrics::RecipientStateEnum GlobalIndexMetrics::RecipientState::toMetrics()
    const {
    using MetricsEnum = GlobalIndexCumulativeMetrics::RecipientStateEnum;

    switch (_enumVal) {
        case GlobalIndexRecipientStateEnumPlaceholder::kUnused:
            return MetricsEnum::kUnused;

        case GlobalIndexRecipientStateEnumPlaceholder::kAwaitingFetchTimestamp:
            return MetricsEnum::kAwaitingFetchTimestamp;

        case GlobalIndexRecipientStateEnumPlaceholder::kCreatingCollection:
            return MetricsEnum::kCreatingCollection;

        case GlobalIndexRecipientStateEnumPlaceholder::kCloning:
            return MetricsEnum::kCloning;

        case GlobalIndexRecipientStateEnumPlaceholder::kApplying:
            return MetricsEnum::kApplying;

        case GlobalIndexRecipientStateEnumPlaceholder::kError:
            return MetricsEnum::kError;

        case GlobalIndexRecipientStateEnumPlaceholder::kStrictConsistency:
            return MetricsEnum::kStrictConsistency;

        case GlobalIndexRecipientStateEnumPlaceholder::kDone:
            return MetricsEnum::kDone;

        default:
            invariant(false, str::stream() << "Unexpected Global Index coordinator state: ");
            MONGO_UNREACHABLE;
    }
}

GlobalIndexMetrics::GlobalIndexRecipientStateEnumPlaceholder
GlobalIndexMetrics::RecipientState::getState() const {
    return _enumVal;
}

GlobalIndexMetrics::CoordinatorState::CoordinatorState(
    GlobalIndexCoordinatorStateEnumPlaceholder enumVal)
    : _enumVal(enumVal) {}

GlobalIndexCumulativeMetrics::CoordinatorStateEnum GlobalIndexMetrics::CoordinatorState::toMetrics()
    const {
    using MetricsEnum = GlobalIndexCumulativeMetrics::CoordinatorStateEnum;

    switch (_enumVal) {
        case GlobalIndexCoordinatorStateEnumPlaceholder::kUnused:
            return MetricsEnum::kUnused;

        case GlobalIndexCoordinatorStateEnumPlaceholder::kInitializing:
            return MetricsEnum::kInitializing;

        case GlobalIndexCoordinatorStateEnumPlaceholder::kPreparingToDonate:
            return MetricsEnum::kPreparingToDonate;

        case GlobalIndexCoordinatorStateEnumPlaceholder::kCloning:
            return MetricsEnum::kCloning;

        case GlobalIndexCoordinatorStateEnumPlaceholder::kApplying:
            return MetricsEnum::kApplying;

        case GlobalIndexCoordinatorStateEnumPlaceholder::kBlockingWrites:
            return MetricsEnum::kBlockingWrites;

        case GlobalIndexCoordinatorStateEnumPlaceholder::kAborting:
            return MetricsEnum::kAborting;

        case GlobalIndexCoordinatorStateEnumPlaceholder::kCommitting:
            return MetricsEnum::kCommitting;

        case GlobalIndexCoordinatorStateEnumPlaceholder::kDone:
            return MetricsEnum::kDone;
        default:
            invariant(false, str::stream() << "Unexpected Global Index coordinator state: ");
            MONGO_UNREACHABLE;
    }
}

GlobalIndexMetrics::GlobalIndexCoordinatorStateEnumPlaceholder
GlobalIndexMetrics::CoordinatorState::getState() const {
    return _enumVal;
}


GlobalIndexMetrics::GlobalIndexMetrics(UUID instanceId,
                                       BSONObj originatingCommand,
                                       NamespaceString nss,
                                       Role role,
                                       Date_t startTime,
                                       ClockSource* clockSource,
                                       ShardingDataTransformCumulativeMetrics* cumulativeMetrics)
    : ShardingDataTransformInstanceMetrics{std::move(instanceId),
                                           std::move(originatingCommand),
                                           std::move(nss),
                                           role,
                                           startTime,
                                           clockSource,
                                           cumulativeMetrics,
                                           std::make_unique<GlobalIndexMetricsFieldNameProvider>()},
      _stateHolder{getGlobalIndexCumulativeMetrics(), getDefaultState(role)},
      _scopedObserver(registerInstanceMetrics()),
      _globalIndexFieldNames{static_cast<GlobalIndexMetricsFieldNameProvider*>(_fieldNames.get())} {
}

GlobalIndexMetrics::~GlobalIndexMetrics() {
    // Deregister the observer first to ensure that the observer will no longer be able to reach
    // this object while destructor is running.
    _scopedObserver.reset();
}

std::string GlobalIndexMetrics::createOperationDescription() const noexcept {
    return fmt::format("GlobalIndexMetrics{}Service {}",
                       ShardingDataTransformMetrics::getRoleName(_role),
                       _instanceId.toString());
}

GlobalIndexCumulativeMetrics* GlobalIndexMetrics::getGlobalIndexCumulativeMetrics() {
    return dynamic_cast<GlobalIndexCumulativeMetrics*>(getCumulativeMetrics());
}


boost::optional<Milliseconds> GlobalIndexMetrics::getRecipientHighEstimateRemainingTimeMillis()
    const {
    return boost::none;
}

}  // namespace mongo
