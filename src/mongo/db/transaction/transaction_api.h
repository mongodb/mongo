/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/find_command_gen.h"
#include "mongo/db/resource_yielder.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/executor/inline_executor.h"
#include "mongo/executor/task_executor.h"
#include "mongo/rpc/write_concern_error_detail.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/future.h"

namespace mongo::txn_api {
namespace details {
class TxnHooks;
class TransactionWithRetries;
}  // namespace details

// Max number of retries allowed for a transaction operation. The API uses exponential backoffs
// capped at 1 second for transient error and commit network error retries, so this corresponds to
// roughly 2 minutes of sleeps in total between retries meant to loosely mirror the 2 minute timeout
// used by the driver's convenient transactions API:
// https://github.com/mongodb/specifications/blob/92d77a6d/source/transactions-convenient-api/transactions-convenient-api.rst
static constexpr int kTxnRetryLimit = 120;
static constexpr auto kMaxTimeMSField = "maxTimeMS";

/**
 * Encapsulates the command status and write concern error from a response to a commitTransaction
 * command.
 */
struct CommitResult {
    /**
     * Returns an error status with additional context if any of the inner errors are non OK.
     */
    Status getEffectiveStatus() const {
        if (!cmdStatus.isOK()) {
            return cmdStatus.withContext("Command error committing internal transaction");
        }
        if (!wcError.toStatus().isOK()) {
            return wcError.toStatus().withContext(
                "Write concern error committing internal transaction");
        }
        return Status::OK();
    }

    Status cmdStatus;
    WriteConcernErrorDetail wcError;
};

/**
 * Interface for the “backend” of an internal transaction responsible for executing commands.
 * Intended to be overriden and customized for different use cases.
 */
class TransactionClient {
public:
    virtual ~TransactionClient(){};

    /**
     * Called by the transaction that owns this transaction client to install hooks for attaching
     * transaction metadata to requests and parsing it from responses. Must be called before any
     * commands have been sent and cannot be called more than once.
     */
    virtual void initialize(std::unique_ptr<details::TxnHooks> hooks) = 0;

    /**
     * Runs the given command as part of the transaction that owns this transaction client.
     */
    virtual SemiFuture<BSONObj> runCommand(const DatabaseName& dbName, BSONObj cmd) const = 0;

    virtual BSONObj runCommandSync(const DatabaseName& dbName, BSONObj cmd) const = 0;
    /**
     * Helper method to run commands representable as a BatchedCommandRequest in the transaction
     * client's transaction.
     *
     * The given stmtIds are included in the sent command. If the API's transaction was spawned on
     * behalf of a retryable write, the statement ids must be unique for each write in the
     * transaction as the underlying servers will save history for each id the same as for a
     * retryable write. A write can opt out of this by sending a -1 statement id or an empty vector,
     * which is ignored.
     *
     * If a sent statement id had already been seen for this transaction, the write with that id
     * won't apply a second time and instead returns its response from its original execution. That
     * write's id will be in the batch response's "retriedStmtIds" array field.
     *
     * Users of this API for transactions spawned on behalf of retryable writes likely should
     * include a stmtId for each write that should not execute twice and should check the
     * "retriedStmtIds" in the returned BatchedCommandResponse to detect when a write had already
     * applied, and thus the retryable write that spawned this transaction has already committed.
     * Note that only one "pre" or "post" image can be stored per transaction, so only one
     * findAndModify per transaction may have a non -1 statement id.
     *
     */
    virtual SemiFuture<BatchedCommandResponse> runCRUDOp(const BatchedCommandRequest& cmd,
                                                         std::vector<StmtId> stmtIds) const = 0;
    virtual BatchedCommandResponse runCRUDOpSync(const BatchedCommandRequest& cmd,
                                                 std::vector<StmtId> stmtIds) const = 0;

    /**
     * Helper method that runs the given find in the transaction client's transaction and will
     * iterate and exhaust the find's cursor, returning a vector with all matching documents.
     */
    virtual SemiFuture<std::vector<BSONObj>> exhaustiveFind(
        const FindCommandRequest& cmd) const = 0;
    virtual std::vector<BSONObj> exhaustiveFindSync(const FindCommandRequest& cmd) const = 0;

