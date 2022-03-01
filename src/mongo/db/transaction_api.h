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
#include "mongo/db/logical_session_id.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/find_command_gen.h"
#include "mongo/db/resource_yielder.h"
#include "mongo/rpc/write_concern_error_detail.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/future.h"

namespace mongo::txn_api {
namespace details {
class TxnMetadataHooks;
class Transaction;
}  // namespace details

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
    virtual void injectHooks(std::unique_ptr<details::TxnMetadataHooks> hooks) = 0;

    /**
     * Runs the given command as part of the transaction that owns this transaction client.
     */
    virtual SemiFuture<BSONObj> runCommand(StringData dbName, BSONObj cmd) const = 0;

    /**
     * Helper method to run commands representable as a BatchedCommandRequest in the transaction
     * client's transaction.
     *
     * The given stmtIds are included in the sent command. If the API's transaction was spawned on
     * behalf of a retryable write, the statement ids must be unique for each write in the
     * transaction as the underlying servers will save history for each id the same as for a
     * retryable write. A write can opt out of this by sending a -1 statement id, which is ignored.
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

    /**
     * Helper method that runs the given find in the transaction client's transaction and will
     * iterate and exhaust the find's cursor, returning a vector with all matching documents.
     */
    virtual SemiFuture<std::vector<BSONObj>> exhaustiveFind(
        const FindCommandRequest& cmd) const = 0;

    /**
     * Whether the implementation expects to work in the client transaction context. The API
     * currently assumes the client transaction was always started in the server before the API is
     * invoked, which is true for service entry point clients, but may not be true for all possible
     * implementations.
     */
    virtual bool supportsClientTransactionContext() const = 0;
};

using Callback =
    unique_function<SemiFuture<void>(const TransactionClient& txnClient, ExecutorPtr txnExec)>;

/**
 * Encapsulates the logic for executing an internal transaction based on the state in the given
 * OperationContext and automatically retrying on errors.
 */
class TransactionWithRetries {
public:
    TransactionWithRetries(const TransactionWithRetries&) = delete;
    TransactionWithRetries operator=(const TransactionWithRetries&) = delete;

    /**
     * Main constructor that constructs an internal transaction with the default options.
     */
    TransactionWithRetries(OperationContext* opCtx,
                           ExecutorPtr executor,
                           std::unique_ptr<ResourceYielder> resourceYielder)
        : _internalTxn(std::make_shared<details::Transaction>(opCtx, executor)),
          _resourceYielder(std::move(resourceYielder)) {}

    /**
     * Alternate constructor that accepts a custom transaction client.
     */
    TransactionWithRetries(OperationContext* opCtx,
                           ExecutorPtr executor,
                           std::unique_ptr<TransactionClient> txnClient,
                           std::unique_ptr<ResourceYielder> resourceYielder)
        : _internalTxn(
              std::make_shared<details::Transaction>(opCtx, executor, std::move(txnClient))),
          _resourceYielder(std::move(resourceYielder)) {}

    /**
     * Runs the given transaction callback synchronously.
     *
     * Returns a bundle with the commit command status and write concern error, if any. Any error
     * prior to receiving a response from commit (e.g. an interruption or a user assertion in the
     * given callback) will result in a non-ok StatusWith. Note that abort errors are not returned
     * because an abort will only happen implicitly when another error has occurred, and that
     * original error is returned instead.
     *
     * TODO SERVER-61782: Make this async.
     * TODO SERVER-61782: Allow returning a SemiFuture with any type.
     */
    StatusWith<CommitResult> runSyncNoThrow(OperationContext* opCtx, Callback callback) noexcept;

    /**
     * Same as above except will throw if the commit result has a non-ok command status or a write
     * concern error.
     */
    void runSync(OperationContext* opCtx, Callback callback) {
        auto result = uassertStatusOK(runSyncNoThrow(opCtx, std::move(callback)));
        uassertStatusOK(result.getEffectiveStatus());
    }

private:
    /**
     * Attempts to abort the active internal transaction, logging on errors.
     */
    void _bestEffortAbort(OperationContext* opCtx);

    /**
     * Runs the body, commit, and abort logic in the owned internal transaction. Yields the
     * ResourceYielder before constructing and waiting on the future and then unyields before
     * returning the ready future's result. The only case where the resource won't be unyielded is
     * if the given operation context is interrupted or reaches its deadline.
     *
     * Notably, the futures are constructed after yielding so a future made with an inline executor
     * will still run after the yield.
     */
    Status _runBodyWithYields(OperationContext* opCtx) noexcept;
    StatusWith<CommitResult> _runCommitWithYields(OperationContext* opCtx) noexcept;
    Status _runAbortWithYields(OperationContext* opCtx) noexcept;

    std::shared_ptr<details::Transaction> _internalTxn;
    std::unique_ptr<ResourceYielder> _resourceYielder;
};

/**
 * Contains implementation details for the above API. Classes in this namespace should not be used
 * directly.
 */
