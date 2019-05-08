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
        enum class ReadOnly { kUnset, kReadOnly, kNotReadOnly };

        Participant(bool isCoordinator,
                    StmtId stmtIdCreatedAt,
                    SharedTransactionOptions sharedOptions);

        /**
         * Attaches necessary fields if this is participating in a multi statement transaction.
         */
        BSONObj attachTxnFieldsIfNeeded(BSONObj cmd, bool isFirstStatementInThisParticipant) const;

        // True if the participant has been chosen as the coordinator for its transaction
        const bool isCoordinator{false};

        // Is updated to kReadOnly or kNotReadOnly based on the readOnly field in the participant's
        // responses to statements.
        ReadOnly readOnly{ReadOnly::kUnset};

        // The highest statement id of the request during which this participant was created.
        const StmtId stmtIdCreatedAt;

        // Returns the shared transaction options this participant was created with
        const SharedTransactionOptions sharedOptions;
    };

    // Container for timing stats for the current transaction. Includes helpers for calculating some
    // metrics like transaction duration.
    struct TimingStats {
        /**
         * Returns the duration of the transaction. The transaction start time must have been set
         * before this can be called.
         */
        Microseconds getDuration(TickSource* tickSource, TickSource::Tick curTicks) const;

        /**
         * Returns the duration of commit. The commit start time must have been set before this can
         * be called.
         */
        Microseconds getCommitDuration(TickSource* tickSource, TickSource::Tick curTicks) const;

        // The start time of the transaction. Note that tick values should only ever be used to
        // measure distance from other tick values, not for reporting absolute wall clock time.
        TickSource::Tick startTime{0};

        // When commit was started.
        TickSource::Tick commitStartTime{0};

        // The end time of the transaction.
        TickSource::Tick endTime{0};
    };

    enum class TransactionActions { kStart, kContinue, kCommit };

    // Reason a transaction terminated.
    enum class TerminationCause {
        kCommitted,
        kAborted,
    };

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
        boost::optional<StmtId> _stmtIdSelectedAt;
        LogicalTime _atClusterTime;
    };

    TransactionRouter();
    TransactionRouter(const TransactionRouter&) = delete;
    TransactionRouter& operator=(const TransactionRouter&) = delete;
    TransactionRouter(TransactionRouter&&) = delete;
    TransactionRouter& operator=(TransactionRouter&&) = delete;
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
     * Processes the transaction metadata in the response from the participant if the response
     * indicates the operation succeeded.
     */
    void processParticipantResponse(const ShardId& shardId, const BSONObj& responseObj);

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
     * If a coordinator has been selected for the current transaction, returns its id.
     */
    const boost::optional<ShardId>& getCoordinatorId() const;

    /**
     * If a recovery shard has been selected for the current transaction, returns its id.
     */
    const boost::optional<ShardId>& getRecoveryShardId() const;

    /**
     * Commits the transaction.
     *
     * For transactions that only did reads or only wrote to one shard, sends commit directly to the
     * participants and returns the first error response or the last (success) response.
     *
     * For transactions that performed writes to multiple shards, hands off the participant list to
     * the coordinator to do two-phase commit, and returns the coordinator's response.
     */
    BSONObj commitTransaction(OperationContext* opCtx,
                              const boost::optional<TxnRecoveryToken>& recoveryToken);

    /**
     * Sends abort to all participants.
     *
     * Returns the first error response or the last (success) response.
     */
    BSONObj abortTransaction(OperationContext* opCtx);

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

    /**
     * Returns a copy of the timing stats of the transaction router's active transaction.
     */
    TimingStats getTimingStats() const {
        return _timingStats;
    }