    /**
     * Whether the implementation expects to work in the client transaction context. The API
     * currently assumes the client transaction was always started in the server before the API is
     * invoked, which is true for service entry point clients, but may not be true for all possible
     * implementations.
     */
    virtual bool supportsClientTransactionContext() const = 0;

    /**
     * Returns if the client is eligible to run cluster operations.
     */
    virtual bool runsClusterOperations() const = 0;
};

using Callback =
    unique_function<SemiFuture<void>(const TransactionClient& txnClient, ExecutorPtr txnExec)>;

/**
 * Encapsulates the logic for executing an internal transaction based on the state in the given
 * OperationContext and automatically retrying on errors.
 *
 * TODO SERVER-65839: Make a version for async contexts that doesn't require an opCtx.
 */
class SyncTransactionWithRetries {
public:
    SyncTransactionWithRetries(const SyncTransactionWithRetries&) = delete;
    SyncTransactionWithRetries operator=(const SyncTransactionWithRetries&) = delete;

    /**
     * Returns a SyncTransactionWithRetries suitable for use within an existing operation. The
     * session options from the given opCtx will be used to infer the transaction's options.
     *
     * Optionally accepts a custom TransactionClient and will default to a client that runs commands
     * against the local service entry point.
     */
    SyncTransactionWithRetries(
        OperationContext* opCtx,
        std::shared_ptr<executor::InlineExecutor::SleepableExecutor> sleepableExecutor,
        std::unique_ptr<ResourceYielder> resourceYielder,
        std::shared_ptr<executor::InlineExecutor> executor,
        std::unique_ptr<TransactionClient> txnClient = nullptr);
    /**
     * Returns a bundle with the commit command status and write concern error, if any. Any error
     * prior to receiving a response from commit (e.g. an interruption or a user assertion in the
     * given callback) will result in a non-ok StatusWith. Note that abort errors are not returned
     * because an abort will only happen implicitly when another error has occurred, and that
     * original error is returned instead.
     *
     * Will yield resources on the given opCtx before running if a resourceYielder was provided in
     * the constructor and unyield after running. Unyield will always be attempted if yield
     * succeeded, but an error from unyield will not be returned if the transaction itself returned
     * an error.
     *
     * TODO SERVER-65840: Allow returning any type.
     */
    StatusWith<CommitResult> runNoThrow(OperationContext* opCtx, Callback callback) noexcept;

    /**
     * Same as above except will throw if the commit result has a non-ok command status or a write
     * concern error.
     */
    void run(OperationContext* opCtx, Callback callback) {
        auto result = uassertStatusOK(runNoThrow(opCtx, std::move(callback)));
        uassertStatusOK(result.getEffectiveStatus());
    }

private:
    CancellationSource _source;
    std::unique_ptr<ResourceYielder> _resourceYielder;
    std::shared_ptr<executor::InlineExecutor> _inlineExecutor;
    std::shared_ptr<executor::InlineExecutor::SleepableExecutor> _sleepExec;
    std::shared_ptr<details::TransactionWithRetries> _txn;
};

/**
 * Contains implementation details for the above API. Classes in this namespace should not be used
 * directly.
 */
namespace details {

/**
 * Customization point for behaviors different in the default SEPTransactionClient and the one for
 * running distributed transactions.
 */
class SEPTransactionClientBehaviors {
public:
    virtual ~SEPTransactionClientBehaviors() {}

    /**
     * Makes any necessary modifications to the given command, e.g. changing the name to the
     * "cluster" version for the cluster behaviors.
     */
    virtual BSONObj maybeModifyCommand(BSONObj cmdObj) const = 0;

    /**
     * Returns a future with the result of running the given request.
     */
    virtual Future<DbResponse> handleRequest(OperationContext* opCtx,
                                             const Message& request) const = 0;

