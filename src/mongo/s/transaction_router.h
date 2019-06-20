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
#include "mongo/db/session_catalog.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/shard_id.h"
#include "mongo/util/string_map.h"

namespace mongo {

/**
 * Keeps track of the transaction state. A session is in use when it is being used by a request.
 */
class TransactionRouter {
    struct PrivateState;
    struct ObservableState;

public:
    TransactionRouter();
    TransactionRouter(const TransactionRouter&) = delete;
    TransactionRouter& operator=(const TransactionRouter&) = delete;
    TransactionRouter(TransactionRouter&&) = delete;
    TransactionRouter& operator=(TransactionRouter&&) = delete;
    ~TransactionRouter();

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
                    ReadOnly inReadOnly,
                    SharedTransactionOptions sharedOptions);

        /**
         * Attaches necessary fields if this is participating in a multi statement transaction.
         */
        BSONObj attachTxnFieldsIfNeeded(BSONObj cmd, bool isFirstStatementInThisParticipant) const;

        // True if the participant has been chosen as the coordinator for its transaction
        const bool isCoordinator{false};

        // Is updated to kReadOnly or kNotReadOnly based on the readOnly field in the participant's
        // responses to statements.
        const ReadOnly readOnly{ReadOnly::kUnset};

        // Returns the shared transaction options this participant was created with
        const SharedTransactionOptions sharedOptions;

        // The highest statement id of the request during which this participant was created.
        const StmtId stmtIdCreatedAt;
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

    /**
     * Class used by observers to examine the state of a TransactionRouter.
     */
    class Observer {
    public:
        explicit Observer(const ObservableSession& session);

    protected:
        explicit Observer(TransactionRouter* tr) : _tr(tr) {}

        const TransactionRouter::ObservableState& o() const {
            return _tr->_o;
        }

        TransactionRouter* _tr;
    };  // class Observer

    /**
     * Class used by a thread that has checked out the TransactionRouter's session to observe
     * and modify the transaction router.
     */
    class Router : public Observer {
    public:
        explicit Router(OperationContext* opCtx);

        explicit operator bool() const {
            return _tr;
        }

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
        BSONObj attachTxnFieldsIfNeeded(OperationContext* opCtx,
                                        const ShardId& shardId,
                                        const BSONObj& cmdObj);

        /**
        * Processes the transaction metadata in the response from the participant if the response
        * indicates the operation succeeded.
        */
        void processParticipantResponse(OperationContext* opCtx,
                                        const ShardId& shardId,
                                        const BSONObj& responseObj);

        /**
        * Returns true if the current transaction can retry on a stale version error from a
        * contacted shard. This is always true except for an error received by a write that is not
        * the first overall statement in the sharded transaction. This is because the entire
        * command will be retried, and shards that were not stale and are targeted again may
        * incorrectly execute the command a second time.
        *
        * Note: Even if this method returns true, the retry attempt may still fail, e.g. if one of
        * the shards that returned a stale version error was involved in a previously completed a
        * statement for this transaction.
        *
        * TODO SERVER-37207: Change batch writes to retry only the failed writes in a batch, to
        * allow retrying writes beyond the first overall statement.
        */
        bool canContinueOnStaleShardOrDbError(StringData cmdName) const;

        /**
        * Updates the transaction state to allow for a retry of the current command on a stale
        * version error. This includes sending abortTransaction to all cleared participants. Will
        * throw if the transaction cannot be continued.
        */
        void onStaleShardOrDbError(OperationContext* opCtx,
                                   StringData cmdName,
                                   const Status& errorStatus);

        /**
        * Returns true if the current transaction can retry on a snapshot error. This is only true
        * on the first command recevied for a transaction.
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
         * Returns true if the associated transaction is running at snapshot level read concern.
         */
        bool mustUseAtClusterTime() const;

        /**
         * Returns the read timestamp for this transaction. Callers must verify that the read
         * timestamp has been selected for this transaction before calling this function.
         */
        LogicalTime getSelectedAtClusterTime() const;

        /**
        * Sets the atClusterTime for the current transaction to the latest time in the router's
        * logical clock. Does nothing if the transaction does not have snapshot read concern or an
        * atClusterTime has already been selected and cannot be changed.
        */
        void setDefaultAtClusterTime(OperationContext* opCtx);

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
        * For transactions that only did reads or only wrote to one shard, sends commit directly to
        * the participants and returns the first error response or the last (success) response.
        *
        * For transactions that performed writes to multiple shards, hands off the participant list
        * to the coordinator to do two-phase commit, and returns the coordinator's response.
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
        * If a coordinator has been selected for this transaction already, constructs a recovery
        * token, which can be used to resume commit or abort of the transaction from a different
        * router.
        */
        void appendRecoveryToken(BSONObjBuilder* builder) const;

        /**
        * Returns a string with the active transaction's transaction number and logical session id
        * (i.e. the transaction id).
        */
        std::string txnIdToString() const;

        /**
        * Returns the participant for this transaction or nullptr if the specified shard is not
        * participant of this transaction.
        */
        const Participant* getParticipant(const ShardId& shard);

        /**
        * Returns the statement id of the latest received command for this transaction.
        */
        StmtId getLatestStmtId() const {
            return p().latestStmtId;
        }

        /**
        * Returns a copy of the timing stats of the transaction router's active transaction.
        */
        const TimingStats& getTimingStats() const {
            return o().timingStats;
        }

    private:
        /**
        * Resets the router's state. Used when the router sees a new transaction for the first time.
        * This is required because we don't create a new router object for each transaction, but
        * instead reuse the same object across different transactions.
        */
        void _resetRouterState(OperationContext* opCtx, const TxnNumber& txnNumber);

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
        * Hands off coordinating a two-phase commit across all participants to the coordinator
        * shard.
        */
        BSONObj _handOffCommitToCoordinator(OperationContext* opCtx);

