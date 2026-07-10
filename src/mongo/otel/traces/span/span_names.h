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

#include "mongo/util/modules.h"

#include <string_view>

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
class [[MONGO_MOD_PUBLIC]] SpanName {
private:
    class Passkey {
        friend class SpanName;
        explicit constexpr Passkey() = default;
    };

public:
    /** N&O team must own all uses of this passkey variable. */
    [[MONGO_MOD_PRIVATE]] static constexpr Passkey passkeyForNetworkingAndObservabilityOnly{};

    /**
     * Note that this requires a passkey for construction, which only N&O code is allowed to use.
     */
    constexpr SpanName(Passkey, std::string_view name, SampledByDefault sampledByDefault)
        : _name(name), _sampledByDefault{sampledByDefault} {}

    constexpr std::string_view getName() const {
        return _name;
    }

    constexpr SampledByDefault getSampledByDefault() const {
        return _sampledByDefault;
    }

    constexpr bool operator==(const SpanName& other) const {
        return getName() == other.getName();
    }

private:
    std::string_view _name;
    SampledByDefault _sampledByDefault;
};

/**
 * Central registry of OpenTelemetry span names used in the server. When adding a new span to the
 * server, please add an entry to span_names grouped under your team name.
 *
 * This ensures that the N&O team has full ownership over new OTel spans in the server for
 * centralized collaboration with downstream OTel consumers.
 */