    /**
     * Returns if the client is eligible to run cluster operations.
     */
    virtual bool runsClusterOperations() const = 0;
};

/**
 * Default behaviors that does not modify commands and runs them against the local process service
 * entry point.
 */
class DefaultSEPTransactionClientBehaviors : public SEPTransactionClientBehaviors {
public:
    BSONObj maybeModifyCommand(BSONObj cmdObj) const override {
        return cmdObj;
    }

    Future<DbResponse> handleRequest(OperationContext* opCtx,
                                     const Message& request) const override;

    bool runsClusterOperations() const {
        return false;
    }
};

/**
 * Default transaction client that runs given commands through the local process service entry
 * point.
 */
class SEPTransactionClient : public TransactionClient {
public:
    SEPTransactionClient(OperationContext* opCtx,
                         std::shared_ptr<executor::InlineExecutor> inlineExecutor,
                         std::shared_ptr<executor::InlineExecutor::SleepableExecutor> executor,
                         std::unique_ptr<SEPTransactionClientBehaviors> behaviors)
        : _serviceContext(opCtx->getServiceContext()),
          _inlineExecutor(inlineExecutor),
          _executor(executor),
          _behaviors(std::move(behaviors)) {}

    SEPTransactionClient(const SEPTransactionClient&) = delete;
    SEPTransactionClient operator=(const SEPTransactionClient&) = delete;

    virtual void initialize(std::unique_ptr<details::TxnHooks> hooks) override {
        invariant(!_hooks);
        _hooks = std::move(hooks);
    }

    virtual SemiFuture<BSONObj> runCommand(const DatabaseName& dbName, BSONObj cmd) const override;
    virtual BSONObj runCommandSync(const DatabaseName& dbName, BSONObj cmd) const override;

    virtual SemiFuture<BatchedCommandResponse> runCRUDOp(
        const BatchedCommandRequest& cmd, std::vector<StmtId> stmtIds) const override;
    virtual BatchedCommandResponse runCRUDOpSync(const BatchedCommandRequest& cmd,
                                                 std::vector<StmtId> stmtIds) const override;

    virtual SemiFuture<std::vector<BSONObj>> exhaustiveFind(
        const FindCommandRequest& cmd) const override;
    virtual std::vector<BSONObj> exhaustiveFindSync(const FindCommandRequest& cmd) const override;

    virtual bool supportsClientTransactionContext() const override {
        return true;
    }

    virtual bool runsClusterOperations() const override {
        return _behaviors->runsClusterOperations();
    }

private:
    ExecutorFuture<BSONObj> _runCommand(const DatabaseName& dbName, BSONObj cmd) const;

    ExecutorFuture<BatchedCommandResponse> _runCRUDOp(const BatchedCommandRequest& cmd,
                                                      std::vector<StmtId> stmtIds) const;

    ExecutorFuture<std::vector<BSONObj>> _exhaustiveFind(const FindCommandRequest& cmd) const;

private:
    ServiceContext* const _serviceContext;
    std::shared_ptr<executor::InlineExecutor> _inlineExecutor;
    std::shared_ptr<executor::InlineExecutor::SleepableExecutor> _executor;
    std::unique_ptr<SEPTransactionClientBehaviors> _behaviors;
    std::unique_ptr<details::TxnHooks> _hooks;
};

/**
 * Encapsulates the logic for an internal transaction based on the state in the given
 * OperationContext.
 */
class Transaction : public std::enable_shared_from_this<Transaction> {
public:
    enum class ExecutionContext {
        kOwnSession,
        kClientSession,
        kClientRetryableWrite,
        kClientTransaction,
    };

    enum class ErrorHandlingStep {
        kDoNotRetry,
        kAbortAndDoNotRetry,
        kRetryTransaction,
        kRetryCommit,
    };

    Transaction(const Transaction&) = delete;
    Transaction operator=(const Transaction&) = delete;
    ~Transaction();

    /**
     * Constructs a Transaction with the given TransactionClient and extracts the session options
     * and infers its execution context from the given OperationContext.
     */
    Transaction(OperationContext* opCtx,
                std::shared_ptr<executor::InlineExecutor::SleepableExecutor> executor,
                const CancellationToken& token,
                std::unique_ptr<TransactionClient> txnClient)
        : _executor(executor),
          _txnClient(std::move(txnClient)),
          _token(token),
          _service(opCtx->getServiceContext()) {
        _primeTransaction(opCtx);
        _txnClient->initialize(_makeTxnHooks());
    }

