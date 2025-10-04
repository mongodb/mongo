/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/api_parameters.h"
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

private:
    int _numShards;

    int _numCommitVotes{0};
    int _numAbortVotes{0};
    int _numNoVotes{0};

    Timestamp _maxPrepareTimestamp;
    absl::flat_hash_set<NamespaceString> _affectedNamespaces;
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
 */
Future<repl::OpTime> persistDecision(txn::AsyncWorkScheduler& scheduler,
                                     const LogicalSessionId& lsid,
                                     const TxnNumberAndRetryCounter& txnNumberAndRetryCounter,
                                     const txn::ParticipantsList& participants,
                                     const txn::CoordinatorCommitDecision& decision,
                                     const std::vector<NamespaceString>& affectedNamespaces);

/**
 * Sends commit to all shards and returns a future that will be resolved when all participants have
 * responded with success.
 */
Future<void> sendCommit(ServiceContext* service,
                        txn::AsyncWorkScheduler& scheduler,
                        const LogicalSessionId& lsid,
                        const TxnNumberAndRetryCounter& txnNumberAndRetryCounter,
                        const APIParameters& apiParams,
                        const txn::ParticipantsList& participants,
                        Timestamp commitTimestamp);

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
    OperationContextFn operationContextFn = [](OperationContext*) {});

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

}  // namespace txn
}  // namespace mongo
