// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/modules.h"


#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/s/transaction_coordinator_document_gen.h"
#include "mongo/db/s/transaction_coordinator_futures_util.h"
#include "mongo/db/s/transaction_coordinator_structures.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/util/future.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_set.h>
#include <absl/hash/hash.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace txn {

/**
 * Upserts a document of the form:
 *
 * {
 *    _id: {lsid: <lsid>, txnNumber: <txnNumber>, txnRetryCounter: <txnRetryCounter>}
 *    participants: ["shard0000", "shard0001"]
 * }
 *
 * into config.transaction_coordinators and returns the opTime of the upsert.
 *
 * Throws if the upsert fails or waiting for writeConcern fails.
 *
 * If the upsert returns a DuplicateKey error, converts it to an anonymous error, because it means a
 * document for the (lsid, txnNumber, txnRetryCounter) exists with a different participant list.
 */
Future<repl::OpTime> persistParticipantsList(
    txn::AsyncWorkScheduler& scheduler,
    const LogicalSessionId& lsid,
    const TxnNumberAndRetryCounter& txnNumberAndRetryCounter,
    const txn::ParticipantsList& participants);

struct PrepareResponse;

/**
 * Returns true if the given end-of-transaction accounting metadata is consistent with the given
 * participant list, i.e. every shard the metadata references is a participant of the transaction.
 * Used to reject malformed metadata recovered from the coordinator document before it is attached
 * to the participants' commitTransaction commands.
 */
bool isEndOfTransactionMetadataConsistentWithParticipants(
    const EndOfTransactionScopesInfoBase& metadata, const std::vector<ShardId>& participants);

class PrepareVoteConsensus {
public:
    PrepareVoteConsensus(int numShards) : _numShards(numShards) {}

    void registerVote(const PrepareResponse& vote);

    /**
     * May only be called when all of `numShards` have called `registerVote` above. Contains the
     * commit decision of the vote, which would only be kCommit if all shards have responded with a
     * 'commit'.
     */
    CoordinatorCommitDecision decision() const;

    absl::flat_hash_set<NamespaceString> releaseAffectedNamespaces() {
        return std::move(_affectedNamespaces);
    }

    /**
     * Computes the end-of-transaction accounting metadata (participant sets) from the recorded
     * votes. Read-only participants are excluded. `shardId` is left defaulted and must be set by
     * the caller.
     */
    EndOfTransactionScopesInfoBase releaseEndOfTransactionMetadata();

private:
    int _numShards;

    int _numCommitVotes{0};
    int _numAbortVotes{0};
    int _numNoVotes{0};

    Timestamp _maxPrepareTimestamp;
    absl::flat_hash_set<NamespaceString> _affectedNamespaces;

    // Namespaces written by each write-participant shard, used to derive the participant sets.
    std::map<ShardId, std::vector<NamespaceString>> _namespacesByShard;

    boost::optional<Status> _abortStatus;
};

/**
 * Sends prepare to all participants and keeps retrying the prepare call infinitely until a vote or
 * one of the errors below is encountered. The future will be resolved as follows:
 *  a) kCommit if all participants voted commit
 *  b) kAbort if at least one participant voted abort
 *  c) kAbort if the scheduler was interrupted with a TransactionCoordinatorReachedAbortDecision
 *      error code
 *  d) kAbort if one of the shards received a ShardNotFound error
 *  e) Exception TransactionCoordinatorSteppingDown if the scheduler is interrupted with that code
 */
Future<PrepareVoteConsensus> sendPrepare(ServiceContext* service,
                                         txn::AsyncWorkScheduler& scheduler,
                                         const LogicalSessionId& lsid,
                                         const TxnNumberAndRetryCounter& txnNumberAndRetryCounter,
                                         const APIParameters& apiParams,
                                         const txn::ParticipantsList& participants);

/**
 * If 'commitTimestamp' is boost::none, updates the document in config.transaction_coordinators
 * for
 *
 * (lsid, txnNumber, txnRetryCounter) to be:
 *
 * {
 *    _id: {lsid: <lsid>, txnNumber: <txnNumber>, txnRetryCounter: <txnRetryCounter>}
 *    participants: ["shard0000", "shard0001"]
 *    decision: "abort"
 * }
 *
 * else updates the document to be:
 *
 * {
 *    _id: {lsid: <lsid>, txnNumber: <txnNumber>, txnRetryCounter: <txnRetryCounter>}
 *    participants: ["shard0000", "shard0001"]
 *    decision: "commit"
 *    commitTimestamp: Timestamp(xxxxxxxx, x),
 * }
 *
 * Returns the opTime of the write.
 *
 * Throws if the update fails or waiting for writeConcern fails.
 *
 * If the update succeeds but did not update any document, throws an anonymous error, because it
 * means either no document for (lsid, txnNumber, txnRetryCounter) exists, or a document exists but
 * has a different participant list, different decision, or different commit Timestamp.
 *
 * The end-of-transaction accounting metadata is written into the document iff
 * 'endOfTransactionScopes' has at least one cluster participant. The caller decides whether the
 * metadata is part of the decision; this function does not consult the feature flag or kill switch.
 */
Future<repl::OpTime> persistDecision(
    txn::AsyncWorkScheduler& scheduler,
    const LogicalSessionId& lsid,
    const TxnNumberAndRetryCounter& txnNumberAndRetryCounter,
    const txn::ParticipantsList& participants,
    const txn::CoordinatorCommitDecision& decision,
    const std::vector<NamespaceString>& affectedNamespaces,
    const EndOfTransactionScopesInfoBase& endOfTransactionScopes = {});

