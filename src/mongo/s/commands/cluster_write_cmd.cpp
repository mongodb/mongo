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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/owned_pointer_vector.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/write_commands/write_commands_common.h"
#include "mongo/db/curop.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/storage/duplicate_key_error_info.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/cluster_last_error_info.h"
#include "mongo/s/commands/cluster_explain.h"
#include "mongo/s/commands/document_shard_key_update_util.h"
#include "mongo/s/grid.h"
#include "mongo/s/multi_statement_transaction_requests_sender.h"
#include "mongo/s/session_catalog_router.h"
#include "mongo/s/transaction_router.h"
#include "mongo/s/would_change_owning_shard_exception.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/s/write_ops/chunk_manager_targeter.h"
#include "mongo/s/write_ops/cluster_write.h"
#include "mongo/util/log.h"
#include "mongo/util/timer.h"

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(hangAfterThrowWouldChangeOwningShardRetryableWrite);

void batchErrorToLastError(const BatchedCommandRequest& request,
                           const BatchedCommandResponse& response,
                           LastError* error) {
    error->reset();

    std::unique_ptr<WriteErrorDetail> commandError;
    WriteErrorDetail* lastBatchError = nullptr;

    if (!response.getOk()) {
        // Command-level error, all writes failed
        commandError = stdx::make_unique<WriteErrorDetail>();
        commandError->setStatus(response.getTopLevelStatus());
        lastBatchError = commandError.get();
    } else if (response.isErrDetailsSet()) {
        // The last error in the batch is always reported - this matches expected COE semantics for
        // insert batches. For updates and deletes, error is only reported if the error was on the
        // last item.
        const bool lastOpErrored = response.getErrDetails().back()->getIndex() ==
            static_cast<int>(request.sizeWriteOps() - 1);
        if (request.getBatchType() == BatchedCommandRequest::BatchType_Insert || lastOpErrored) {
            lastBatchError = response.getErrDetails().back();
        }
    } else {
        // We don't care about write concern errors, these happen in legacy mode in GLE.
    }

    // Record an error if one exists
    if (lastBatchError) {
        const auto& errMsg = lastBatchError->toStatus().reason();
        error->setLastError(lastBatchError->toStatus().code(),
                            errMsg.empty() ? "see code for details" : errMsg);
        return;
    }

    // Record write stats otherwise
    //
    // NOTE: For multi-write batches, our semantics change a little because we don't have
    // un-aggregated "n" stats
    if (request.getBatchType() == BatchedCommandRequest::BatchType_Update) {
        BSONObj upsertedId;
        if (response.isUpsertDetailsSet()) {
            // Only report the very last item's upserted id if applicable
            if (response.getUpsertDetails().back()->getIndex() + 1 ==
                static_cast<int>(request.sizeWriteOps())) {
                upsertedId = response.getUpsertDetails().back()->getUpsertedID();
            }
        }

        const int numUpserted = response.isUpsertDetailsSet() ? response.sizeUpsertDetails() : 0;
        const int numMatched = response.getN() - numUpserted;
        invariant(numMatched >= 0);

        // Wrap upserted id in "upserted" field
        BSONObj leUpsertedId;
        if (!upsertedId.isEmpty()) {
            leUpsertedId = upsertedId.firstElement().wrap(kUpsertedFieldName);
        }

        error->recordUpdate(numMatched > 0, response.getN(), leUpsertedId);
    } else if (request.getBatchType() == BatchedCommandRequest::BatchType_Delete) {
        error->recordDelete(response.getN());
    }
}

/**
 * Checks if the response contains a WouldChangeOwningShard error. If it does, asserts that the
 * batch size is 1 and returns the extra info attached to the exception.
 */
boost::optional<WouldChangeOwningShardInfo> getWouldChangeOwningShardErrorInfo(
    OperationContext* opCtx,
    const BatchedCommandRequest& request,
    BatchedCommandResponse* response,
    bool originalCmdInTxn) {

    if (!response->getOk() || !response->isErrDetailsSet()) {
        return boost::none;
    }

    // Updating the shard key when batch size > 1 is disallowed when the document would move
    // shards. If the update is in a transaction uassert. If the write is not in a transaction,
    // change any WouldChangeOwningShard errors in this batch to InvalidOptions to be reported
    // to the user.
    if (request.sizeWriteOps() != 1U) {
        for (auto it = response->getErrDetails().begin(); it != response->getErrDetails().end();
             ++it) {
            if ((*it)->toStatus() != ErrorCodes::WouldChangeOwningShard) {
                continue;
            }

            if (originalCmdInTxn)
                uasserted(ErrorCodes::InvalidOptions,
                          "Document shard key value updates that cause the doc to move shards "
                          "must be sent with write batch of size 1");

            (*it)->setStatus({ErrorCodes::InvalidOptions,
                              "Document shard key value updates that cause the doc to move shards "
                              "must be sent with write batch of size 1"});
        }

        return boost::none;
    } else {
        for (const auto& err : response->getErrDetails()) {
            if (err->toStatus() != ErrorCodes::WouldChangeOwningShard) {
                continue;
            }

            BSONObjBuilder extraInfoBuilder;
            err->toStatus().extraInfo()->serialize(&extraInfoBuilder);
            auto extraInfo = extraInfoBuilder.obj();
            return WouldChangeOwningShardInfo::parseFromCommandError(extraInfo);
        }
    }

    return boost::none;
}