        /**
        * Sets the given logical time as the atClusterTime for the transaction to be the greater of
        * the given time and the user's afterClusterTime, if one was provided.
        */
        void _setAtClusterTime(OperationContext* opCtx,
                               const boost::optional<LogicalTime>& afterClusterTime,
                               LogicalTime candidateTime);

        /**
        * Throws NoSuchTransaction if the response from abortTransaction failed with a code other
        * than NoSuchTransaction. Does not check for write concern errors.
        */
        void _assertAbortStatusIsOkOrNoSuchTransaction(
            const AsyncRequestsSender::Response& response) const;

        /**
        * If the transaction's read concern level is snapshot, asserts the participant's
        * atClusterTime matches the transaction's.
        */
        void _verifyParticipantAtClusterTime(const Participant& participant);

        /**
        * Removes all participants created during the current statement from the participant list
        * and sends abortTransaction to each. Waits for all responses before returning.
        */
        void _clearPendingParticipants(OperationContext* opCtx);

        /**
        * Creates a new participant for the shard.
        */
        TransactionRouter::Participant& _createParticipant(OperationContext* opCtx,
                                                           const ShardId& shard);

        /**
        * Sets the new readOnly value for the current participant on the shard.
        */
        void _setReadOnlyForParticipant(OperationContext* opCtx,
                                        const ShardId& shard,
                                        const Participant::ReadOnly readOnly);

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
        void _onStartCommit(WithLock wl, OperationContext* opCtx);

        /**
        * Updates relevant metrics when a transaction receives a successful response for commit.
        */
        void _onSuccessfulCommit(OperationContext* opCtx);

        /**
        * Updates relevant metrics when commit receives a response with a non-retryable command
        * error per the retryable writes specification.
        */
        void _onNonRetryableCommitError(OperationContext* opCtx, Status commitStatus);

        /**
        * The first time this method is called it marks the transaction as over in the router's
        * diagnostics and will log transaction information if its duration is over the global slowMS
        * threshold or the transaction log componenet verbosity >= 1. Only meant to be called when
        * the router definitively knows the transaction's outcome, e.g. it should not be invoked
        * after a network error on commit.
        */
        void _endTransactionTrackingIfNecessary(OperationContext* opCtx,
                                                TerminationCause terminationCause);

        /**
        * Returns all participants created during the current statement.
        */
        std::vector<ShardId> _getPendingParticipants() const;

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

        TransactionRouter::PrivateState& p() {
            return _tr->_p;
        }
        const TransactionRouter::PrivateState& p() const {
            return _tr->_p;
        }

        TransactionRouter::ObservableState& o(WithLock) {
            return _tr->_o;
        }
        using Observer::o;
    };  // class Router

    static Router get(OperationContext* opCtx) {
        return Router(opCtx);
    }

    static Observer get(const ObservableSession& osession) {
        return Observer(osession);
    }

private:
    /**
     * State in this struct may be read by methods of Observer or Router, and may be written by
     * methods of Router when they acquire the lock on the opCtx's Client. Access this inside
     * Observer and Router using the private o() method for reading and (Router only) the
     * o(WithLock) method for writing.
     */
    struct ObservableState {
        // The currently active transaction number on this router, if beginOrContinueTxn has been
        // called. Otherwise set to kUninitializedTxnNumber.
        TxnNumber txnNumber{kUninitializedTxnNumber};

        // Is updated at commit time to reflect which commit path was taken.
        CommitType commitType{CommitType::kNotInitiated};

        // Map of current participants of the current transaction.
        StringMap<Participant> participants;

        // The id of participant chosen as the two-phase commit coordinator. If, at commit time,
        // two-phase commit is required, the participant list is handed off to this shard. Is unset
        // until the transaction has targeted a participant, and is set to the first participant
        // targeted. Is reset if the first participant targeted returns a "needs retargeting" error.
        boost::optional<ShardId> coordinatorId;

        // The read concern the current transaction was started with.
        repl::ReadConcernArgs readConcernArgs;

        // The cluster time of the timestamp all participant shards in the current transaction with
        // snapshot level read concern must read from. Only set for transactions running with
        // snapshot level read concern.
        boost::optional<AtClusterTime> atClusterTime;

        // String representing the reason a transaction aborted. Either the string name of the error
        // code that led to an implicit abort or "abort" if the client sent abortTransaction.
        std::string abortCause;

        // Stats used for calculating durations for the active transaction.
        TimingStats timingStats;
    } _o;

    /**
     * State in this struct may be read and written by methods of the Router, only. It may
     * access the struct via the private p() accessor. No further locking is required in methods of
     * the Router.
     */
    struct PrivateState {
        // Indicates whether this is trying to recover a commitTransaction on the current
        // transaction.
        bool isRecoveringCommit{false};

        // The id of the recovery participant. Passed in the recoveryToken that is included on
        // responses to the client. Is unset until the transaction has done a write, and is set to
        // the first participant that reports having done a write. Is reset if that participant is
        // removed from the participant list because another participant targeted in the same
        // statement returned a "needs retargeting" error.
        boost::optional<ShardId> recoveryShardId;

        // The statement id of the latest received command for this transaction. For batch writes,
        // this will be the highest stmtId contained in the batch. Incremented by one if new
        // commands do not contain statement ids.
        StmtId latestStmtId{kDefaultFirstStmtId};

        // The statement id of the command that began this transaction. Defaults to zero if no
        // statement id was included in the first command.
        StmtId firstStmtId{kDefaultFirstStmtId};

        // Track whether commit or abort have been initiated.
        bool terminationInitiated{false};
    } _p;
};

}  // namespace mongo