/**
 * Sends commit to all shards and returns a future that will be resolved when all participants have
 * responded with success. When 'endOfTransactionScopes' has at least one cluster participant, each
 * participant's commitTransaction command carries the metadata with that participant's shard id.
 * The caller decides whether the metadata is part of the decision; this function does not consult
 * the feature flag or kill switch.
 */
Future<void> sendCommit(ServiceContext* service,
                        txn::AsyncWorkScheduler& scheduler,
                        const LogicalSessionId& lsid,
                        const TxnNumberAndRetryCounter& txnNumberAndRetryCounter,
                        const APIParameters& apiParams,
                        const txn::ParticipantsList& participants,
                        Timestamp commitTimestamp,
                        const EndOfTransactionScopesInfoBase& endOfTransactionScopes = {});

/**
 * Sends abort to all shards and returns a future that will be resolved when all participants have
 * responded with success.
 */
Future<void> sendAbort(ServiceContext* service,
                       txn::AsyncWorkScheduler& scheduler,
                       const LogicalSessionId& lsid,
                       const TxnNumberAndRetryCounter& txnNumberAndRetryCounter,
                       const APIParameters& apiParams,
                       const txn::ParticipantsList& participants);

/**
 * Deletes the document in config.transaction_coordinators for (lsid, txnNumber).
 *
 * Does *not* wait for the delete to be majority-committed.
 *
 * Throws if the update fails.
 *
 * If the delete succeeds but did not delete any document, throws an anonymous error, because it
 * means either no document for (lsid, txnNumber, txnRetryCounter) exists, or a document exists but
 * without a decision.
 */
Future<void> deleteCoordinatorDoc(txn::AsyncWorkScheduler& scheduler,
                                  const LogicalSessionId& lsid,
                                  const TxnNumberAndRetryCounter& txnNumberAndRetryCounter);

/**
 * Reads and returns all documents in config.transaction_coordinators.
 */
std::vector<txn::TransactionCoordinatorDocument> readAllCoordinatorDocs(OperationContext* opCtx);

//
// These methods are used internally and are exposed for unit-testing purposes only
//

/**
 * Sends prepare to the given shard and returns a future, which will be set with the vote.
 *
 * This method will retry until it receives a non-retryable response from the remote node or until
 * the scheduler under which it is running is shut down. Because of this it can return only the
 * following error code(s):
 *   - TransactionCoordinatorSteppingDown
 *   - ShardNotFound
 */
struct PrepareResponse {
    // Shard id from which the response was received
    ShardId shardId;

    // If set to none, this means the shard did not produce a vote
    boost::optional<txn::PrepareVote> vote;

    // Will only be set if the vote was kCommit
    boost::optional<Timestamp> prepareTimestamp;

    // Will only be set if the vote was kCommit
    std::vector<NamespaceString> affectedNamespaces;

    // Will only be set if the vote was kAbort or no value
    boost::optional<Status> abortReason;
};
Future<PrepareResponse> sendPrepareToShard(
    ServiceContext* service,
    txn::AsyncWorkScheduler& scheduler,
    const LogicalSessionId& lsid,
    const TxnNumberAndRetryCounter& txnNumberAndRetryCounter,
    const ShardId& shardId,
    const BSONObj& prepareCommandObj,
    OperationContextFn operationContextFn = [](OperationContext*) {});

/**
 * Sends a command corresponding to a commit decision (i.e. commitTransaction or*
 * abortTransaction) to the given shard and returns a future, which will be set with the result.
 *
 * Used for sendCommit and sendAbort.
 *
 * 'commandObjWithoutEndOfTransactionScopes' is set when 'commandObj' carries the end-of-transaction
 * accounting metadata. If the shard rejects the command with IDLUnknownField (i.e. its binary
 * predates the metadata field), the metadata is stripped, and the commit is retried without the
 * metadata.
 *
 * This method will retry until it receives a response from the remote node which can be
 * interpreted as vote abort (e.g. NoSuchTransaction), or until the scheduler under which it is
 * running is shut down. Because of this it can return only the following error code(s):
 *   - TransactionCoordinatorSteppingDown
 */
Future<void> sendDecisionToShard(
    ServiceContext* service,
    txn::AsyncWorkScheduler& scheduler,
    const LogicalSessionId& lsid,
    const TxnNumberAndRetryCounter& txnNumberAndRetryCounter,
    const ShardId& shardId,
    const BSONObj& commandObj,
    OperationContextFn operationContextFn = [](OperationContext*) {},
    boost::optional<BSONObj> commandObjWithoutEndOfTransactionScopes = boost::none);

Future<void> writeEndOfTransaction(txn::AsyncWorkScheduler& scheduler,
                                   const LogicalSessionId& lsid,
                                   const TxnNumberAndRetryCounter& txnNumberAndRetryCounter,
                                   const std::vector<NamespaceString>& affectedNamespaces);

/**
 * Returns a string representation of the transaction id represented by the given session id and
 * transaction number.
 */
std::string txnIdToString(const LogicalSessionId& lsid,
                          const TxnNumberAndRetryCounter& txnNumberAndRetryCounter);

/**
 * For testing only: resets the log severity suppressor used for the "shard rejected the
 * end-of-transaction metadata" message, so the next call returns the unsuppressed (Info) severity.
 * Pass nullptr to restore the system clock. Not thread-safe.
 */
void resetEndOfTransactionMetadataRejectedLogSeveritySuppressor_forTest(ClockSource* cs);

}  // namespace txn
}  // namespace mongo
