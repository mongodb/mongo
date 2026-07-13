// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/otel/traces/span/span_name.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo::otel::traces {

/**
 * Central registry of OpenTelemetry span names used in the server. When adding a new span to the
 * server, please add an entry to span_names grouped under your team name.
 *
 * This ensures that the N&O team has full ownership over new OTel spans in the server for
 * centralized collaboration with downstream OTel consumers.
 */
namespace span_names {

#define SPAN_NAME_(id, name)                        \
    [[MONGO_MOD_PUBLIC]] inline constexpr auto id = \
        SpanName(SpanName::passkeyForNetworkingAndObservabilityOnly, name)

// Generic RPC span name as a fallback if command name is unavailable. This is the name of the rpc
// protocol per otel semantic conventions
// (see https://opentelemetry.io/docs/specs/semconv/rpc/rpc-spans/#name).
SPAN_NAME_(kMongoRPC, "mongorpc");

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

// Test-only
SPAN_NAME_(kTest1, "test_only.span1");
SPAN_NAME_(kTest2, "test_only.span2");
SPAN_NAME_(kTest3, "test_only.span3");
SPAN_NAME_(kTest4, "test_only.span4");

#undef SPAN_NAME_
}  // namespace span_names

/**
 * Registers a span name for a `mongo::Command`. Must only be called from `Command`'s constructor.
 * Do not add calls to this function anywhere else.
 * The returned reference is stable for the lifetime of the process.
 */
[[MONGO_MOD_PUBLIC]] const SpanName& registerCommandSpanName(std::string_view name);

/**
 * Gets the span name for a command name. Returns nullptr if the command name is not found. The
 * returned pointer will remain valid for the lifetime of the process.
 */
[[MONGO_MOD_PUBLIC]] const SpanName* lookupCommandSpanName(std::string_view name);

}  // namespace mongo::otel::traces