    /**
     * Sets the callback to be used by this transaction.
     */
    void setCallback(Callback callback) {
        invariant(!_callback);
        _callback = std::move(callback);
    }

    /**
     * Runs the previously set callback with the TransactionClient owned by this transaction.
     */
    SemiFuture<void> runCallback();

    /**
     * Used by the transaction runner to commit the transaction. Returns a future with a non-OK
     * status if the commit failed to send, otherwise returns a future with a bundle with the
     * command and write concern statuses.
     */
    SemiFuture<CommitResult> commit();

    /**
     * Used by the transaction runner to abort the transaction. Returns a future with a non-OK
     * status if there was an error sending the command, a non-ok command result, or a write concern
     * error.
     */
    SemiFuture<void> abort();

    /**
     * Handles the given transaction result based on where the transaction is in its lifecycle and
     * its execution context, e.g. by updating its txnNumber, returning the next step for the
     * transaction runner.
     */
    ErrorHandlingStep handleError(const StatusWith<CommitResult>& swResult,
                                  int attemptCounter) const noexcept;

    /**
     * Returns an object with info about the internal transaction for diagnostics.
     */
    BSONObj reportStateForLog() const;

    /**
     * Attaches transaction metadata to the given command and updates internal transaction state.
     */
    void prepareRequest(BSONObjBuilder* cmdBuilder);

    /**
     * Extracts relevant info, like TransientTransactionError labels, from the given command
     * response.
     */
    void processResponse(const BSONObj& reply);

    /**
     * Prepares the internal transaction state for a full transaction retry.
     */
    void primeForTransactionRetry() noexcept;

    /**
     * Prepares the internal transaction state for a retry of commit.
     */
    void primeForCommitRetry() noexcept;

    /**
     * Prepares the transaction state to be cleaned up after a fatal error.
     */
    void primeForCleanup() noexcept;

    /**
     * True if the transaction must be cleaned up, which implies it cannot be continued.
     */
    bool needsCleanup() const noexcept;

    /**
     * Returns a cancellation token to be used by the transaction's client. May change depending on
     * the state of the transaction, i.e. returns an uncancelable token in the kNeedsCleanup state.
     */
    CancellationToken getTokenForCommand() const;

    /**
     * Returns the latest operationTime returned by a command in this transaction.
     */
    LogicalTime getOperationTime() const;

    /**
     * Returns an unowned pointer to the ServiceContext used to create this transaction.
     */
    ServiceContext* getParentServiceContext() const {
        return _service;
    }

private:
    class TransactionState {
    public:
        enum StateFlag {
            kInit,
            kStarted,
            kStartedCommit,
            kRetryingCommit,
            kStartedAbort,
            kNeedsCleanup,
        };

        bool is(StateFlag state) const {
            return _state == state;
        }

        bool isInCommit() const {
            return _state == TransactionState::kStartedCommit ||
                _state == TransactionState::kRetryingCommit;
        }

        /**
         * Transitions the state and validates the transition is legal, crashing if it is not.
         */
        void transitionTo(StateFlag newState);

        static std::string toString(StateFlag state);

        std::string toString() const {
            return toString(_state);
        }

    private:
        bool _isLegalTransition(StateFlag oldState, StateFlag newState);

        StateFlag _state = kInit;
    };

    std::unique_ptr<TxnHooks> _makeTxnHooks() {
        return std::make_unique<TxnHooks>(*this);
    }

    BSONObj _reportStateForLog(WithLock) const;

    void _setSessionInfo(WithLock,
                         LogicalSessionId lsid,
                         TxnNumber txnNumber,
                         boost::optional<bool> startTransaction);

    SemiFuture<BSONObj> _commitOrAbort(const DatabaseName& dbName, StringData cmdName);