/**
 * Called when the response contains a WouldChangeOwningShard error. Deletes the original document
 * and inserts the new one in a transaction. Returns whether or not we match and delete the original
 * document. If the delete and insert succeed, modifies the response object to reflect that we
 * successfully updated one document.
 */
bool updateShardKeyValue(OperationContext* opCtx,
                         const BatchedCommandRequest& request,
                         BatchedCommandResponse* response,
                         const WouldChangeOwningShardInfo& wouldChangeOwningShardErrorInfo) {
    auto matchedDoc = documentShardKeyUpdateUtil::updateShardKeyForDocument(
        opCtx,
        request.getNS(),
        wouldChangeOwningShardErrorInfo,
        request.getWriteCommandBase().getStmtId() ? request.getWriteCommandBase().getStmtId().get()
                                                  : 0);
    if (!matchedDoc)
        return false;

    return true;
}

/**
 * Changes the shard key for the document if the response object contains a WouldChangeOwningShard
 * error. If the original command was sent as a retryable write, starts a transaction on the same
 * session and txnNum, deletes the original document, inserts the new one, and commits the
 * transaction. If the original command is part of a transaction, deletes the original document and
 * inserts the new one. Returns whether or not we actually complete the delete and insert.
 */
bool handleWouldChangeOwningShardError(OperationContext* opCtx,
                                       const BatchedCommandRequest& request,
                                       BatchedCommandResponse* response,
                                       BatchWriteExecStats stats) {
    auto txnRouter = TransactionRouter::get(opCtx);
    bool isRetryableWrite = opCtx->getTxnNumber() && !txnRouter;

    auto wouldChangeOwningShardErrorInfo =
        getWouldChangeOwningShardErrorInfo(opCtx, request, response, !isRetryableWrite);
    if (!wouldChangeOwningShardErrorInfo)
        return false;

    bool updatedShardKey = false;
    if (isRetryableWrite) {
        if (MONGO_FAIL_POINT(hangAfterThrowWouldChangeOwningShardRetryableWrite)) {
            log() << "Hit hangAfterThrowWouldChangeOwningShardRetryableWrite failpoint";
            MONGO_FAIL_POINT_PAUSE_WHILE_SET_OR_INTERRUPTED(
                opCtx, hangAfterThrowWouldChangeOwningShardRetryableWrite);
        }
        RouterOperationContextSession routerSession(opCtx);
        try {
            // Start transaction and re-run the original update command
            auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
            readConcernArgs = repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);

            auto txnRouterForShardKeyChange =
                documentShardKeyUpdateUtil::startTransactionForShardKeyUpdate(opCtx);
            // Clear the error details from the response object before sending the write again
            response->unsetErrDetails();
            ClusterWriter::write(opCtx, request, &stats, response);
            wouldChangeOwningShardErrorInfo =
                getWouldChangeOwningShardErrorInfo(opCtx, request, response, !isRetryableWrite);

            // If we do not get WouldChangeOwningShard when re-running the update, the document has
            // been modified or deleted concurrently and we do not need to delete it and insert a
            // new one.
            updatedShardKey = wouldChangeOwningShardErrorInfo &&
                updateShardKeyValue(
                                  opCtx, request, response, wouldChangeOwningShardErrorInfo.get());

            // Commit the transaction
            auto commitResponse = documentShardKeyUpdateUtil::commitShardKeyUpdateTransaction(
                opCtx, txnRouterForShardKeyChange);

            uassertStatusOK(getStatusFromCommandResult(commitResponse));

            auto writeConcernDetail = getWriteConcernErrorDetailFromBSONObj(commitResponse);
            if (writeConcernDetail && !writeConcernDetail->toStatus().isOK())
                response->setWriteConcernError(writeConcernDetail.release());
        } catch (const DBException& e) {
            // Set the error status to the status of the failed command and abort the transaction.
            auto status = e.toStatus();
            if (status == ErrorCodes::DuplicateKey) {
                BSONObjBuilder extraInfoBuilder;
                status.extraInfo()->serialize(&extraInfoBuilder);
                auto extraInfo = extraInfoBuilder.obj();
                if (extraInfo.getObjectField("keyPattern").hasField("_id"))
                    status = status.withContext(
                        "Failed to update document's shard key field. There is either an "
                        "orphan for this document or _id for this collection is not globally "
                        "unique.");
            } else {
                status = status.withContext(
                    "Update operation was converted into a distributed transaction because the "
                    "document being updated would move shards and that transaction failed");
            }

            response->getErrDetails().back()->setStatus(status);

            auto txnRouterForAbort = TransactionRouter::get(opCtx);
            if (txnRouterForAbort)
                txnRouterForAbort->implicitlyAbortTransaction(opCtx, status);

            return false;
        }
    } else {
        try {
            // Delete the original document and insert the new one
            updatedShardKey = updateShardKeyValue(
                opCtx, request, response, wouldChangeOwningShardErrorInfo.get());
        } catch (const ExceptionFor<ErrorCodes::DuplicateKey>& ex) {
            Status status = ex->getKeyPattern().hasField("_id")
                ? ex.toStatus().withContext(
                      "Failed to update document's shard key field. There is either an orphan "
                      "for this document or _id for this collection is not globally unique.")
                : ex.toStatus();
            uassertStatusOK(status);
        }
    }

    if (updatedShardKey) {
        // If we get here, the batch size is 1 and we have successfully deleted the old doc
        // and inserted the new one, so it is safe to unset the error details.
        response->unsetErrDetails();
        response->setN(response->getN() + 1);
        response->setNModified(response->getNModified() + 1);
    }

    return updatedShardKey;
}

