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

#include <boost/optional.hpp>
#include <map>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/shard_id.h"
#include "mongo/util/string_map.h"

namespace mongo {

/**
 * Keeps track of the transaction state. A session is in use when it is being used by a request.
 */
class TransactionRouter {
public:
    // The default value to use as the statement id of the first command in the transaction if none
    // was sent.
    static const StmtId kDefaultFirstStmtId = 0;

    /**
     * Represents the options for a transaction that are shared across all participants. These
     * cannot be changed without restarting the transactions that may have already been begun on
     * every participant, i.e. clearing the current participant list.
     */
    struct SharedTransactionOptions {
        // Set for all distributed transactions.
        TxnNumber txnNumber;
        repl::ReadConcernArgs readConcernArgs;

        // Only set for transactions with snapshot level read concern.
        boost::optional<LogicalTime> atClusterTime;
    };

    /**
     * Represents a shard participant in a distributed transaction. Lives only for the duration of
     * the transaction that created it.
     */
    struct Participant {
        Participant(bool isCoordinator,
                    StmtId stmtIdCreatedAt,
                    SharedTransactionOptions sharedOptions);

        /**
         * Attaches necessary fields if this is participating in a multi statement transaction.
         */
        BSONObj attachTxnFieldsIfNeeded(BSONObj cmd, bool isFirstStatementInThisParticipant) const;

        // True if the participant has been chosen as the coordinator for its transaction
        const bool isCoordinator{false};

        // The highest statement id of the request during which this participant was created.
        const StmtId stmtIdCreatedAt{kUninitializedStmtId};

        // Returns the shared transaction options this participant was created with
        const SharedTransactionOptions sharedOptions;
    };

    enum class TransactionActions { kStart, kContinue, kCommit };

    /**
     * Encapsulates the logic around selecting a global read timestamp for a sharded transaction at
     * snapshot level read concern.
     *
     * The first command in a transaction to target at least one shard must select a cluster time
     * timestamp before targeting, but may change the timestamp before contacting any shards to
     * allow optimizing the timestamp based on the targeted shards. If the first command encounters
     * a retryable error, e.g. StaleShardVersion or SnapshotTooOld, the retry may also select a new
     * timestamp. Once the first command has successfully completed, the timestamp cannot be
     * changed.
     */
    class AtClusterTime {
    public:
        /**
         * Cannot be called until a timestamp has been set.
         */
        LogicalTime getTime() const;

        /**
         * Sets the timestamp and remembers the statement id of the command that set it.
         */
        void setTime(LogicalTime atClusterTime, StmtId currentStmtId);

        /**
         * True if the timestamp can be changed by a command running at the given statement id.
         */
        bool canChange(StmtId currentStmtId) const;

    private:
        StmtId _stmtIdSelectedAt = kUninitializedStmtId;
        LogicalTime _atClusterTime;
    };

    TransactionRouter();
    ~TransactionRouter();

    /**
     * Extract the runtime state attached to the operation context. Returns nullptr if none is
     * attached.
     */
    static TransactionRouter* get(OperationContext* opCtx);

    /**
     * Starts a fresh transaction in this session or continue an existing one. Also cleans up the
     * previous transaction state.
     */
    void beginOrContinueTxn(OperationContext* opCtx,
                            TxnNumber txnNumber,
                            TransactionActions action);

    /**
     * Attaches the required transaction related fields for a request to be sent to the given
     * shard.
     *
     * Calling this method has the following side effects:
     * 1. Potentially selecting a coordinator.
     * 2. Adding the shard to the list of participants.
     * 3. Also append fields for first statements (ex. startTransaction, readConcern)
     *    if the shard was newly added to the list of participants.
     */
    BSONObj attachTxnFieldsIfNeeded(const ShardId& shardId, const BSONObj& cmdObj);

    /**
     * Returns true if the current transaction can retry on a stale version error from a contacted
     * shard. This is always true except for an error received by a write that is not the first
     * overall statement in the sharded transaction. This is because the entire command will be
     * retried, and shards that were not stale and are targeted again may incorrectly execute the
     * command a second time.
     *
     * Note: Even if this method returns true, the retry attempt may still fail, e.g. if one of the
     * shards that returned a stale version error was involved in a previously completed a statement
     * for this transaction.
     *
     * TODO SERVER-37207: Change batch writes to retry only the failed writes in a batch, to allow
     * retrying writes beyond the first overall statement.
     */
    bool canContinueOnStaleShardOrDbError(StringData cmdName) const;

    /**
     * Updates the transaction state to allow for a retry of the current command on a stale version
     * error. This includes sending abortTransaction to all cleared participants. Will throw if the
     * transaction cannot be continued.
     */
    void onStaleShardOrDbError(OperationContext* opCtx,
                               StringData cmdName,
                               const Status& errorStatus);

    /**
     * Returns true if the current transaction can retry on a snapshot error. This is only true on
     * the first command recevied for a transaction.
     */
    bool canContinueOnSnapshotError() const;

    /**
     * Resets the transaction state to allow for a retry attempt. This includes clearing all
     * participants, clearing the coordinator, resetting the global read timestamp, and sending
     * abortTransaction to all cleared participants. Will throw if the transaction cannot be
     * continued.
     */
    void onSnapshotError(OperationContext* opCtx, const Status& errorStatus);

    /**
     * Updates the transaction tracking state to allow for a retry attempt on a view resolution
     * error. This includes sending abortTransaction to all cleared participants.
     */
    void onViewResolutionError(OperationContext* opCtx, const NamespaceString& nss);