private:
    // The type of commit initiated for this transaction.
    enum class CommitType {
        kNotInitiated,
        kNoShards,
        kSingleShard,
        kSingleWriteShard,
        kReadOnly,
        kTwoPhaseCommit,
        kRecoverWithToken,
    };

    // Helper to convert the CommitType enum into a human readable string for diagnostics.
    std::string _commitTypeToString(CommitType state) const {
        switch (state) {
            case CommitType::kNotInitiated:
                return "notInitiated";
            case CommitType::kNoShards:
                return "noShards";
            case CommitType::kSingleShard:
                return "singleShard";
            case CommitType::kSingleWriteShard:
                return "singleWriteShard";
            case CommitType::kReadOnly:
                return "readOnly";
            case CommitType::kTwoPhaseCommit:
                return "twoPhaseCommit";
            case CommitType::kRecoverWithToken:
                return "recoverWithToken";
        }
        MONGO_UNREACHABLE;
    }

    /**
     * Prints slow transaction information to the log.
     */
    void _logSlowTransaction(OperationContext* opCtx, TerminationCause terminationCause) const;

    /**
     * Returns a string to be logged for slow transactions.
     */
    std::string _transactionInfoForLog(OperationContext* opCtx,
                                       TerminationCause terminationCause) const;

    // Shortcut to obtain the id of the session under which this transaction router runs
    const LogicalSessionId& _sessionId() const;

    /**
     * Resets the router's state. Used when the router sees a new transaction for the first time.
     * This is required because we don't create a new router object for each transaction, but
     * instead reuse the same object across different transactions.
     */
    void _resetRouterState(const TxnNumber& txnNumber);

    /**
     * Internal method for committing a transaction. Should only throw on failure to send commit.
     */
    BSONObj _commitTransaction(OperationContext* opCtx,
                               const boost::optional<TxnRecoveryToken>& recoveryToken);

    /**
     * Retrieves the transaction's outcome from the shard specified in the recovery token.
     */
    BSONObj _commitWithRecoveryToken(OperationContext* opCtx,
                                     const TxnRecoveryToken& recoveryToken);

    /**
     * Hands off coordinating a two-phase commit across all participants to the coordinator shard.
     */
    BSONObj _handOffCommitToCoordinator(OperationContext* opCtx);

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

    /**
     * Updates relevant metrics when a new transaction is begun.
     */
    void _onNewTransaction(OperationContext* opCtx);

    /**
     * Updates relevant metrics when a router receives commit for a higher txnNumber than it has
     * seen so far.
     */
    void _onBeginRecoveringDecision(OperationContext* opCtx);

    /**
     * Updates relevant metrics when the router receives an explicit abort from the client.
     */
    void _onExplicitAbort(OperationContext* opCtx);

    /**
     * Updates relevant metrics when the router begins an implicit abort after an error.
     */
    void _onImplicitAbort(OperationContext* opCtx, const Status& errorStatus);

    /**
     * Updates relevant metrics when a transaction is about to begin commit.
     */
    void _onStartCommit(OperationContext* opCtx);

    /**
     * Updates relevant metrics when a transaction receives a successful response for commit.
     */
    void _onSuccessfulCommit(OperationContext* opCtx);

    /**
     * Updates relevant metrics when commit receives a response with a non-retryable command error
     * per the retryable writes specification.
     */
    void _onNonRetryableCommitError(OperationContext* opCtx, Status commitStatus);

    /**
     * The first time this method is called it marks the transaction as over in the router's
     * diagnostics and will log transaction information if its duration is over the global slowMS
     * threshold or the transaction log componenet verbosity >= 1. Only meant to be called when the
     * router definitively knows the transaction's outcome, e.g. it should not be invoked after a
     * network error on commit.
     */
    void _endTransactionTrackingIfNecessary(OperationContext* opCtx,
                                            TerminationCause terminationCause);

    // The currently active transaction number on this router, if beginOrContinueTxn has been
    // called. Otherwise set to kUninitializedTxnNumber.
    TxnNumber _txnNumber{kUninitializedTxnNumber};

    // Is updated at commit time to reflect which commit path was taken.
    CommitType _commitType{CommitType::kNotInitiated};

    // Indicates whether this is trying to recover a commitTransaction on the current transaction.
    bool _isRecoveringCommit{false};

    // Map of current participants of the current transaction.
    StringMap<Participant> _participants;

    // The id of participant chosen as the two-phase commit coordinator. If, at commit time,
    // two-phase commit is required, the participant list is handed off to this shard. Is unset
    // until the transaction has targeted a participant, and is set to the first participant
    // targeted. Is reset if the first participant targeted returns a "needs retargeting" error.
    boost::optional<ShardId> _coordinatorId;

    // The id of the recovery participant. Passed in the recoveryToken that is included on responses
    // to the client. Is unset until the transaction has done a write, and is set to the first
    // participant that reports having done a write. Is reset if that participant is removed from
    // the participant list because another participant targeted in the same statement returned a
    // "needs retargeting" error.
    boost::optional<ShardId> _recoveryShardId;

    // The read concern the current transaction was started with.
    repl::ReadConcernArgs _readConcernArgs;

    // The cluster time of the timestamp all participant shards in the current transaction with
    // snapshot level read concern must read from. Only set for transactions running with snapshot
    // level read concern.
    boost::optional<AtClusterTime> _atClusterTime;

    // The statement id of the latest received command for this transaction. For batch writes, this
    // will be the highest stmtId contained in the batch. Incremented by one if new commands do not
    // contain statement ids.
    StmtId _latestStmtId{kDefaultFirstStmtId};

    // The statement id of the command that began this transaction. Defaults to zero if no statement
    // id was included in the first command.
    StmtId _firstStmtId{kDefaultFirstStmtId};

    // String representing the reason a transaction aborted. Either the string name of the error
    // code that led to an implicit abort or "abort" if the client sent abortTransaction.
    std::string _abortCause;

    // Stats used for calculating durations for the active transaction.
    TimingStats _timingStats;
};

}  // namespace mongo