/**
 * Base class for mongos write commands.
 */
class ClusterWriteCmd : public Command {
public:
    virtual ~ClusterWriteCmd() {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kNever;
    }

protected:
    class InvocationBase;

    ClusterWriteCmd(StringData name) : Command(name) {}

private:
    /**
     * Executes a write command against a particular database, and targets the command based on
     * a write operation.
     *
     * Does *not* retry or retarget if the metadata is stale.
     */
    static Status _commandOpWrite(OperationContext* opCtx,
                                  const std::string& dbName,
                                  const BSONObj& command,
                                  BatchItemRef targetingBatchItem,
                                  std::vector<Strategy::CommandResult>* results) {
        // Note that this implementation will not handle targeting retries and does not completely
        // emulate write behavior
        ChunkManagerTargeter targeter(targetingBatchItem.getRequest()->getNS());
        Status status = targeter.init(opCtx);
        if (!status.isOK())
            return status;

        auto swEndpoints = [&]() -> StatusWith<std::vector<ShardEndpoint>> {
            if (targetingBatchItem.getOpType() == BatchedCommandRequest::BatchType_Insert) {
                auto swEndpoint = targeter.targetInsert(opCtx, targetingBatchItem.getDocument());
                if (!swEndpoint.isOK())
                    return swEndpoint.getStatus();
                return std::vector<ShardEndpoint>{std::move(swEndpoint.getValue())};
            } else if (targetingBatchItem.getOpType() == BatchedCommandRequest::BatchType_Update) {
                return targeter.targetUpdate(opCtx, targetingBatchItem.getUpdate());
            } else if (targetingBatchItem.getOpType() == BatchedCommandRequest::BatchType_Delete) {
                return targeter.targetDelete(opCtx, targetingBatchItem.getDelete());
            } else {
                MONGO_UNREACHABLE;
            }
        }();

        if (!swEndpoints.isOK())
            return swEndpoints.getStatus();

        // Assemble requests
        std::vector<AsyncRequestsSender::Request> requests;
        for (const auto& endpoint : swEndpoints.getValue()) {
            requests.emplace_back(endpoint.shardName, command);
        }

        // Send the requests.

        const ReadPreferenceSetting readPref(ReadPreference::PrimaryOnly, TagSet());
        MultiStatementTransactionRequestsSender ars(
            opCtx,
            Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
            dbName,
            requests,
            readPref,
            Shard::RetryPolicy::kNoRetry);

        // Receive the responses.

        Status dispatchStatus = Status::OK();
        while (!ars.done()) {
            // Block until a response is available.
            auto response = ars.next();

            if (!response.swResponse.isOK()) {
                dispatchStatus = std::move(response.swResponse.getStatus());
                break;
            }

            Strategy::CommandResult result;

            // If the response status was OK, the response must contain which host was targeted.
            invariant(response.shardHostAndPort);
            result.target = ConnectionString(std::move(*response.shardHostAndPort));

            result.shardTargetId = std::move(response.shardId);
            result.result = std::move(response.swResponse.getValue().data);

            results->push_back(result);
        }

        return dispatchStatus;
    }
};