    /**
     * Sets the atClusterTime for the current transaction to the latest time in the router's logical
     * clock. Does nothing if the transaction does not have snapshot read concern or an
     * atClusterTime has already been selected and cannot be changed.
     */
    void setDefaultAtClusterTime(OperationContext* opCtx);

    /**
     * Returns the global read timestamp for this transaction. Returns boost::none for transactions
     * that don't run at snapshot level read concern or if a timestamp has not yet been selected.
     */
    const boost::optional<AtClusterTime>& getAtClusterTime() const;

    /**
     * If a coordinator has been selected for the current transaction, returns its identifier
     */
    const boost::optional<ShardId>& getCoordinatorId() const;

    /**
     * Commits the transaction. For transactions with multiple participants, this will initiate
     * the two phase commit procedure.
     */
    Shard::CommandResponse commitTransaction(
        OperationContext* opCtx, const boost::optional<TxnRecoveryToken>& recoveryToken);

    /**
     * Sends abort to all participants and returns the responses from all shards.
     */
    std::vector<AsyncRequestsSender::Response> abortTransaction(OperationContext* opCtx,
                                                                bool isImplicit = false);

    /**
     * Sends abort to all shards in the current participant list. Will retry on retryable errors,
     * but ignores the responses from each shard.
     */
    void implicitlyAbortTransaction(OperationContext* opCtx, const Status& errorStatus);

    /**
     * Returns the participant for this transaction or nullptr if the specified shard is not
     * participant of this transaction.
     */
    Participant* getParticipant(const ShardId& shard);

    /**
     * If a coordinator has been selected for this transaction already, constructs a recovery token,
     * which can be used to resume commit or abort of the transaction from a different router.
     */
    void appendRecoveryToken(BSONObjBuilder* builder) const;

    /**
     * Returns a string with the active transaction's transaction number and logical session id
     * (i.e. the transaction id).
     */
    std::string txnIdToString() const;

    /**
     * Returns the statement id of the latest received command for this transaction.
     */
    StmtId getLatestStmtId() const {
        return _latestStmtId;
    }

private:
    // Shortcut to obtain the id of the session under which this transaction router runs
    const LogicalSessionId& _sessionId() const;

    /**
     * Run basic commit for transactions that touched a single shard.
     */
    Shard::CommandResponse _commitSingleShardTransaction(OperationContext* opCtx);

    Shard::CommandResponse _commitWithRecoveryToken(OperationContext* opCtx,
                                                    const TxnRecoveryToken& recoveryToken);

    /**
     * Run two phase commit for transactions that touched multiple shards.
     */
    Shard::CommandResponse _commitMultiShardTransaction(OperationContext* opCtx);

    /**
     * Sets the given logical time as the atClusterTime for the transaction to be the greater of the
     * given time and the user's afterClusterTime, if one was provided.
     */
    void _setAtClusterTime(const boost::optional<LogicalTime>& afterClusterTime,
                           LogicalTime candidateTime);

    /**
     * Throws NoSuchTransaction if the response from abortTransaction failed with a code other than
     * NoSuchTransaction. Does not check for write concern errors.
     */
    void _assertAbortStatusIsOkOrNoSuchTransaction(
        const AsyncRequestsSender::Response& response) const;

    /**
     * Returns all participants created during the current statement.
     */
    std::vector<ShardId> _getPendingParticipants() const;

    /**
     * Removes all participants created during the current statement from the participant list and
     * sends abortTransaction to each. Waits for all responses before returning.
     */
    void _clearPendingParticipants(OperationContext* opCtx);

    /**
     * Creates a new participant for the shard.
     */
    Participant& _createParticipant(const ShardId& shard);

    /**
     * If the transaction's read concern level is snapshot, asserts the participant's atClusterTime
     * matches the transaction's.
     */
    void _verifyParticipantAtClusterTime(const Participant& participant);

    // The currently active transaction number on this router, if beginOrContinueTxn has been
    // called. Otherwise set to kUninitializedTxnNumber.
    TxnNumber _txnNumber{kUninitializedTxnNumber};

    // Whether the router has initiated a two-phase commit by handing off commit coordination to the
    // coordinator. If so, the router should no longer implicitly abort the transaction on errors,
    // since the coordinator may independently make a commit decision.
    bool _initiatedTwoPhaseCommit{false};

    // Indicates whether this is trying to recover a commitTransaction on the current transaction.
    bool _isRecoveringCommit{false};

    // Map of current participants of the current transaction.
    StringMap<Participant> _participants;

    // The id of coordinator participant, used to construct prepare requests.
    boost::optional<ShardId> _coordinatorId;

    // The read concern the current transaction was started with.
    repl::ReadConcernArgs _readConcernArgs;

    // The cluster time of the timestamp all participant shards in the current transaction with
    // snapshot level read concern must read from. Only set for transactions running with snapshot
    // level read concern.
    boost::optional<AtClusterTime> _atClusterTime;

    // The statement id of the latest received command for this transaction. For batch writes, this
    // will be the highest stmtId contained in the batch. Incremented by one if new commands do not
    // contain statement ids.
    StmtId _latestStmtId = kUninitializedStmtId;

    // The statement id of the command that began this transaction. Defaults to zero if no statement
    // id was included in the first command.
    StmtId _firstStmtId = kUninitializedStmtId;
};

}  // namespace mongo