namespace details {

/**
 * Default transaction client that runs given commands through the local process service entry
 * point.
 */
class SEPTransactionClient : public TransactionClient {
public:
    SEPTransactionClient(OperationContext* opCtx, ExecutorPtr executor)
        : _serviceContext(opCtx->getServiceContext()), _executor(executor) {
        _cancelableOpCtxFactory = std::make_unique<CancelableOperationContextFactory>(
            opCtx->getCancellationToken(), executor);
    }

    SEPTransactionClient(const SEPTransactionClient&) = delete;
    SEPTransactionClient operator=(const SEPTransactionClient&) = delete;

    virtual void injectHooks(std::unique_ptr<details::TxnMetadataHooks> hooks) override {
        invariant(!_hooks);
        _hooks = std::move(hooks);
    }

    virtual SemiFuture<BSONObj> runCommand(StringData dbName, BSONObj cmd) const override;

    virtual SemiFuture<BatchedCommandResponse> runCRUDOp(
        const BatchedCommandRequest& cmd, std::vector<StmtId> stmtIds) const override;

    virtual SemiFuture<std::vector<BSONObj>> exhaustiveFind(
        const FindCommandRequest& cmd) const override;

    virtual bool supportsClientTransactionContext() const override {
        return true;
    }

private:
    ServiceContext* const _serviceContext;
    ExecutorPtr _executor;
    std::unique_ptr<details::TxnMetadataHooks> _hooks;
    std::unique_ptr<CancelableOperationContextFactory> _cancelableOpCtxFactory;
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

    enum class TransactionState {
        kInit,
        kStarted,
        kStartedCommit,
        kStartedAbort,
        kDone,
    };

    Transaction(const Transaction&) = delete;
    Transaction operator=(const Transaction&) = delete;

    /**
     * Main constructor that extracts the session options and infers its execution context from the
     * given OperationContext and constructs a default TransactionClient.
     */
    Transaction(OperationContext* opCtx, ExecutorPtr executor)
        : _executor(executor), _txnClient(std::make_unique<SEPTransactionClient>(opCtx, executor)) {
        _primeTransaction(opCtx);
        _txnClient->injectHooks(_makeTxnMetadataHooks());
    }

    /**
     * Alternate constructor that accepts a custom TransactionClient.
     */
    Transaction(OperationContext* opCtx,
                ExecutorPtr executor,
                std::unique_ptr<TransactionClient> txnClient)
        : _executor(executor), _txnClient(std::move(txnClient)) {
        _primeTransaction(opCtx);
        _txnClient->injectHooks(_makeTxnMetadataHooks());
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
    ErrorHandlingStep handleError(const StatusWith<CommitResult>& swResult) const;

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
    void primeForTransactionRetry();

    /**
     * Prepares the internal transaction state for a retry of commit.
     */
    void primeForCommitRetry();

    /**
     * Returns the latest operationTime returned by a command in this transaction.
     */
    LogicalTime getOperationTime() const;

private:
    std::unique_ptr<TxnMetadataHooks> _makeTxnMetadataHooks() {
        return std::make_unique<TxnMetadataHooks>(*this);
    }

    BSONObj _reportStateForLog(WithLock) const;

    void _setSessionInfo(WithLock,
                         LogicalSessionId lsid,
                         TxnNumber txnNumber,
                         boost::optional<bool> startTransaction);

    SemiFuture<BSONObj> _commitOrAbort(StringData dbName, StringData cmdName);

    /**
     * Extracts transaction options from Operation Context and infers the internal transaction’s
     * execution context, e.g. client has no session, client is running a retryable write.
     */
    void _primeTransaction(OperationContext* opCtx);

    const ExecutorPtr _executor;
    std::unique_ptr<TransactionClient> _txnClient;
    Callback _callback;

    BSONObj _writeConcern;
    BSONObj _readConcern;
    ExecutionContext _execContext;

    // Protects the members below that are accessed by the TxnMetadataHooks, which are called by the
    // user's callback and may run on a separate thread than the one that is driving the
    // Transaction.
    mutable Mutex _mutex = MONGO_MAKE_LATCH("Transaction::_mutex");

    LogicalTime _lastOperationTime;
    bool _latestResponseHasTransientTransactionErrorLabel{false};

    OperationSessionInfo _sessionInfo;
    TransactionState _state{TransactionState::kInit};
};

/**
 * Hooks called by each TransactionClient before sending a request and upon receiving a response
 * responsible for attaching relevant transaction metadata and updating the transaction's state
 */
class TxnMetadataHooks {
public:
    TxnMetadataHooks(details::Transaction& internalTxn) : _internalTxn(internalTxn) {}

    void runRequestHook(BSONObjBuilder* cmdBuilder) {
        _internalTxn.prepareRequest(cmdBuilder);
    }

    void runReplyHook(const BSONObj& reply) {
        _internalTxn.processResponse(reply);
    }

private:
    Transaction& _internalTxn;
};

}  // namespace details
}  // namespace mongo::txn_api
