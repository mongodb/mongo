/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
#include "mongo/util/modules.h"

namespace mongo::otel::traces {
class SpanNameMaker;

/** Helper to implement the passkey idiom. */
template <typename T>
class MONGO_MOD_PUBLIC Passkey {
private:
    friend T;
    constexpr Passkey() = default;
};

/**
 * Indicates whether a span should be included in sampled traces by default when no explicit
 * sampling decision has been made by the caller.
 *
 * "yes" means this span is an entry point to an operation we always want to trace (e.g., the
 * root span of a tracked operation). Only a small number of spans should be "yes" — typically
 * only the outermost entry points to operations listed as sampled-by-default in the OTel design.
 * Child spans within a sampled trace are captured automatically and should remain "no".
 *
 * "no" means the span defers sampling to the parent context or the configured sampler.
 */
enum class SampledByDefault : bool { no, yes };

/** Wrapper class around a string to ensure `SpanName`s are only constructed in certain places. */
class MONGO_MOD_PUBLIC SpanName {
public:
    /**
     * Note that this can only be constructed by code allowed to access the passkey. N&O must have
     * ownership of the files defining and instantiating the Passkey types. Additional Passkey types
     * are meant to facilitate cases where the span names should not be visible outside some
     * module, in order to prevent leaking information related to that module.
     */
    constexpr SpanName(StringData name, Passkey<SpanNameMaker>, SampledByDefault sampledByDefault)
        : _name(name), _sampledByDefault{sampledByDefault} {}

    constexpr StringData getName() const {
        return _name;
    }

    constexpr SampledByDefault getSampledByDefault() const {
        return _sampledByDefault;
    }

    constexpr bool operator==(const SpanName& other) const {
        return getName() == other.getName();
    }

private:
    StringData _name;
    SampledByDefault _sampledByDefault;
};

/** Helper to create SpanName instances. */
class MONGO_MOD_FILE_PRIVATE SpanNameMaker{public : static constexpr SpanName make(
    StringData name, SampledByDefault sampledByDefault = SampledByDefault::no){
    return SpanName(name, Passkey<SpanNameMaker>{}, sampledByDefault);
}  // namespace mongo::otel::traces
}
;

/**
 * Central registry of OpenTelemetry span names used in the server. When adding a new span to the
 * server, please add an entry to SpanNames grouped under your team name.
 *
 * This ensures that the N&O team has full ownership over new OTel spans in the server for
 * centralized collaboration with downstream OTel consumers.
 */
class MONGO_MOD_PUBLIC SpanNames {
public:
    // Test-only
    static constexpr SpanName kTest1 = SpanNameMaker::make("test_only.span1");
    static constexpr SpanName kTest2 = SpanNameMaker::make("test_only.span2");
    static constexpr SpanName kTest3 = SpanNameMaker::make("test_only.span3");

