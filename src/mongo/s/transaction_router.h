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

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/session/session_catalog.h"
#include "mongo/db/shard_id.h"
#include "mongo/db/stats/single_transaction_stats.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/client/shard.h"
#include "mongo/util/assert_util_core.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/duration.h"
#include "mongo/util/string_map.h"
#include "mongo/util/tick_source.h"
#include "mongo/util/time_support.h"

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

    // The reason why TransactionRouter::Router::stash() is called.
    enum class StashReason { kDone, kYield };

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
        TxnNumberAndRetryCounter txnNumberAndRetryCounter;
        APIParameters apiParameters;
        repl::ReadConcernArgs readConcernArgs;

        // Only set for transactions with snapshot level read concern.
        boost::optional<LogicalTime> atClusterTimeForSnapshotReadConcern;

        boost::optional<LogicalTime> placementConflictTimeForNonSnapshotReadConcern;

        bool isInternalTransactionForRetryableWrite;
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
        BSONObj attachTxnFieldsIfNeeded(OperationContext* opCtx,
                                        BSONObj cmd,
                                        bool isFirstStatementInThisParticipant,
                                        bool addingParticipantViaSubRouter,
                                        bool hasTxnCreatedAnyDatabase) const;

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

        /**
         * Returns the total active time of the transaction, given the current time value. A
         * transaction is active when there is a running operation that is part of the transaction.
         */
        Microseconds getTimeActiveMicros(TickSource* tickSource, TickSource::Tick curTicks) const;

        /**
         * Returns the total inactive time of the transaction, given the current time value. A
         * transaction is inactive when it is idly waiting for a new operation to occur.
         */
        Microseconds getTimeInactiveMicros(TickSource* tickSource, TickSource::Tick curTicks) const;

        // The start time of the transaction in millisecond resolution. Used only for diagnostics
        // reporting.
        Date_t startWallClockTime;

        // The start time of the transaction. Note that tick values should only ever be used to
        // measure distance from other tick values, not for reporting absolute wall clock time. A
        // value of zero means the transaction hasn't started yet.
        TickSource::Tick startTime{0};

        // The start time of the transaction commit in millisecond resolution. Used only for
        // diagnostics reporting.
        Date_t commitStartWallClockTime;

        // When commit was started. A value of zero means the commit hasn't started yet.
        TickSource::Tick commitStartTime{0};

        // The end time of the transaction. A value of zero means the transaction hasn't ended yet.
        TickSource::Tick endTime{0};

        // The total amount of active time spent by the transaction.
        Microseconds timeActiveMicros = Microseconds{0};

        // The time at which the transaction was last marked as active. The transaction is
        // considered active if this value is not equal to 0.
        TickSource::Tick lastTimeActiveStart{0};
    };

    enum class TransactionActions { kStart, kContinue, kStartOrContinue, kCommit };

    // Reason a transaction terminated.
    enum class TerminationCause {
        kCommitted,
        kAborted,
    };

    /**
     * Helper class responsible for updating per transaction and router wide transaction metrics on
     * certain transaction events.
     */
    class MetricsTracker {
    public:
        MetricsTracker(ServiceContext* service) : _service(service) {}
        MetricsTracker(const MetricsTracker&) = delete;
        MetricsTracker& operator=(const MetricsTracker&) = delete;
        MetricsTracker(MetricsTracker&&) = delete;
        MetricsTracker& operator=(MetricsTracker&&) = delete;
        ~MetricsTracker();

        bool isTrackingOver() const {
            return timingStats.endTime != 0;
        }

        bool hasStarted() const {
            return timingStats.startTime != 0;
        }

        bool isActive() const {
            return timingStats.lastTimeActiveStart != 0;
        }

        bool commitHasStarted() const {
            return timingStats.commitStartTime != 0;
        }

        const auto& getTimingStats() const {
            return timingStats;
        }

        /**
         * Marks the transaction as active and sets the start of the transaction's active time and
         * overall start time the first time it is called.
         *
         * This method is a no-op if the transaction is not currently inactive or has already ended.
         */
        void trySetActive(TickSource* tickSource, TickSource::Tick curTicks);

        /**
         * Marks the transaction as inactive, sets the total active time of the transaction, and
         * updates relevant server status counters.
         *
         * This method is a no-op if the transaction is not currently active or has already ended.
         */
        void trySetInactive(TickSource* tickSource, TickSource::Tick curTicks);

        /**
         * Marks the transaction as having begun commit, updating relevent stats. Assumes the
         * transaction is currently active.
         */
        void startCommit(TickSource* tickSource,
                         TickSource::Tick curTicks,
                         TransactionRouter::CommitType commitType,
                         std::size_t numParticipantsAtCommit);

        /**
         * Marks the transaction as over, updating stats based on the termination cause, which is
         * either commit or abort.
         */
        void endTransaction(TickSource* tickSource,
                            TickSource::Tick curTicks,
                            TransactionRouter::TerminationCause terminationCause,
                            TransactionRouter::CommitType commitType,
                            StringData abortCause);

    private:
        // Pointer to the service context used to get the tick source and router wide transaction
        // metrics decorations.
        ServiceContext* const _service;

        // Stats used for calculating durations for the active transaction.
        TransactionRouter::TimingStats timingStats;
    };

    /**
     * Encapsulates the logic around selecting a global read timestamp for a sharded transaction at
     * snapshot level read concern.
     *
     * The first command in a transaction to target at least one shard must select a cluster time
     * timestamp before targeting, but may change the timestamp before contacting any shards to
     * allow optimizing the timestamp based on the targeted shards. If the first command encounters
     * a retryable error, e.g. "retargeting needed" or SnapshotTooOld, the retry may also select a
     * new timestamp. Once the first command has successfully completed, the timestamp cannot be
     * changed.
     */
    class AtClusterTime {
    public:
        /**
         * Cannot be called until a timestamp has been set.
         */
        LogicalTime getTime() const;

        /**
         * Returns true if the _atClusterTime has been changed from the default uninitialized value.
         */
        bool timeHasBeenSet() const;

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

        /**
         * Report the current state of an session. The sessionIsActive boolean indicates whether
         * the session and transaction are currently active.
         *
         * The Client lock for the given OperationContext must be held when calling this method in
         * the case where sessionIsActive is true.
         */
        BSONObj reportState(OperationContext* opCtx, bool sessionIsActive) const;
        void reportState(OperationContext* opCtx,
                         BSONObjBuilder* builder,
                         bool sessionIsActive) const;

        /**
         * Returns if the router has received at least one request for a transaction.
         */
        auto isInitialized() const {
            return o().txnNumberAndRetryCounter.getTxnNumber() != kUninitializedTxnNumber;
        }

        /**
         * Returns if this TransactionRouter instance can be reaped. Always true unless an operation
         * has yielded the router and has not unyielded yet. We cannot reap the instance in that
         * case or the unyield would check out a different TransactionRouter than it yielded.
         */
        auto canBeReaped() const {
            return o().activeYields == 0;
        }

    protected:
        explicit Observer(TransactionRouter* tr) : _tr(tr) {}

        const TransactionRouter::ObservableState& o() const {
            return _tr->_o;
        }

        // Reports the current state of the session using the provided builder.
        void _reportState(OperationContext* opCtx,
                          BSONObjBuilder* builder,
                          bool sessionIsActive) const;

        // Reports the 'transaction' state of this transaction for currentOp using the provided
        // builder.
        void _reportTransactionState(OperationContext* opCtx, BSONObjBuilder* builder) const;

        // Returns true if the atClusterTime has been changed from the default uninitialized value.
        bool _atClusterTimeHasBeenSet() const;

        // Shortcut to obtain the id of the session under which this transaction router runs
        const LogicalSessionId& _sessionId() const;

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
         * Starts a fresh transaction in this session or continue an existing one. Also cleans up
         * the previous transaction state.
         */
        void beginOrContinueTxn(OperationContext* opCtx,
                                TxnNumber txnNumber,
                                TransactionActions action);

        /**
         * Updates transaction diagnostics and, if necessary, the number of active yielders when the
         * transaction's session is checked in.
         */
        void stash(OperationContext* opCtx, StashReason reason);

        /**
         * Validates transaction state is still compatible after a yield.
         */
        void unstash(OperationContext* opCtx);

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
        bool canContinueOnStaleShardOrDbError(StringData cmdName, const Status& status) const;

        /**
         * Updates the transaction state to allow for a retry of the current command on a stale
         * version error. This includes sending abortTransaction to all cleared participants. Will
         * throw if the transaction cannot be continued.
         */
        void onStaleShardOrDbError(OperationContext* opCtx,
                                   StringData cmdName,
                                   const Status& status);

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
        void onSnapshotError(OperationContext* opCtx, const Status& status);

        /**
         * Updates the transaction tracking state to allow for a retry attempt on a view resolution
         * error. This includes sending abortTransaction to all cleared participants.
         */
        void onViewResolutionError(OperationContext* opCtx, const NamespaceString& nss);

        /**
         * If the transaction is not running at a read concern snapshot, returns boost::none.
         * Otherwise returns the timestamps that has been selected for the transaction.
         */
        boost::optional<LogicalTime> getSelectedAtClusterTime() const;

        /**
         * Sets the atClusterTime for the current transaction to the latest time in the router's
         * logical clock. Does nothing if the transaction does not have snapshot read concern or an
         * atClusterTime has already been selected and cannot be changed.
         */
        void setDefaultAtClusterTime(OperationContext* opCtx);

        /**
         * If the transaction has specified a placementConflictTime returns the value, otherwise
         * returns boost::none.
         */
        boost::optional<LogicalTime> getPlacementConflictTime() const;

        /**
         * If a coordinator has been selected for the current transaction, returns its id.
         */
        const boost::optional<ShardId>& getCoordinatorId() const;

        /**
         * If a recovery shard has been selected for the current transaction, returns its id.
         */
        const boost::optional<ShardId>& getRecoveryShardId() const;

        /**
         * If this router is a sub-router and the txnNumber and retryCounter match that on the
         * opCtx, returns a map containing {participantShardId : readOnly} for each participant
         * added by this router. It's possible that readOnly is not set if either an error occured
         * before receiving a response from a particular shard, or a shard returned an error.
         *
         * Returns boost::none if this router is not a sub-router, or if the txnNumber or
         * retryCounter on this router do not match that on the opCtx.
         */
        // TODO SERVER-85353 Remove commandName and nss parameters, which are used only for the
        // failpoint
        boost::optional<StringMap<boost::optional<bool>>> getAdditionalParticipantsForResponse(
            OperationContext* opCtx,
            boost::optional<const std::string&> commandName = boost::none,
            boost::optional<const NamespaceString&> nss = boost::none);

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
         * Sends abort to all shards in the current participant list. Will retry on retryable
         * errors, but ignores the responses from each shard.
         */
        void implicitlyAbortTransaction(OperationContext* opCtx, const Status& status);

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
        const auto& getTimingStats_forTest() const {
            invariant(o().metricsTracker);
            return o().metricsTracker->getTimingStats();
        }

        /**
         * Returns if the router is not currently tracking an active transaction.
         */
        bool isTrackingOver() {
            if (o().metricsTracker)
                return o().metricsTracker->isTrackingOver();
            return true;
        }

        /**
         * Annotate that this transaction has attempted to create database 'dbName'.
         */
        void annotateCreatedDatabase(DatabaseName dbName) {
            p().createdDatabases.insert(dbName);
        }

    private:
        /**
         * Resets the router's state. Used when the router sees a new transaction for the first
         * time. This is required because we don't create a new router object for each transaction,
         * but instead reuse the same object across different transactions.
         */
        void _resetRouterState(OperationContext* opCtx,
                               const TxnNumberAndRetryCounter& txnNumberAndRetryCounter);

        /**
         * Calls _resetRouterState and then resets the read concern and the cluster time of the
         * timestamp that all participant shards in the current transaction with snapshot level read
         * concern must read from.
         */
        void _resetRouterStateForStartTransaction(
            OperationContext* opCtx, const TxnNumberAndRetryCounter& txnNumberAndRetryCounter);

        /**
         * Continues or restarts the currently active transaction.
         */
        void _continueTxn(OperationContext* opCtx,
                          TxnNumberAndRetryCounter txnNumberAndRetryCounter,
                          TransactionActions action);

        /**
         * Starts a new transaction or continues a transaction started by a different router to
         * recover the commit decision.
         */
        void _beginTxn(OperationContext* opCtx,
                       TxnNumberAndRetryCounter txnNumberAndRetryCounter,
                       TransactionActions action);

        /**
         * Internal method for committing a transaction. Should only throw on failure to send
         * commit.
         */
        BSONObj _commitTransaction(OperationContext* opCtx,
                                   const boost::optional<TxnRecoveryToken>& recoveryToken,
                                   bool isFirstCommitAttempt);

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
         * Throws NoSuchTransaction if the response from abortTransaction failed with a code other
         * than NoSuchTransaction. Does not check for write concern errors.
         */
        void _assertAbortStatusIsOkOrNoSuchTransaction(
            const AsyncRequestsSender::Response& response) const;

        /**
         * Removes all participants created during the current statement from the participant list
         * and sends abortTransaction to each if there is more than one participant and the status
         * is not stale . Waits for all responses before returning.
         */
        void _clearPendingParticipants(OperationContext* opCtx, boost::optional<Status> optStatus);

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
                                        Participant::ReadOnly readOnly);

        /**
         * Updates relevant metrics when the router receives an explicit abort from the client.
         */
        void _onExplicitAbort(OperationContext* opCtx);

        /**
         * Updates relevant metrics when the router begins an implicit abort after an error.
         */
        void _onImplicitAbort(OperationContext* opCtx, const Status& status);

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
         * Updates relevent metrics when a transaction is continued.
         */
        void _onContinue(OperationContext* opCtx);

        /**
         * The first time this method is called it marks the transaction as over in the router's
         * diagnostics and will log transaction information if its duration is over the global
         * slowMS threshold or the transaction log componenet verbosity >= 1. Only meant to be
         * called when the router definitively knows the transaction's outcome, e.g. it should not
         * be invoked after a network error on commit.
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
         * Returns the LastClientInfo object.
         */
        const SingleTransactionStats::LastClientInfo& _getLastClientInfo() const;

        /**
         * Updates the LastClientInfo object with the given Client's information.
         */
        void _updateLastClientInfo(Client* client);

        /**
         * Returns true if a status contains a stale shard or db routing error and the transaction
         * is retryable
         */
        bool _errorAllowsRetryOnStaleShardOrDb(const Status& status) const;

        /**
         * Returns true if the router is currently processing a retryable statement in a retryable
         * internal transaction.
         */
        bool _isRetryableStmtInARetryableInternalTxn(const BSONObj& cmdObj) const;

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

    /**
     * Takes a cmdObj which could have come from one of the two paths:
     *  1. Verbatim taken from the user's request (and therefore *may contain* read concern
     *     arguments) or
     *  2. Newly generated by the feature based on a user's request (and *doesn't contain* read
     *     concern arguments)
     *
     * AND outputs a new request that contains the original fields of the request along with the
     * respective readConcernArgs augmented with atClusterTimeForSnapshotReadConcern if the request
     * asks for a snapshot level.
     *
     * The 'atClusterTimeForSnapshotReadConcern' will be boost::none in all cases except when the
     * read concern level is 'snapshot' or the caller provided `atClusterTime`.
     *
     * TODO (SERVER-80526): This code re-checks that the input cmdObj is in sync with the parsed
     * readConcernArgs (i.e., that we didn't swap majority for local or snapshot somewhere along the
     * command execution path). This is very error prone and wasteful and a better architecture
     * would be if cmdObj was not allowed to contain any read concern arguments so that we can just
     * append the ones passed to the function.
     */
    static BSONObj appendFieldsForStartTransaction(
        BSONObj cmdObj,
        const repl::ReadConcernArgs& readConcernArgs,
        const boost::optional<LogicalTime>& atClusterTimeForSnapshotReadConcern,
        const boost::optional<LogicalTime>& placementConflictTimeForNonSnapshotReadConcern,
        bool doAppendStartTransaction,
        bool startOrContinueTransaction,
        bool hasTxnCreatedAnyDatabase);

    /**
     * Appends the needed fields when continuing a transaction on a participant.
     */
    static BSONObj appendFieldsForContinueTransaction(
        BSONObj cmdObj,
        const boost::optional<LogicalTime>& placementConflictTimeForNonSnapshotReadConcern,
        bool hasTxnCreatedAnyDatabase);

    /**
     * Returns a new read concern settings object by combining the input settings.
     */
    static repl::ReadConcernArgs reconcileReadConcern(
        const boost::optional<repl::ReadConcernArgs>& cmdLevelReadConcern,
        const repl::ReadConcernArgs& txnLevelReadConcern,
        const boost::optional<LogicalTime>& atClusterTimeForSnapshotReadConcern,
        const boost::optional<LogicalTime>& placementConflictTimeForNonSnapshotReadConcern);