namespace span_names {

#define SPAN_NAME_(id, name)                                  \
    [[MONGO_MOD_PUBLIC]] inline constexpr auto id = SpanName( \
        SpanName::passkeyForNetworkingAndObservabilityOnly, name, SampledByDefault{false})

// Test-only
SPAN_NAME_(kTest1, "test_only.span1");
SPAN_NAME_(kTest2, "test_only.span2");
SPAN_NAME_(kTest3, "test_only.span3");

// Resharding spans
SPAN_NAME_(kReshardCollectionCmdInvocationTypedRun, "ReshardCollectionCmd::Invocation::typedRun");
SPAN_NAME_(kReshardingCoordinatorRun, "ReshardingCoordinator::run");
SPAN_NAME_(kReshardingCoordinatorRunUntilReadyToCommit,
           "ReshardingCoordinator::_runUntilReadyToCommit");
SPAN_NAME_(kReshardingCoordinatorCommitting, "ReshardingCoordinator::committing");
SPAN_NAME_(kReshardingCoordinatorAfterFinish, "ReshardingCoordinator::afterFinish");
SPAN_NAME_(kReshardingCoordinatorWaitForCommitMonitor,
           "ReshardingCoordinator::waitForCommitMonitor");
SPAN_NAME_(kReshardingCoordinatorFinalize, "ReshardingCoordinator::finalize");
SPAN_NAME_(kReshardingCoordinatorRecipientPostCloningDeltaCollector,
           "ReshardingCoordinator::RecipientPostCloningDeltaCollector");
SPAN_NAME_(kReshardingCoordinatorAwaitAllDonorsReadyToDonate,
           "ReshardingCoordinator::_awaitAllDonorsReadyToDonate");
SPAN_NAME_(kReshardingCoordinatorAwaitAllRecipientsCloning,
           "ReshardingCoordinator::_awaitAllRecipientsCloning");
SPAN_NAME_(kReshardingCoordinatorFetchAndPersistNumDocumentsToCloneFromDonors,
           "ReshardingCoordinator::_fetchAndPersistNumDocumentsToCloneFromDonors");
SPAN_NAME_(kReshardingCoordinatorAwaitAllRecipientsFinishedCloning,
           "ReshardingCoordinator::_awaitAllRecipientsFinishedCloning");
SPAN_NAME_(kReshardingCoordinatorTellAllDonorsToRefresh,
           "ReshardingCoordinator::_tellAllDonorsToRefresh");
SPAN_NAME_(kReshardingCoordinatorAwaitAllRecipientsFinishedApplying,
           "ReshardingCoordinator::_awaitAllRecipientsFinishedApplying");
SPAN_NAME_(kReshardingCoordinatorTellAllParticipantsReshardingReadyToCommit,
           "ReshardingCoordinator::tellAllParticipantsReshardingReadyToCommit");
SPAN_NAME_(kReshardingCoordinatorAwaitAllRecipientsInStrictConsistency,
           "ReshardingCoordinator::_awaitAllRecipientsInStrictConsistency");
SPAN_NAME_(kReshardingDonorStateMachineRun, "ReshardingDonorService::DonorStateMachine::run");
SPAN_NAME_(kReshardingCoordinatorDonorPostCloningDeltaCollector,
           "ReshardingCoordinator::DonorPostCloningDeltaCollector");
SPAN_NAME_(kReshardingDonorOnPreparingToDonateCalculateTimestampThenTransitionToDonatingInitialData,
           "ReshardingDonorService::_onPreparingToDonate"
           "CalculateTimestampThenTransitionToDonatingInitialData");
SPAN_NAME_(kReshardingDonorAwaitAllRecipientsDoneCloningThenTransitionToDonatingOplogEntries,
           "ReshardingDonorService::_awaitAllRecipientsDoneCloning"
           "ThenTransitionToDonatingOplogEntries");
SPAN_NAME_(kReshardingDonorCreateAndStartChangeStreamsMonitor,
           "ReshardingDonorService::_createAndStartChangeStreamsMonitor");
SPAN_NAME_(kReshardingDonorAwaitAllRecipientsDoneApplyingThenTransitionToPreparingToBlockWrites,
           "ReshardingDonorService::_awaitAllRecipientsDoneApplying"
           "ThenTransitionToPreparingToBlockWrites");
SPAN_NAME_(kReshardingDonorWriteTransactionOplogEntryThenTransitionToBlockingWrites,
           "ReshardingDonorService::_writeTransactionOplogEntry"
           "ThenTransitionToBlockingWrites");
SPAN_NAME_(kReshardingDonorRunUntilBlockingWritesOrErrored,
           "ReshardingDonorService::_runUntilBlockingWritesOrErrored");
SPAN_NAME_(kReshardingDonorNotifyCoordinatorAndAwaitDecision,
           "ReshardingDonorService::_notifyCoordinatorAndAwaitDecision");
SPAN_NAME_(kReshardingRecipientRun, "ReshardingRecipientService::run");
SPAN_NAME_(kReshardingRecipientAwaitAllDonorsPreparedToDonateThenTransitionToCreatingCollection,
           "ReshardingRecipientService::_awaitAllDonorsPreparedToDonate"
           "ThenTransitionToCreatingCollection");
SPAN_NAME_(kReshardingRecipientCreateTemporaryReshardingCollectionThenTransitionToCloning,
           "ReshardingRecipientService::_createTemporaryReshardingCollection"
           "ThenTransitionToCloning");
SPAN_NAME_(kReshardingRecipientCloneThenTransitionToBuildingIndex,
           "ReshardingRecipientService::_cloneThenTransitionToBuildingIndex");
SPAN_NAME_(kReshardingRecipientBuildIndexThenTransitionToApplying,
           "ReshardingRecipientService::_buildIndexThenTransitionToApplying");
SPAN_NAME_(kReshardingRecipientCreateAndStartChangeStreamsMonitor,
           "ReshardingRecipientService::_createAndStartChangeStreamsMonitor");
SPAN_NAME_(kReshardingRecipientAwaitAllDonorsBlockingWritesThenTransitionToStrictConsistency,
           "ReshardingRecipientService::_awaitAllDonorsBlockingWrites"
           "ThenTransitionToStrictConsistency");
SPAN_NAME_(kReshardingRecipientRunUntilStrictConsistencyOrErrored,
           "ReshardingRecipientService::_runUntilStrictConsistencyOrErrored");
SPAN_NAME_(kReshardingRecipientNotifyCoordinatorAndAwaitDecision,
           "ReshardingRecipientService::_notifyCoordinatorAndAwaitDecision");

#undef SPAN_NAME_
}  // namespace span_names

/**
 * Registers a span name for a `mongo::Command`. Must only be called from `Command`'s constructor.
 * Do not add calls to this function anywhere else.
 * The returned reference is stable for the lifetime of the process.
 */
[[MONGO_MOD_PUBLIC]] const SpanName& registerCommandSpanName(
    std::string_view name, SampledByDefault sampledByDefault = SampledByDefault{false});

}  // namespace mongo::otel::traces
