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

/**
 * Indicates whether a span can initiate a sampled trace by default assuming no other configuration
 * on this specific span.
 *
 * `true` means this span is an entry point to an operation we always want to trace (e.g., the
 * root span of a tracked operation). Only a small number of spans should be `true` — typically
 * only the outermost entry points to operations we want to gather traces for by default. Child
 * spans within a sampled trace are captured automatically regardless of this value.
 *
 * `false` means a trace is never sampled due to the inclusion of this span (but could be via other
 * means).
 */
enum class SampledByDefault : bool {};

/** Wrapper class around a string to ensure `SpanName`s are only constructed in certain places. */
class MONGO_MOD_PUBLIC SpanName {
private:
    class Passkey {
        friend class SpanName;
        explicit constexpr Passkey() = default;
    };

public:
    /** N&O team must own all uses of this passkey variable. */
    MONGO_MOD_PRIVATE static constexpr Passkey passkeyForNetworkingAndObservabilityOnly{};

    /**
     * Note that this requires a passkey for construction, which only N&O code is allowed to use.
     */
    constexpr SpanName(Passkey, StringData name, SampledByDefault sampledByDefault)
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

namespace span_names_details {
/** Absolutely no calls to this function should be made from code that is NOT owned by N&O team. */
MONGO_MOD_FILE_PRIVATE constexpr SpanName make(StringData name,
                                               SampledByDefault sampledByDefault = SampledByDefault{
                                                   false}) {
    return SpanName{SpanName::passkeyForNetworkingAndObservabilityOnly, name, sampledByDefault};
}
}  // namespace span_names_details

/**
 * Central registry of OpenTelemetry span names used in the server. When adding a new span to the
 * server, please add an entry to SpanNames grouped under your team name.
 *
 * This ensures that the N&O team has full ownership over new OTel spans in the server for
 * centralized collaboration with downstream OTel consumers.
 */
class MONGO_MOD_PUBLIC SpanNames {
private:
    /** Wraps span_name_details::make to make the definitions below less verbose. */
    static constexpr auto make = []<typename... Args>(Args&&... args) {
        return span_names_details::make(std::forward<Args>(args)...);
    };

public:
    // Test-only
    static constexpr SpanName kTest1 = make("test_only.span1");
    static constexpr SpanName kTest2 = make("test_only.span2");
    static constexpr SpanName kTest3 = make("test_only.span3");