    /**
     * Extracts transaction options from Operation Context and infers the internal transaction’s
     * execution context, e.g. client has no session, client is running a retryable write.
     */
    void _primeTransaction(OperationContext* opCtx);

    std::shared_ptr<executor::InlineExecutor::SleepableExecutor> _executor;
    std::unique_ptr<TransactionClient> _txnClient;
    CancellationToken _token;
    Callback _callback;

    boost::optional<Date_t> _opDeadline;
    BSONObj _writeConcern;
    BSONObj _readConcern;
    APIParameters _apiParameters;
    ExecutionContext _execContext;

    // Protects the members below that are accessed by the TxnHooks, which are called by the user's
    // callback and may run on a separate thread than the one that is driving the Transaction.
    mutable Mutex _mutex = MONGO_MAKE_LATCH("Transaction::_mutex");

    LogicalTime _lastOperationTime;
    bool _latestResponseHasTransientTransactionErrorLabel{false};

    OperationSessionInfo _sessionInfo;
    TransactionState _state;
    bool _acquiredSessionFromPool{false};
    ServiceContext* _service;
};

/**
 * Hooks used by each TransactionClient to interface with its associated Transaction.
 */
class TxnHooks {
public:
    TxnHooks(details::Transaction& internalTxn) : _internalTxn(internalTxn) {}

    /**
     * Called before sending a command in the TransactionClient.
     */
    void runRequestHook(BSONObjBuilder* cmdBuilder) {
        _internalTxn.prepareRequest(cmdBuilder);
    }

    /**
     * Called after receiving a response to a command in the TransactionClient.
     */
    void runReplyHook(const BSONObj& reply) {
        _internalTxn.processResponse(reply);
    }

    /**
     * Called to get the cancellation token to be used for a command in the TransactionClient.
     */
    CancellationToken getTokenForCommand() {
        return _internalTxn.getTokenForCommand();
    }

private:
    Transaction& _internalTxn;
};

class TransactionWithRetries : public std::enable_shared_from_this<TransactionWithRetries> {
public:
    TransactionWithRetries(const TransactionWithRetries&) = delete;
    TransactionWithRetries operator=(const TransactionWithRetries&) = delete;

    TransactionWithRetries(OperationContext* opCtx,
                           std::shared_ptr<executor::InlineExecutor::SleepableExecutor> executor,
                           const CancellationToken& token,
                           std::unique_ptr<TransactionClient> txnClient)
        : _internalTxn(std::make_shared<Transaction>(opCtx, executor, token, std::move(txnClient))),
          _executor(executor),
          _token(token) {}

    /**
     * Returns a bundle with the commit command status and write concern error, if any. Any error
     * prior to receiving a response from commit (e.g. an interruption or a user assertion in the
     * given callback) will result in a non-ok StatusWith. Note that abort errors are not returned
     * because an abort will only happen implicitly when another error has occurred, and that
     * original error is returned instead.
     *
     * TODO SERVER-65840: Allow returning a SemiFuture with any type.
     */
    SemiFuture<CommitResult> run(OperationContext* opCtx, Callback callback) noexcept;

    /**
     * Returns the latest operationTime returned by a command in this transaction.
     */
    LogicalTime getOperationTime() const {
        return _internalTxn->getOperationTime();
    }

    /**
     * If the transaction needs to be cleaned up, i.e. aborted, this will schedule the necessary
     * work. Callers can wait for cleanup by waiting on the returned future.
     */
    SemiFuture<void> cleanUpIfNecessary();

private:
    // Helper methods for running a transaction.
    ExecutorFuture<void> _runBodyHandleErrors(int bodyAttempts);
    ExecutorFuture<CommitResult> _runCommitHandleErrors(int commitAttempts);
    ExecutorFuture<CommitResult> _runCommitWithRetries();

    /**
     * Attempts to abort the active internal transaction, logging on errors after swallowing them.
     */
    ExecutorFuture<void> _bestEffortAbort();

    std::shared_ptr<Transaction> _internalTxn;
    std::shared_ptr<executor::InlineExecutor::SleepableExecutor> _executor;
    CancellationToken _token;
};

}  // namespace details
}  // namespace mongo::txn_api