    // Resharding spans
    static constexpr SpanName kReshardCollectionCmdInvocationTypedRun =
        SpanNameMaker::make("ReshardCollectionCmd::Invocation::typedRun");
    static constexpr SpanName kReshardingCoordinatorRun =
        SpanNameMaker::make("ReshardingCoordinator::run");
    static constexpr SpanName kReshardingCoordinatorRunUntilReadyToCommit =
        SpanNameMaker::make("ReshardingCoordinator::_runUntilReadyToCommit");
    static constexpr SpanName kReshardingCoordinatorCommitting =
        SpanNameMaker::make("ReshardingCoordinator::committing");
    static constexpr SpanName kReshardingCoordinatorAfterFinish =
        SpanNameMaker::make("ReshardingCoordinator::afterFinish");
    static constexpr SpanName kReshardingCoordinatorWaitForCommitMonitor =
        SpanNameMaker::make("ReshardingCoordinator::waitForCommitMonitor");
    static constexpr SpanName kReshardingCoordinatorFinalize =
        SpanNameMaker::make("ReshardingCoordinator::finalize");
    static constexpr SpanName kReshardingCoordinatorRecipientPostCloningDeltaCollector =
        SpanNameMaker::make("ReshardingCoordinator::RecipientPostCloningDeltaCollector");
    static constexpr SpanName kReshardingCoordinatorAwaitAllDonorsReadyToDonate =
        SpanNameMaker::make("ReshardingCoordinator::_awaitAllDonorsReadyToDonate");
    static constexpr SpanName kReshardingCoordinatorAwaitAllRecipientsCloning =
        SpanNameMaker::make("ReshardingCoordinator::_awaitAllRecipientsCloning");
    static constexpr SpanName kReshardingCoordinatorFetchAndPersistNumDocumentsToCloneFromDonors =
        SpanNameMaker::make("ReshardingCoordinator::_fetchAndPersistNumDocumentsToCloneFromDonors");
    static constexpr SpanName kReshardingCoordinatorAwaitAllRecipientsFinishedCloning =
        SpanNameMaker::make("ReshardingCoordinator::_awaitAllRecipientsFinishedCloning");
    static constexpr SpanName kReshardingCoordinatorTellAllDonorsToRefresh =
        SpanNameMaker::make("ReshardingCoordinator::_tellAllDonorsToRefresh");
    static constexpr SpanName kReshardingCoordinatorAwaitAllRecipientsFinishedApplying =
        SpanNameMaker::make("ReshardingCoordinator::_awaitAllRecipientsFinishedApplying");
    static constexpr SpanName kReshardingCoordinatorTellAllParticipantsReshardingReadyToCommit =
        SpanNameMaker::make("ReshardingCoordinator::tellAllParticipantsReshardingReadyToCommit");
    static constexpr SpanName kReshardingCoordinatorAwaitAllRecipientsInStrictConsistency =
        SpanNameMaker::make("ReshardingCoordinator::_awaitAllRecipientsInStrictConsistency");
    static constexpr SpanName kReshardingDonorStateMachineRun =
        SpanNameMaker::make("ReshardingDonorService::DonorStateMachine::run");
    static constexpr SpanName kReshardingCoordinatorDonorPostCloningDeltaCollector =
        SpanNameMaker::make("ReshardingCoordinator::DonorPostCloningDeltaCollector");
    static constexpr SpanName
        kReshardingDonorOnPreparingToDonateCalculateTimestampThenTransitionToDonatingInitialData =
            SpanNameMaker::make(
                "ReshardingDonorService::_"
                "onPreparingToDonateCalculateTimestampThenTransitionToDonatingInitialData");
    static constexpr SpanName
        kReshardingDonorAwaitAllRecipientsDoneCloningThenTransitionToDonatingOplogEntries =
            SpanNameMaker::make(
                "ReshardingDonorService::_"
                "awaitAllRecipientsDoneCloningThenTransitionToDonatingOplogEntries");
    static constexpr SpanName kReshardingDonorCreateAndStartChangeStreamsMonitor =
        SpanNameMaker::make("ReshardingDonorService::_createAndStartChangeStreamsMonitor");
    static constexpr SpanName
        kReshardingDonorAwaitAllRecipientsDoneApplyingThenTransitionToPreparingToBlockWrites =
            SpanNameMaker::make(
                "ReshardingDonorService::_"
                "awaitAllRecipientsDoneApplyingThenTransitionToPreparingToBlockWrites");
    static constexpr SpanName
        kReshardingDonorWriteTransactionOplogEntryThenTransitionToBlockingWrites =
            SpanNameMaker::make(
                "ReshardingDonorService::_"
                "writeTransactionOplogEntryThenTransitionToBlockingWrites");
    static constexpr SpanName kReshardingDonorRunUntilBlockingWritesOrErrored =
        SpanNameMaker::make("ReshardingDonorService::_runUntilBlockingWritesOrErrored");
    static constexpr SpanName kReshardingDonorNotifyCoordinatorAndAwaitDecision =
        SpanNameMaker::make("ReshardingDonorService::_notifyCoordinatorAndAwaitDecision");
    static constexpr SpanName kReshardingRecipientRun =
        SpanNameMaker::make("ReshardingRecipientService::run");
    static constexpr SpanName
        kReshardingRecipientAwaitAllDonorsPreparedToDonateThenTransitionToCreatingCollection =
            SpanNameMaker::make(
                "ReshardingRecipientService::_"
                "awaitAllDonorsPreparedToDonateThenTransitionToCreatingCollection");
    static constexpr SpanName
        kReshardingRecipientCreateTemporaryReshardingCollectionThenTransitionToCloning =
            SpanNameMaker::make(
                "ReshardingRecipientService::_"
                "createTemporaryReshardingCollectionThenTransitionToCloning");
    static constexpr SpanName kReshardingRecipientCloneThenTransitionToBuildingIndex =
        SpanNameMaker::make("ReshardingRecipientService::_cloneThenTransitionToBuildingIndex");
    static constexpr SpanName kReshardingRecipientBuildIndexThenTransitionToApplying =
        SpanNameMaker::make("ReshardingRecipientService::_buildIndexThenTransitionToApplying");
    static constexpr SpanName kReshardingRecipientCreateAndStartChangeStreamsMonitor =
        SpanNameMaker::make("ReshardingRecipientService::_createAndStartChangeStreamsMonitor");
    static constexpr SpanName
        kReshardingRecipientAwaitAllDonorsBlockingWritesThenTransitionToStrictConsistency =
            SpanNameMaker::make(
                "ReshardingRecipientService::_"
                "awaitAllDonorsBlockingWritesThenTransitionToStrictConsistency");
    static constexpr SpanName kReshardingRecipientRunUntilStrictConsistencyOrErrored =
        SpanNameMaker::make("ReshardingRecipientService::_runUntilStrictConsistencyOrErrored");
    static constexpr SpanName kReshardingRecipientNotifyCoordinatorAndAwaitDecision =
        SpanNameMaker::make("ReshardingRecipientService::_notifyCoordinatorAndAwaitDecision");
};

}  // namespace mongo::otel::traces