    // Resharding spans
    static constexpr SpanName kReshardCollectionCmdInvocationTypedRun =
        make("ReshardCollectionCmd::Invocation::typedRun");
    static constexpr SpanName kReshardingCoordinatorRun = make("ReshardingCoordinator::run");
    static constexpr SpanName kReshardingCoordinatorRunUntilReadyToCommit =
        make("ReshardingCoordinator::_runUntilReadyToCommit");
    static constexpr SpanName kReshardingCoordinatorCommitting =
        make("ReshardingCoordinator::committing");
    static constexpr SpanName kReshardingCoordinatorAfterFinish =
        make("ReshardingCoordinator::afterFinish");
    static constexpr SpanName kReshardingCoordinatorWaitForCommitMonitor =
        make("ReshardingCoordinator::waitForCommitMonitor");
    static constexpr SpanName kReshardingCoordinatorFinalize =
        make("ReshardingCoordinator::finalize");
    static constexpr SpanName kReshardingCoordinatorRecipientPostCloningDeltaCollector =
        make("ReshardingCoordinator::RecipientPostCloningDeltaCollector");
    static constexpr SpanName kReshardingCoordinatorAwaitAllDonorsReadyToDonate =
        make("ReshardingCoordinator::_awaitAllDonorsReadyToDonate");
    static constexpr SpanName kReshardingCoordinatorAwaitAllRecipientsCloning =
        make("ReshardingCoordinator::_awaitAllRecipientsCloning");
    static constexpr SpanName kReshardingCoordinatorFetchAndPersistNumDocumentsToCloneFromDonors =
        make("ReshardingCoordinator::_fetchAndPersistNumDocumentsToCloneFromDonors");
    static constexpr SpanName kReshardingCoordinatorAwaitAllRecipientsFinishedCloning =
        make("ReshardingCoordinator::_awaitAllRecipientsFinishedCloning");
    static constexpr SpanName kReshardingCoordinatorTellAllDonorsToRefresh =
        make("ReshardingCoordinator::_tellAllDonorsToRefresh");
    static constexpr SpanName kReshardingCoordinatorAwaitAllRecipientsFinishedApplying =
        make("ReshardingCoordinator::_awaitAllRecipientsFinishedApplying");
    static constexpr SpanName kReshardingCoordinatorTellAllParticipantsReshardingReadyToCommit =
        make("ReshardingCoordinator::tellAllParticipantsReshardingReadyToCommit");
    static constexpr SpanName kReshardingCoordinatorAwaitAllRecipientsInStrictConsistency =
        make("ReshardingCoordinator::_awaitAllRecipientsInStrictConsistency");
    static constexpr SpanName kReshardingDonorStateMachineRun =
        make("ReshardingDonorService::DonorStateMachine::run");
    static constexpr SpanName kReshardingCoordinatorDonorPostCloningDeltaCollector =
        make("ReshardingCoordinator::DonorPostCloningDeltaCollector");
    static constexpr SpanName
        kReshardingDonorOnPreparingToDonateCalculateTimestampThenTransitionToDonatingInitialData =
            make(
                "ReshardingDonorService::_onPreparingToDonate"
                "CalculateTimestampThenTransitionToDonatingInitialData");
    static constexpr SpanName
        kReshardingDonorAwaitAllRecipientsDoneCloningThenTransitionToDonatingOplogEntries = make(
            "ReshardingDonorService::_awaitAllRecipientsDoneCloning"
            "ThenTransitionToDonatingOplogEntries");
    static constexpr SpanName kReshardingDonorCreateAndStartChangeStreamsMonitor =
        make("ReshardingDonorService::_createAndStartChangeStreamsMonitor");
    static constexpr SpanName
        kReshardingDonorAwaitAllRecipientsDoneApplyingThenTransitionToPreparingToBlockWrites = make(
            "ReshardingDonorService::_awaitAllRecipientsDoneApplying"
            "ThenTransitionToPreparingToBlockWrites");
    static constexpr SpanName
        kReshardingDonorWriteTransactionOplogEntryThenTransitionToBlockingWrites = make(
            "ReshardingDonorService::_writeTransactionOplogEntry"
            "ThenTransitionToBlockingWrites");
    static constexpr SpanName kReshardingDonorRunUntilBlockingWritesOrErrored =
        make("ReshardingDonorService::_runUntilBlockingWritesOrErrored");
    static constexpr SpanName kReshardingDonorNotifyCoordinatorAndAwaitDecision =
        make("ReshardingDonorService::_notifyCoordinatorAndAwaitDecision");
    static constexpr SpanName kReshardingRecipientRun = make("ReshardingRecipientService::run");
    static constexpr SpanName
        kReshardingRecipientAwaitAllDonorsPreparedToDonateThenTransitionToCreatingCollection = make(
            "ReshardingRecipientService::_awaitAllDonorsPreparedToDonate"
            "ThenTransitionToCreatingCollection");
    static constexpr SpanName
        kReshardingRecipientCreateTemporaryReshardingCollectionThenTransitionToCloning = make(
            "ReshardingRecipientService::_createTemporaryReshardingCollection"
            "ThenTransitionToCloning");
    static constexpr SpanName kReshardingRecipientCloneThenTransitionToBuildingIndex =
        make("ReshardingRecipientService::_cloneThenTransitionToBuildingIndex");
    static constexpr SpanName kReshardingRecipientBuildIndexThenTransitionToApplying =
        make("ReshardingRecipientService::_buildIndexThenTransitionToApplying");
    static constexpr SpanName kReshardingRecipientCreateAndStartChangeStreamsMonitor =
        make("ReshardingRecipientService::_createAndStartChangeStreamsMonitor");
    static constexpr SpanName
        kReshardingRecipientAwaitAllDonorsBlockingWritesThenTransitionToStrictConsistency = make(
            "ReshardingRecipientService::_awaitAllDonorsBlockingWrites"
            "ThenTransitionToStrictConsistency");
    static constexpr SpanName kReshardingRecipientRunUntilStrictConsistencyOrErrored =
        make("ReshardingRecipientService::_runUntilStrictConsistencyOrErrored");
    static constexpr SpanName kReshardingRecipientNotifyCoordinatorAndAwaitDecision =
        make("ReshardingRecipientService::_notifyCoordinatorAndAwaitDecision");
};

/**
 * Registers a span name for a `mongo::Command`. Must only be called from `Command`'s constructor.
 * Do not add calls to this function anywhere else.
 * The returned reference is stable for the lifetime of the process.
 */
MONGO_MOD_PUBLIC const SpanName& registerCommandSpanName(
    StringData name, SampledByDefault sampledByDefault = SampledByDefault{false});

}  // namespace mongo::otel::traces