class ClusterWriteCmd::InvocationBase : public CommandInvocation {
public:
    InvocationBase(const ClusterWriteCmd* command,
                   const OpMsgRequest& request,
                   BatchedCommandRequest batchedRequest)
        : CommandInvocation(command),
          _bypass{shouldBypassDocumentValidationForCommand(request.body)},
          _request{&request},
          _batchedRequest{std::move(batchedRequest)} {}

    const BatchedCommandRequest& getBatchedRequest() const {
        return _batchedRequest;
    }

    bool getBypass() const {
        return _bypass;
    }

private:
    virtual void doCheckAuthorizationHook(AuthorizationSession* authzSession) const = 0;

    bool runImpl(OperationContext* opCtx,
                 const OpMsgRequest& request,
                 BatchedCommandRequest& batchedRequest,
                 BSONObjBuilder& result) const {
        auto db = batchedRequest.getNS().db();
        if (db != NamespaceString::kAdminDb && db != NamespaceString::kConfigDb) {
            batchedRequest.setAllowImplicitCreate(false);
        }

        BatchWriteExecStats stats;
        BatchedCommandResponse response;
        ClusterWriter::write(opCtx, batchedRequest, &stats, &response);

        bool updatedShardKey = false;
        if (_batchedRequest.getBatchType() == BatchedCommandRequest::BatchType_Update) {
            updatedShardKey =
                handleWouldChangeOwningShardError(opCtx, batchedRequest, &response, stats);
        }

        // Populate the lastError object based on the write response
        batchErrorToLastError(batchedRequest, response, &LastError::get(opCtx->getClient()));

        size_t numAttempts;

        if (!response.getOk()) {
            numAttempts = 0;
        } else if (batchedRequest.getWriteCommandBase().getOrdered() &&
                   response.isErrDetailsSet()) {
            // Add one failed attempt
            numAttempts = response.getErrDetailsAt(0)->getIndex() + 1;
        } else {
            numAttempts = batchedRequest.sizeWriteOps();
        }

        // TODO: increase opcounters by more than one
        auto& debug = CurOp::get(opCtx)->debug();
        switch (_batchedRequest.getBatchType()) {
            case BatchedCommandRequest::BatchType_Insert:
                for (size_t i = 0; i < numAttempts; ++i) {
                    globalOpCounters.gotInsert();
                }
                debug.additiveMetrics.ninserted = response.getN();
                break;
            case BatchedCommandRequest::BatchType_Update:
                for (size_t i = 0; i < numAttempts; ++i) {
                    globalOpCounters.gotUpdate();
                }
                debug.upsert = response.isUpsertDetailsSet();
                debug.additiveMetrics.nMatched =
                    response.getN() - (debug.upsert ? response.sizeUpsertDetails() : 0);
                debug.additiveMetrics.nModified = response.getNModified();
                break;
            case BatchedCommandRequest::BatchType_Delete:
                for (size_t i = 0; i < numAttempts; ++i) {
                    globalOpCounters.gotDelete();
                }
                debug.additiveMetrics.ndeleted = response.getN();
                break;
        }

        // Save the last opTimes written on each shard for this client, to allow GLE to work
        ClusterLastErrorInfo::get(opCtx->getClient())->addHostOpTimes(stats.getWriteOpTimes());

        // Record the number of shards targeted by this write.
        CurOp::get(opCtx)->debug().nShards =
            stats.getTargetedShards().size() + (updatedShardKey ? 1 : 0);

        if (auto txnRouter = TransactionRouter::get(opCtx)) {
            auto writeCmdStatus = response.toStatus();
            if (!writeCmdStatus.isOK()) {
                txnRouter->implicitlyAbortTransaction(opCtx, writeCmdStatus);
            }
        }

        result.appendElements(response.toBSON());
        return response.getOk();
    }

    void run(OperationContext* opCtx, rpc::ReplyBuilderInterface* result) override {
        BSONObjBuilder bob = result->getBodyBuilder();
        bool ok = runImpl(opCtx, *_request, _batchedRequest, bob);
        if (!ok)
            CommandHelpers::appendSimpleCommandStatus(bob, ok);
    }

