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

#include "mongo/db/s/resharding/resharding_cumulative_metrics.h"

namespace mongo {

namespace {
constexpr auto kResharding = "resharding";
}

ReshardingCumulativeMetrics::ReshardingCumulativeMetrics()
    : ShardingDataTransformCumulativeMetrics(
          kResharding, std::make_unique<ReshardingCumulativeMetricsFieldNameProvider>()),
      _fieldNames(
          static_cast<const ReshardingCumulativeMetricsFieldNameProvider*>(getFieldNames())) {}

StringData ReshardingCumulativeMetrics::fieldNameFor(
    CoordinatorStateEnum state, const ReshardingCumulativeMetricsFieldNameProvider* provider) {
    switch (state) {
        case CoordinatorStateEnum::kInitializing:
            return provider->getForCountInstancesInCoordinatorState1Initializing();

        case CoordinatorStateEnum::kPreparingToDonate:
            return provider->getForCountInstancesInCoordinatorState2PreparingToDonate();

        case CoordinatorStateEnum::kCloning:
            return provider->getForCountInstancesInCoordinatorState3Cloning();

        case CoordinatorStateEnum::kApplying:
            return provider->getForCountInstancesInCoordinatorState4Applying();

        case CoordinatorStateEnum::kBlockingWrites:
            return provider->getForCountInstancesInCoordinatorState5BlockingWrites();

        case CoordinatorStateEnum::kAborting:
            return provider->getForCountInstancesInCoordinatorState6Aborting();

        case CoordinatorStateEnum::kCommitting:
            return provider->getForCountInstancesInCoordinatorState7Committing();

        default:
            uasserted(6438601,
                      str::stream()
                          << "no field name for coordinator state " << static_cast<int32_t>(state));
            break;
    }

    MONGO_UNREACHABLE;
}

StringData ReshardingCumulativeMetrics::fieldNameFor(
    DonorStateEnum state, const ReshardingCumulativeMetricsFieldNameProvider* provider) {
    switch (state) {
        case DonorStateEnum::kPreparingToDonate:
            return provider->getForCountInstancesInDonorState1PreparingToDonate();

        case DonorStateEnum::kDonatingInitialData:
            return provider->getForCountInstancesInDonorState2DonatingInitialData();

        case DonorStateEnum::kDonatingOplogEntries:
            return provider->getForCountInstancesInDonorState3DonatingOplogEntries();

        case DonorStateEnum::kPreparingToBlockWrites:
            return provider->getForCountInstancesInDonorState4PreparingToBlockWrites();

        case DonorStateEnum::kError:
            return provider->getForCountInstancesInDonorState5Error();

        case DonorStateEnum::kBlockingWrites:
            return provider->getForCountInstancesInDonorState6BlockingWrites();

        case DonorStateEnum::kDone:
            return provider->getForCountInstancesInDonorState7Done();

        default:
            uasserted(6438700,
                      str::stream()
                          << "no field name for donor state " << static_cast<int32_t>(state));
            break;
    }

    MONGO_UNREACHABLE;
}

StringData ReshardingCumulativeMetrics::fieldNameFor(
    RecipientStateEnum state, const ReshardingCumulativeMetricsFieldNameProvider* provider) {
    switch (state) {
        case RecipientStateEnum::kAwaitingFetchTimestamp:
            return provider->getForCountInstancesInRecipientState1AwaitingFetchTimestamp();

        case RecipientStateEnum::kCreatingCollection:
            return provider->getForCountInstancesInRecipientState2CreatingCollection();

        case RecipientStateEnum::kCloning:
            return provider->getForCountInstancesInRecipientState3Cloning();

        case RecipientStateEnum::kApplying:
            return provider->getForCountInstancesInRecipientState4Applying();

        case RecipientStateEnum::kError:
            return provider->getForCountInstancesInRecipientState5Error();

        case RecipientStateEnum::kStrictConsistency:
            return provider->getForCountInstancesInRecipientState6StrictConsistency();

        case RecipientStateEnum::kDone:
            return provider->getForCountInstancesInRecipientState7Done();

        default:
            uasserted(6438900,
                      str::stream()
                          << "no field name for recipient state " << static_cast<int32_t>(state));
            break;
    }

    MONGO_UNREACHABLE;
}

void ReshardingCumulativeMetrics::reportActive(BSONObjBuilder* bob) const {
    ShardingDataTransformCumulativeMetrics::reportActive(bob);

    bob->append(_fieldNames->getForOplogEntriesFetched(), _oplogEntriesFetched.load());
    bob->append(_fieldNames->getForOplogEntriesApplied(), _oplogEntriesApplied.load());
    bob->append(_fieldNames->getForInsertsApplied(), _insertsApplied.load());
    bob->append(_fieldNames->getForUpdatesApplied(), _updatesApplied.load());
    bob->append(_fieldNames->getForDeletesApplied(), _deletesApplied.load());
}

void ReshardingCumulativeMetrics::reportLatencies(BSONObjBuilder* bob) const {
    ShardingDataTransformCumulativeMetrics::reportLatencies(bob);

    bob->append(_fieldNames->getForOplogFetchingTotalRemoteBatchRetrievalTimeMillis(),
                _oplogFetchingTotalRemoteBatchesRetrievalTimeMillis.load());
    bob->append(_fieldNames->getForOplogFetchingTotalRemoteBatchesRetrieved(),
                _oplogFetchingTotalRemoteBatchesRetrieved.load());
    bob->append(_fieldNames->getForOplogFetchingTotalLocalInsertTimeMillis(),
                _oplogFetchingTotalLocalInsertTimeMillis.load());
    bob->append(_fieldNames->getForOplogFetchingTotalLocalInserts(),
                _oplogFetchingTotalLocalInserts.load());
    bob->append(_fieldNames->getForOplogApplyingTotalLocalBatchRetrievalTimeMillis(),
                _oplogApplyingTotalBatchesRetrievalTimeMillis.load());
    bob->append(_fieldNames->getForOplogApplyingTotalLocalBatchesRetrieved(),
                _oplogApplyingTotalBatchesRetrieved.load());
    bob->append(_fieldNames->getForOplogApplyingTotalLocalBatchApplyTimeMillis(),
                _oplogBatchAppliedMillis.load());
    bob->append(_fieldNames->getForOplogApplyingTotalLocalBatchesApplied(),
                _oplogBatchApplied.load());
}

void ReshardingCumulativeMetrics::reportCurrentInSteps(BSONObjBuilder* bob) const {
    ShardingDataTransformCumulativeMetrics::reportCurrentInSteps(bob);

    auto reportState = [this, bob](auto state) {
        bob->append(fieldNameFor(state, _fieldNames), getStateCounter(state)->load());
    };

    reportState(CoordinatorStateEnum::kInitializing);
    reportState(CoordinatorStateEnum::kPreparingToDonate);
    reportState(CoordinatorStateEnum::kCloning);
    reportState(CoordinatorStateEnum::kApplying);
    reportState(CoordinatorStateEnum::kBlockingWrites);
    reportState(CoordinatorStateEnum::kAborting);
    reportState(CoordinatorStateEnum::kCommitting);

    reportState(RecipientStateEnum::kAwaitingFetchTimestamp);
    reportState(RecipientStateEnum::kCreatingCollection);
    reportState(RecipientStateEnum::kCloning);
    reportState(RecipientStateEnum::kApplying);
    reportState(RecipientStateEnum::kError);
    reportState(RecipientStateEnum::kStrictConsistency);
    reportState(RecipientStateEnum::kDone);

    reportState(DonorStateEnum::kPreparingToDonate);
    reportState(DonorStateEnum::kDonatingInitialData);
    reportState(DonorStateEnum::kDonatingOplogEntries);
    reportState(DonorStateEnum::kPreparingToBlockWrites);
    reportState(DonorStateEnum::kError);
    reportState(DonorStateEnum::kBlockingWrites);
    reportState(DonorStateEnum::kDone);
}

void ReshardingCumulativeMetrics::onInsertApplied() {
    _insertsApplied.fetchAndAdd(1);
}

void ReshardingCumulativeMetrics::onUpdateApplied() {
    _updatesApplied.fetchAndAdd(1);
}

void ReshardingCumulativeMetrics::onDeleteApplied() {
    _deletesApplied.fetchAndAdd(1);
}

void ReshardingCumulativeMetrics::onOplogEntriesFetched(int64_t numEntries, Milliseconds elapsed) {
    _oplogEntriesFetched.fetchAndAdd(numEntries);
    _oplogFetchingTotalRemoteBatchesRetrieved.fetchAndAdd(1);
    _oplogFetchingTotalRemoteBatchesRetrievalTimeMillis.fetchAndAdd(
        durationCount<Milliseconds>(elapsed));
}

void ReshardingCumulativeMetrics::onOplogEntriesApplied(int64_t numEntries) {
    _oplogEntriesApplied.fetchAndAdd(numEntries);
}

void ReshardingCumulativeMetrics::onOplogLocalBatchApplied(Milliseconds elapsed) {
    _oplogBatchApplied.fetchAndAdd(1);
    _oplogBatchAppliedMillis.fetchAndAdd(durationCount<Milliseconds>(elapsed));
}

void ReshardingCumulativeMetrics::onLocalInsertDuringOplogFetching(
    const Milliseconds& elapsedTime) {
    _oplogFetchingTotalLocalInserts.fetchAndAdd(1);
    _oplogFetchingTotalLocalInsertTimeMillis.fetchAndAdd(durationCount<Milliseconds>(elapsedTime));
}

void ReshardingCumulativeMetrics::onBatchRetrievedDuringOplogApplying(
    const Milliseconds& elapsedTime) {
    _oplogApplyingTotalBatchesRetrieved.fetchAndAdd(1);
    _oplogApplyingTotalBatchesRetrievalTimeMillis.fetchAndAdd(
        durationCount<Milliseconds>(elapsedTime));
}

}  // namespace mongo
