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
SPAN_NAME_(kReshardCollectionCmdInvocationTypedRun, "reshard_collection_cmd.invocation.typed_run");
SPAN_NAME_(kReshardingCoordinatorRun, "resharding_coordinator.run");
SPAN_NAME_(kReshardingCoordinatorRunUntilReadyToCommit,
           "resharding_coordinator.run_until_ready_to_commit");
SPAN_NAME_(kReshardingCoordinatorCommitting, "resharding_coordinator.committing");
SPAN_NAME_(kReshardingCoordinatorAfterFinish, "resharding_coordinator.after_finish");
SPAN_NAME_(kReshardingCoordinatorWaitForCommitMonitor,
           "resharding_coordinator.wait_for_commit_monitor");
SPAN_NAME_(kReshardingCoordinatorFinalize, "resharding_coordinator.finalize");
SPAN_NAME_(kReshardingCoordinatorRecipientPostCloningDeltaCollector,
           "resharding_coordinator.recipient_post_cloning_delta_collector");
SPAN_NAME_(kReshardingCoordinatorAwaitAllDonorsReadyToDonate,
           "resharding_coordinator.await_all_donors_ready_to_donate");
SPAN_NAME_(kReshardingCoordinatorAwaitAllRecipientsCloning,
           "resharding_coordinator.await_all_recipients_cloning");
SPAN_NAME_(kReshardingCoordinatorFetchAndPersistNumDocumentsToCloneFromDonors,
           "resharding_coordinator.fetch_and_persist_num_documents_to_clone_from_donors");
SPAN_NAME_(kReshardingCoordinatorAwaitAllRecipientsFinishedCloning,
           "resharding_coordinator.await_all_recipients_finished_cloning");
SPAN_NAME_(kReshardingCoordinatorTellAllDonorsToRefresh,
           "resharding_coordinator.tell_all_donors_to_refresh");
SPAN_NAME_(kReshardingCoordinatorAwaitAllRecipientsFinishedApplying,
           "resharding_coordinator.await_all_recipients_finished_applying");
SPAN_NAME_(kReshardingCoordinatorTellAllParticipantsReshardingReadyToCommit,
           "resharding_coordinator.tell_all_participants_resharding_ready_to_commit");
SPAN_NAME_(kReshardingCoordinatorAwaitAllRecipientsInStrictConsistency,
           "resharding_coordinator.await_all_recipients_in_strict_consistency");
SPAN_NAME_(kReshardingDonorStateMachineRun, "resharding_donor_service.donor_state_machine.run");
SPAN_NAME_(kReshardingCoordinatorDonorPostCloningDeltaCollector,
           "resharding_coordinator.donor_post_cloning_delta_collector");
SPAN_NAME_(kReshardingDonorOnPreparingToDonateCalculateTimestampThenTransitionToDonatingInitialData,
           "resharding_donor_service.on_preparing_to_donate_calculate_timestamp"
           "_then_transition_to_donating_initial_data");
SPAN_NAME_(kReshardingDonorAwaitAllRecipientsDoneCloningThenTransitionToDonatingOplogEntries,
           "resharding_donor_service.await_all_recipients_done_cloning"
           "_then_transition_to_donating_oplog_entries");
SPAN_NAME_(kReshardingDonorCreateAndStartChangeStreamsMonitor,
           "resharding_donor_service.create_and_start_change_streams_monitor");
SPAN_NAME_(kReshardingDonorAwaitAllRecipientsDoneApplyingThenTransitionToPreparingToBlockWrites,
           "resharding_donor_service.await_all_recipients_done_applying"
           "_then_transition_to_preparing_to_block_writes");
SPAN_NAME_(kReshardingDonorWriteTransactionOplogEntryThenTransitionToBlockingWrites,
           "resharding_donor_service.write_transaction_oplog_entry"
           "_then_transition_to_blocking_writes");
SPAN_NAME_(kReshardingDonorRunUntilBlockingWritesOrErrored,
           "resharding_donor_service.run_until_blocking_writes_or_errored");
SPAN_NAME_(kReshardingDonorNotifyCoordinatorAndAwaitDecision,
           "resharding_donor_service.notify_coordinator_and_await_decision");
SPAN_NAME_(kReshardingRecipientRun, "resharding_recipient_service.run");
SPAN_NAME_(kReshardingRecipientAwaitAllDonorsPreparedToDonateThenTransitionToCreatingCollection,
           "resharding_recipient_service.await_all_donors_prepared_to_donate"
           "_then_transition_to_creating_collection");
SPAN_NAME_(kReshardingRecipientCreateTemporaryReshardingCollectionThenTransitionToCloning,
           "resharding_recipient_service.create_temporary_resharding_collection"
           "_then_transition_to_cloning");
SPAN_NAME_(kReshardingRecipientCloneThenTransitionToBuildingIndex,
           "resharding_recipient_service.clone_then_transition_to_building_index");
SPAN_NAME_(kReshardingRecipientBuildIndexThenTransitionToApplying,
           "resharding_recipient_service.build_index_then_transition_to_applying");
SPAN_NAME_(kReshardingRecipientCreateAndStartChangeStreamsMonitor,
           "resharding_recipient_service.create_and_start_change_streams_monitor");
SPAN_NAME_(kReshardingRecipientAwaitAllDonorsBlockingWritesThenTransitionToStrictConsistency,
           "resharding_recipient_service.await_all_donors_blocking_writes"
           "_then_transition_to_strict_consistency");
SPAN_NAME_(kReshardingRecipientRunUntilStrictConsistencyOrErrored,
           "resharding_recipient_service.run_until_strict_consistency_or_errored");
SPAN_NAME_(kReshardingRecipientNotifyCoordinatorAndAwaitDecision,
           "resharding_recipient_service.notify_coordinator_and_await_decision");

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
