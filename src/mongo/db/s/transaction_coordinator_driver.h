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

#include <vector>

#include "mongo/db/logical_session_id.h"
#include "mongo/db/s/transaction_coordinator_document_gen.h"
#include "mongo/db/s/transaction_coordinator_futures_util.h"
#include "mongo/executor/task_executor.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/future.h"

namespace mongo {

/**
 * Single instance of this class is owned by each TransactionCoordinator. It abstracts the
 * long-running blocking operations into a futurized interface and ensures that all outstanding
 * operations are joined at completion.
 *
 * Methods on this class must be called from a single thread at a time and other than cancellation,
 * no other operations are allowed until any returned futures are signalled.
 */
class TransactionCoordinatorDriver {
    TransactionCoordinatorDriver(const TransactionCoordinatorDriver&) = delete;
    TransactionCoordinatorDriver& operator=(const TransactionCoordinatorDriver&) = delete;

public:
    TransactionCoordinatorDriver(ServiceContext* serviceContext,
                                 txn::AsyncWorkScheduler& scheduler);

    /**
     * Upserts a document of the form:
     *
     * {
     *    _id: {lsid: <lsid>, txnNumber: <txnNumber>}
     *    participants: ["shard0000", "shard0001"]
     * }
     *
     * into config.transaction_coordinators and waits for the upsert to be majority-committed.
     *
     * Throws if the upsert fails or waiting for writeConcern fails.
     *
     * If the upsert returns a DuplicateKey error, converts it to an anonymous error, because it
     * means a document for the (lsid, txnNumber) exists with a different participant list.
     */
    Future<void> persistParticipantList(const LogicalSessionId& lsid,
                                        TxnNumber txnNumber,
                                        std::vector<ShardId> participantList);

    /**
     * Sends prepare to all participants and returns a future that will be resolved when either:
     *    a) All participants have responded with a vote to commit, or
     *    b) At least one participant votes to abort.
     *
     * If all participants vote to commit, the result will contain the max prepare timestamp of all
     * prepare timestamps attached to the participants' responses. Otherwise the result will simply
     * contain the decision to abort.
     */
    struct PrepareVoteConsensus {
        // Optional decision, if any was reached (decision could be empty if no response was
        // received)
        boost::optional<txn::CommitDecision> decision;

        // Should only be consulted if the decision is commit and contains the maximum prepare
        // timestamp across all participants
        boost::optional<Timestamp> maxPrepareTimestamp;
    };
    Future<PrepareVoteConsensus> sendPrepare(const std::vector<ShardId>& participantShards,
                                             const LogicalSessionId& lsid,
                                             TxnNumber txnNumber);

    /**
     * If 'commitTimestamp' is boost::none, updates the document in config.transaction_coordinators
     * for
     *
     * (lsid, txnNumber) to be:
     *
     * {
     *    _id: {lsid: <lsid>, txnNumber: <txnNumber>}
     *    participants: ["shard0000", "shard0001"]
     *    decision: "abort"
     * }
     *
     * else updates the document to be:
     *
     * {
     *    _id: {lsid: <lsid>, txnNumber: <txnNumber>}
     *    participants: ["shard0000", "shard0001"]
     *    decision: "commit"
     *    commitTimestamp: Timestamp(xxxxxxxx, x),
     * }
     *
     * and waits for the update to be majority-committed.
     *
     * Throws if the update fails or waiting for writeConcern fails.
     *
     * If the update succeeds but did not update any document, throws an anonymous error, because it
     * means either no document for (lsid, txnNumber) exists, or a document exists but has a
     * different participant list, different decision, or different commit Timestamp.
     */
    Future<void> persistDecision(const LogicalSessionId& lsid,
                                 TxnNumber txnNumber,
                                 std::vector<ShardId> participantList,
                                 const boost::optional<Timestamp>& commitTimestamp);

    /**
     * Sends commit to all shards and returns a future that will be resolved when all participants
     * have responded with success.
     */
    Future<void> sendCommit(const std::vector<ShardId>& participantShards,
                            const LogicalSessionId& lsid,
                            TxnNumber txnNumber,
                            Timestamp commitTimestamp);

    /**
     * Sends abort to all shards and returns a future that will be resolved when all participants
     * have responded with success.
     */
    Future<void> sendAbort(const std::vector<ShardId>& participantShards,
                           const LogicalSessionId& lsid,
                           TxnNumber txnNumber);

    /**
     * Deletes the document in config.transaction_coordinators for (lsid, txnNumber).
     *
     * Does *not* wait for the delete to be majority-committed.
     *
     * Throws if the update fails.
     *
     * If the update succeeds but did not update any document, throws an anonymous error, because it
     * means either no document for (lsid, txnNumber) exists, or a document exists but without a
     * decision.
     */
    Future<void> deleteCoordinatorDoc(const LogicalSessionId& lsid, TxnNumber txnNumber);

    /**
     * Reads and returns all documents in config.transaction_coordinators.
     */
    static std::vector<txn::TransactionCoordinatorDocument> readAllCoordinatorDocs(
        OperationContext* opCtx);

    //
    // These methods are used internally and are exposed for unit-testing purposes only
    //

    /**
     * Sends prepare to the given shard and returns a future, which will be set with the vote.
     *
     * This method will retry until it receives a non-retryable response from the remote node or
     * until the scheduler under which it is running is shut down. Because of this it can return
     * only the following error code(s):
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
    };
    Future<PrepareResponse> sendPrepareToShard(txn::AsyncWorkScheduler& scheduler,
                                               const ShardId& shardId,
                                               const BSONObj& prepareCommandObj);

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
    Future<void> sendDecisionToParticipantShard(txn::AsyncWorkScheduler& scheduler,
                                                const ShardId& shardId,
                                                const BSONObj& commandObj);

private:
    ServiceContext* _serviceContext;

    txn::AsyncWorkScheduler& _scheduler;
};

}  // namespace mongo
