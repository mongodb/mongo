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
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/future.h"

namespace mongo::txn_api {
namespace details {
class TxnMetadataHooks;
class Transaction;
}  // namespace details

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
     */
    virtual SemiFuture<BatchedCommandResponse> runCRUDOp(const BatchedCommandRequest& cmd,
                                                         std::vector<StmtId> stmtIds) const = 0;

    /**
     * Helper method that runs the given find in the transaction client's transaction and will
     * iterate and exhaust the find's cursor, returning a vector with all matching documents.
     */
    virtual SemiFuture<std::vector<BSONObj>> exhaustiveFind(
        const FindCommandRequest& cmd) const = 0;
};

/**
 * Encapsulates the logic for executing an internal transaction based on the state in the given
 * OperationContext and automatically retrying on errors.
 */
class TransactionWithRetries : public std::enable_shared_from_this<TransactionWithRetries> {
public:
    using TxnCallback =
        std::function<SemiFuture<void>(const TransactionClient& txnClient, ExecutorPtr txnExec)>;

    TransactionWithRetries(const TransactionWithRetries&) = delete;
    TransactionWithRetries operator=(const TransactionWithRetries&) = delete;

    /**
     * Main constructor that constructs an internal transaction with the default options.
     */
    TransactionWithRetries(OperationContext* opCtx, ExecutorPtr executor)
        : _executor(executor),
          _internalTxn(std::make_unique<details::Transaction>(opCtx, executor)) {}

    /**
     * Alternate constructor that accepts a custom transaction client.
     */
    TransactionWithRetries(OperationContext* opCtx,
                           ExecutorPtr executor,
                           std::unique_ptr<TransactionClient> txnClient)
        : _executor(executor),
          _internalTxn(
              std::make_unique<details::Transaction>(opCtx, executor, std::move(txnClient))) {}

    /**
     * Runs the given transaction callback synchronously, throwing on errors.
     *
     * TODO SERVER-61782: Make this async.
     * TODO SERVER-61782: Allow returning a SemiFuture with any type.
     */
    void runSync(OperationContext* opCtx, TxnCallback func);

private:
    /**
     * Attempts to abort the active internal transaction, logging on errors.
     */
    void _bestEffortAbort(OperationContext* opCtx);

    ExecutorPtr _executor;
    std::unique_ptr<details::Transaction> _internalTxn;
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
class Transaction {
public:
    enum class ErrorHandlingStep {
        kDoNotRetry,
        kRetryTransaction,
        kRetryCommit,
    };

    Transaction(const Transaction&) = delete;
    Transaction operator=(const Transaction&) = delete;

    /**
     * Main constructor that extracts the session options and infers its execution context from the
     * given OperationContext and constructs a default TransactionClient.
     */
    Transaction(OperationContext* opCtx, ExecutorPtr executor)
        : _initialOpCtx(opCtx),
          _executor(executor),
          _txnClient(std::make_unique<SEPTransactionClient>(opCtx, executor)) {
        _primeTransaction(opCtx);
        _txnClient->injectHooks(_makeTxnMetadataHooks());
    }

    /**
     * Alternate constructor that accepts a custom TransactionClient.
     */
    Transaction(OperationContext* opCtx,
                ExecutorPtr executor,
                std::unique_ptr<TransactionClient> txnClient)
        : _initialOpCtx(opCtx), _executor(executor), _txnClient(std::move(txnClient)) {
        _primeTransaction(opCtx);
        _txnClient->injectHooks(_makeTxnMetadataHooks());
    }

    /**
     * Returns the client used to run transaction commands.
     */
    const TransactionClient& getClient() {
        return *_txnClient;
    }

    /**
     * Used by the transaction runner to commit or abort the transaction. Returns an error if the
     * command fails.
     */
    SemiFuture<void> commit();
    SemiFuture<void> abort();

    /**
     * Handles the given client error or the latest error encountered in the transaction based on
     * where the transaction is in its lifecycle, e.g. by updating its txnNumber or txnRetryCounter,
     * and returns the next step for the transaction runner.
     */
    ErrorHandlingStep handleError(Status clientStatus);

    /**
     * Returns an object with info about the internal transaction for diagnostics.
     */
    BSONObj reportStateForLog() {
        return BSON("execContext" << _execContextToString(_execContext) << "sessionInfo"
                                  << _sessionInfo.toBSON());
    }

    /**
     * Attaches transaction metadata to the given command and updates internal transaction state.
     */
    void prepareRequest(BSONObjBuilder* cmdBuilder);

    /**
     * Extracts relevant info, like TransientTransactionError labels, from the given command
     * response.
     */
    void processResponse(const BSONObj& reply);

private:
    enum class TransactionState {
        kInit,
        kStarted,
        kStartedCommit,
        kStartedAbort,
        kDone,
    };

    enum class ExecutionContext {
        kOwnSession,
        kClientSession,
        kClientRetryableWrite,
        kClientTransaction,
    };

    std::string _execContextToString(ExecutionContext execContext);

    std::unique_ptr<TxnMetadataHooks> _makeTxnMetadataHooks() {
        return std::make_unique<TxnMetadataHooks>(*this);
    }

    void _setSessionInfo(LogicalSessionId lsid,
                         TxnNumber txnNumber,
                         boost::optional<TxnRetryCounter> txnRetryCounter);

    SemiFuture<void> _commitOrAbort(StringData dbName, StringData cmdName);

    /**
     * Extracts session options from Operation Context and infers the internal transaction’s
     * execution context, e.g. client has no session, client is running a retryable write.
     */
    void _primeTransaction(OperationContext* opCtx);

    /**
     * Prepares the internal transaction state for a full transaction retry.
     */
    void _primeForTransactionRetry();

    /**
     * Prepares the internal transaction state for a retry of commit.
     */
    void _primeForCommitRetry();

    OperationContext* const _initialOpCtx;
    ExecutorPtr _executor;
    std::unique_ptr<TransactionClient> _txnClient;

    bool _latestResponseHasTransientTransactionErrorLabel{false};
    Status _latestResponseStatus = Status::OK();
    Status _latestResponseWCStatus = Status::OK();

    OperationSessionInfo _sessionInfo;
    ExecutionContext _execContext;
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