    void explain(OperationContext* opCtx,
                 ExplainOptions::Verbosity verbosity,
                 rpc::ReplyBuilderInterface* result) override {
        uassert(ErrorCodes::InvalidLength,
                "explained write batches must be of size 1",
                _batchedRequest.sizeWriteOps() == 1U);

        const auto explainCmd = ClusterExplain::wrapAsExplain(_request->body, verbosity);

        // We will time how long it takes to run the commands on the shards.
        Timer timer;

        // Target the command to the shards based on the singleton batch item.
        BatchItemRef targetingBatchItem(&_batchedRequest, 0);
        std::vector<Strategy::CommandResult> shardResults;
        uassertStatusOK(_commandOpWrite(opCtx,
                                        _request->getDatabase().toString(),
                                        explainCmd,
                                        targetingBatchItem,
                                        &shardResults));
        auto bodyBuilder = result->getBodyBuilder();
        uassertStatusOK(ClusterExplain::buildExplainResult(
            opCtx, shardResults, ClusterExplain::kWriteOnShards, timer.millis(), &bodyBuilder));
    }

    NamespaceString ns() const override {
        return _batchedRequest.getNS();
    }

    bool supportsWriteConcern() const override {
        return true;
    }

    bool supportsReadConcern(repl::ReadConcernLevel level) const final {
        return true;
    }

    void doCheckAuthorization(OperationContext* opCtx) const final {
        try {
            doCheckAuthorizationHook(AuthorizationSession::get(opCtx->getClient()));
        } catch (const DBException& e) {
            LastError::get(opCtx->getClient()).setLastError(e.code(), e.reason());
            throw;
        }
    }

    const ClusterWriteCmd* command() const {
        return static_cast<const ClusterWriteCmd*>(definition());
    }

    bool _bypass;
    const OpMsgRequest* _request;
    BatchedCommandRequest _batchedRequest;
};

class ClusterInsertCmd final : public ClusterWriteCmd {
public:
    ClusterInsertCmd() : ClusterWriteCmd("insert") {}

private:
    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

    private:
        void doCheckAuthorizationHook(AuthorizationSession* authzSession) const final {
            auth::checkAuthForInsertCommand(
                authzSession, getBypass(), getBatchedRequest().getInsertRequest());
        }
    };

    std::unique_ptr<CommandInvocation> parse(OperationContext* opCtx,
                                             const OpMsgRequest& request) final {
        return stdx::make_unique<Invocation>(
            this,
            request,
            BatchedCommandRequest::cloneInsertWithIds(BatchedCommandRequest::parseInsert(request)));
    }

    std::string help() const override {
        return "insert documents";
    }

    LogicalOp getLogicalOp() const override {
        return LogicalOp::opInsert;
    }
} clusterInsertCmd;

class ClusterUpdateCmd final : public ClusterWriteCmd {
public:
    ClusterUpdateCmd() : ClusterWriteCmd("update") {}

private:
    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

    private:
        void doCheckAuthorizationHook(AuthorizationSession* authzSession) const final {
            auth::checkAuthForUpdateCommand(
                authzSession, getBypass(), getBatchedRequest().getUpdateRequest());
        }
    };

    std::unique_ptr<CommandInvocation> parse(OperationContext* opCtx,
                                             const OpMsgRequest& request) final {
        return stdx::make_unique<Invocation>(
            this, request, BatchedCommandRequest::parseUpdate(request));
    }

    std::string help() const override {
        return "update documents";
    }

    LogicalOp getLogicalOp() const override {
        return LogicalOp::opUpdate;
    }
} clusterUpdateCmd;

class ClusterDeleteCmd final : public ClusterWriteCmd {
public:
    ClusterDeleteCmd() : ClusterWriteCmd("delete") {}

private:
    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

    private:
        void doCheckAuthorizationHook(AuthorizationSession* authzSession) const final {
            auth::checkAuthForDeleteCommand(
                authzSession, getBypass(), getBatchedRequest().getDeleteRequest());
        }
    };

    std::unique_ptr<CommandInvocation> parse(OperationContext* opCtx,
                                             const OpMsgRequest& request) final {
        return stdx::make_unique<Invocation>(
            this, request, BatchedCommandRequest::parseDelete(request));
    }

    std::string help() const override {
        return "delete documents";
    }

    LogicalOp getLogicalOp() const override {
        return LogicalOp::opDelete;
    }
} clusterDeleteCmd;

}  // namespace
}  // namespace mongo