private:
    /**
     * State in this struct may be read by methods of Observer or Router, and may be written by
     * methods of Router when they acquire the lock on the opCtx's Client. Access this inside
     * Observer and Router using the private o() method for reading and (Router only) the
     * o(WithLock) method for writing.
     */
    struct ObservableState {

        // Struct with fields txnNumber and txnRetryCounter.
        // If beginOrContinueTxn has been called, txnNumber and txnRetryCounter reflect
        // the router's currently active transaction. Otherwise, they are set to
        // kUninitializedTxnNumber and kUninitializedTxnRetryCounter by default.
        TxnNumberAndRetryCounter txnNumberAndRetryCounter{kUninitializedTxnNumber,
                                                          kUninitializedTxnRetryCounter};

        // Is updated at commit time to reflect which commit path was taken.
        CommitType commitType{CommitType::kNotInitiated};

        // Map of current participants of the current transaction.
        StringMap<Participant> participants;

        // The id of participant chosen as the two-phase commit coordinator. If, at commit time,
        // two-phase commit is required, the participant list is handed off to this shard. Is unset
        // until the transaction has targeted a participant, and is set to the first participant
        // targeted. Is reset if the first participant targeted returns a "needs retargeting" error.
        boost::optional<ShardId> coordinatorId;

        // The API parameters the current transaction was started with.
        APIParameters apiParameters;

        // The read concern the current transaction was started with.
        repl::ReadConcernArgs readConcernArgs;

        // The cluster time of the timestamp all participant shards in the current transaction with
        // snapshot level read concern must read from. Only set for transactions running with
        // snapshot level read concern.
        boost::optional<AtClusterTime> atClusterTimeForSnapshotReadConcern;
        boost::optional<AtClusterTime> placementConflictTimeForNonSnapshotReadConcern;

        // String representing the reason a transaction aborted. Either the string name of the error
        // code that led to an implicit abort or "abort" if the client sent abortTransaction.
        std::string abortCause;

        // Information about the last client to run a transaction operation on this transaction
        // router.
        SingleTransactionStats::LastClientInfo lastClientInfo;

        // Class responsible for updating per transaction and router wide transaction metrics on
        // certain transaction events. Unset until the transaction router has processed at least one
        // transaction command.
        boost::optional<MetricsTracker> metricsTracker;

        // How many operations that checked out the router's session have currently yielded it. The
        // transaction number cannot be changed until this returns to 0, otherwise we cannot
        // guarantee that unyielding the session cannot fail.
        int32_t activeYields{0};

        // Indicates whether the router was created by a shard that is an active transaction
        // participant.
        bool subRouter{false};
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

        // Tracks databases that this transaction has attempted to create.
        std::set<DatabaseName> createdDatabases;
    } _p;
};

}  // namespace mongo
