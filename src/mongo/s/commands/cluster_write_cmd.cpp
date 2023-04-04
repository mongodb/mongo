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


#include "mongo/s/commands/cluster_write_cmd.h"

#include "mongo/base/error_codes.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/curop.h"
#include "mongo/db/fle_crud.h"
#include "mongo/db/internal_transactions_feature_flag_gen.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/storage/duplicate_key_error_info.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/client/num_hosts_targeted_metrics.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/cluster_write.h"
#include "mongo/s/collection_routing_info_targeter.h"
#include "mongo/s/commands/cluster_explain.h"
#include "mongo/s/commands/document_shard_key_update_util.h"
#include "mongo/s/grid.h"
#include "mongo/s/session_catalog_router.h"
#include "mongo/s/transaction_router.h"
#include "mongo/s/transaction_router_resource_yielder.h"
#include "mongo/s/would_change_owning_shard_exception.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/timer.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

using QuerySamplingOptions = OperationContext::QuerySamplingOptions;

MONGO_FAIL_POINT_DEFINE(hangAfterThrowWouldChangeOwningShardRetryableWrite);

void batchErrorToNotPrimaryErrorTracker(const BatchedCommandRequest& request,
                                        const BatchedCommandResponse& response,
                                        NotPrimaryErrorTracker* tracker) {
    tracker->reset();

    boost::optional<Status> commandStatus;
    Status const* lastBatchStatus = nullptr;

    if (!response.getOk()) {
        // Command-level error, all writes failed
        commandStatus = response.getTopLevelStatus();
        lastBatchStatus = commandStatus.get_ptr();
    } else if (response.isErrDetailsSet()) {
        // The last error in the batch is always reported - this matches expected COE semantics for
        // insert batches. For updates and deletes, error is only reported if the error was on the
        // last item.
        const bool lastOpErrored = response.getErrDetails().back().getIndex() ==
            static_cast<int>(request.sizeWriteOps() - 1);
        if (request.getBatchType() == BatchedCommandRequest::BatchType_Insert || lastOpErrored) {
            lastBatchStatus = &response.getErrDetails().back().getStatus();
        }
    }

    // Record an error if one exists
    if (lastBatchStatus) {
        tracker->recordError(lastBatchStatus->code());
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
        for (auto& err : response->getErrDetails()) {
            if (err.getStatus() != ErrorCodes::WouldChangeOwningShard) {
                continue;
            }

            if (originalCmdInTxn)
                uasserted(ErrorCodes::InvalidOptions,
                          "Document shard key value updates that cause the doc to move shards "
                          "must be sent with write batch of size 1");

            err.setStatus({ErrorCodes::InvalidOptions,
                           "Document shard key value updates that cause the doc to move shards "
                           "must be sent with write batch of size 1"});
        }

        return boost::none;
    } else {
        for (const auto& err : response->getErrDetails()) {
            if (err.getStatus() != ErrorCodes::WouldChangeOwningShard) {
                continue;
            }

            BSONObjBuilder extraInfoBuilder;
            err.getStatus().extraInfo()->serialize(&extraInfoBuilder);
            auto extraInfo = extraInfoBuilder.obj();
            return WouldChangeOwningShardInfo::parseFromCommandError(extraInfo);
        }

        return boost::none;
    }
}

void handleWouldChangeOwningShardErrorNonTransaction(OperationContext* opCtx,
                                                     BatchedCommandRequest* request,
                                                     BatchedCommandResponse* response) {
    // Strip write concern because this command will be sent as part of a
    // transaction and the write concern has already been loaded onto the opCtx and
    // will be picked up by the transaction API.
    request->unsetWriteConcern();

    // Strip runtime constants because they will be added again when the API sends this command
    // through the service entry point.
    request->unsetLegacyRuntimeConstants();

    // Unset error details because they will be repopulated below.
    response->unsetErrDetails();

    auto& executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
    auto inlineExecutor = std::make_shared<executor::InlineExecutor>();
    auto sleepInlineExecutor = inlineExecutor->getSleepableExecutor(executor);

    auto txn = txn_api::SyncTransactionWithRetries(
        opCtx, sleepInlineExecutor, nullptr /* resourceYielder */, inlineExecutor);

    // Shared state for the transaction API use below.
    struct SharedBlock {
        SharedBlock(BSONObj cmdObj_, NamespaceString nss_) : cmdObj(cmdObj_), nss(nss_) {}

        BSONObj cmdObj;
        NamespaceString nss;
        BSONObj response;
    };
    BSONObjBuilder cmdWithStmtId(request->toBSON());
    cmdWithStmtId.append(write_ops::WriteCommandRequestBase::kStmtIdFieldName, 0);
    auto sharedBlock = std::make_shared<SharedBlock>(cmdWithStmtId.obj(), request->getNS());

    auto swCommitResult = txn.runNoThrow(
        opCtx, [sharedBlock](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            return txnClient.runCommand(sharedBlock->nss.dbName(), sharedBlock->cmdObj)
                .thenRunOn(txnExec)
                .then([sharedBlock](auto res) {
                    uassertStatusOK(getStatusFromWriteCommandReply(res));

                    sharedBlock->response = CommandHelpers::filterCommandReplyForPassthrough(
                        res.removeField("recoveryToken"));
                })
                .semi();
        });

    auto bodyStatus = swCommitResult.getStatus();
    if (!bodyStatus.isOK()) {
        if (bodyStatus != ErrorCodes::DuplicateKey ||
            (bodyStatus == ErrorCodes::DuplicateKey &&
             !bodyStatus.extraInfo<DuplicateKeyErrorInfo>()->getKeyPattern().hasField("_id"))) {
            bodyStatus.addContext(documentShardKeyUpdateUtil::kNonDuplicateKeyErrorContext);
        }

        response->addToErrDetails({0, bodyStatus});
        return;
    }

    uassertStatusOK(swCommitResult.getValue().cmdStatus);

    // Note this will clear existing response as part of parsing.
    std::string errMsg = "Failed to parse response from WouldChangeOwningShard error handling";
    response->parseBSON(sharedBlock->response, &errMsg);

    // Make a unique pointer with a copy of the error detail because
    // BatchedCommandResponse::setWriteConcernError() expects a pointer to a heap allocated
    // WriteConcernErrorDetail that it can take unique ownership of.
    auto writeConcernDetail =
        std::make_unique<WriteConcernErrorDetail>(swCommitResult.getValue().wcError);
    if (!writeConcernDetail->toStatus().isOK()) {
        response->setWriteConcernError(writeConcernDetail.release());
    }
}

struct UpdateShardKeyResult {
    bool updatedShardKey{false};
    boost::optional<BSONObj> upsertedId;
};

UpdateShardKeyResult handleWouldChangeOwningShardErrorTransaction(
    OperationContext* opCtx,
    BatchedCommandRequest* request,
    BatchedCommandResponse* response,
    const WouldChangeOwningShardInfo& changeInfo) {
    // Shared state for the transaction API use below.
    struct SharedBlock {
        SharedBlock(WouldChangeOwningShardInfo changeInfo_, NamespaceString nss_)
            : changeInfo(changeInfo_), nss(nss_) {}

        WouldChangeOwningShardInfo changeInfo;
        NamespaceString nss;
        bool updatedShardKey{false};
    };
    auto sharedBlock = std::make_shared<SharedBlock>(changeInfo, request->getNS());
    auto& executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
    auto inlineExecutor = std::make_shared<executor::InlineExecutor>();
    auto sleepInlineExecutor = inlineExecutor->getSleepableExecutor(executor);
    auto txn =
        txn_api::SyncTransactionWithRetries(opCtx,
                                            sleepInlineExecutor,
                                            TransactionRouterResourceYielder::makeForLocalHandoff(),
                                            inlineExecutor);

    try {
        txn.run(opCtx,
                [sharedBlock](const txn_api::TransactionClient& txnClient,
                              ExecutorPtr txnExec) -> SemiFuture<void> {
                    return documentShardKeyUpdateUtil::updateShardKeyForDocument(
                               txnClient, txnExec, sharedBlock->nss, sharedBlock->changeInfo)
                        .thenRunOn(txnExec)
                        .then([sharedBlock](bool updatedShardKey) {
                            sharedBlock->updatedShardKey = updatedShardKey;
                        })
                        .semi();
                });
    } catch (const ExceptionFor<ErrorCodes::DuplicateKey>& ex) {
        Status status = ex->getKeyPattern().hasField("_id")
            ? ex.toStatus().withContext(documentShardKeyUpdateUtil::kDuplicateKeyErrorContext)
            : ex.toStatus();
        uassertStatusOK(status);
    }

    // If the operation was an upsert, record the _id of the new document.
    boost::optional<BSONObj> upsertedId;
    if (sharedBlock->updatedShardKey && sharedBlock->changeInfo.getShouldUpsert()) {
        upsertedId = sharedBlock->changeInfo.getPostImage()["_id"].wrap();
    }
    return UpdateShardKeyResult{sharedBlock->updatedShardKey, std::move(upsertedId)};
}

void updateHostsTargetedMetrics(OperationContext* opCtx,
                                BatchedCommandRequest::BatchType batchType,
                                int nShardsOwningChunks,
                                int nShardsTargeted) {
    NumHostsTargetedMetrics::QueryType writeType;
    switch (batchType) {
        case BatchedCommandRequest::BatchType_Insert:
            writeType = NumHostsTargetedMetrics::QueryType::kInsertCmd;
            break;
        case BatchedCommandRequest::BatchType_Update:
            writeType = NumHostsTargetedMetrics::QueryType::kUpdateCmd;
            break;
        case BatchedCommandRequest::BatchType_Delete:
            writeType = NumHostsTargetedMetrics::QueryType::kDeleteCmd;
            break;

            MONGO_UNREACHABLE;
    }

    auto targetType = NumHostsTargetedMetrics::get(opCtx).parseTargetType(
        opCtx, nShardsTargeted, nShardsOwningChunks);
    NumHostsTargetedMetrics::get(opCtx).addNumHostsTargeted(writeType, targetType);
}

}  // namespace

bool ClusterWriteCmd::handleWouldChangeOwningShardError(OperationContext* opCtx,
                                                        BatchedCommandRequest* request,
                                                        BatchedCommandResponse* response,
                                                        BatchWriteExecStats stats) {
    auto txnRouter = TransactionRouter::get(opCtx);
    bool isRetryableWrite = opCtx->getTxnNumber() && !txnRouter;

    auto wouldChangeOwningShardErrorInfo =
        getWouldChangeOwningShardErrorInfo(opCtx, *request, response, !isRetryableWrite);
    if (!wouldChangeOwningShardErrorInfo)
        return false;

    bool updatedShardKey = false;
    boost::optional<BSONObj> upsertedId;

    if (feature_flags::gFeatureFlagUpdateDocumentShardKeyUsingTransactionApi.isEnabled(
            serverGlobalParams.featureCompatibility)) {
        if (txnRouter) {
            auto updateResult = handleWouldChangeOwningShardErrorTransaction(
                opCtx, request, response, *wouldChangeOwningShardErrorInfo);
            updatedShardKey = updateResult.updatedShardKey;
            upsertedId = std::move(updateResult.upsertedId);
        } else {
            // Updating a document's shard key such that its owning shard changes must be run in a
            // transaction. If this update is not already in a transaction, complete the update
            // using an internal transaction.
            if (isRetryableWrite) {
                if (MONGO_unlikely(
                        hangAfterThrowWouldChangeOwningShardRetryableWrite.shouldFail())) {
                    LOGV2(5918603,
                          "Hit hangAfterThrowWouldChangeOwningShardRetryableWrite failpoint");
                    hangAfterThrowWouldChangeOwningShardRetryableWrite.pauseWhileSet(opCtx);
                }
            }
            handleWouldChangeOwningShardErrorNonTransaction(opCtx, request, response);
        }
    } else {
        // TODO SERVER-67429: Delete this branch.
        opCtx->setQuerySamplingOptions(QuerySamplingOptions::kOptOut);

        if (isRetryableWrite) {
            if (MONGO_unlikely(hangAfterThrowWouldChangeOwningShardRetryableWrite.shouldFail())) {
                LOGV2(22759, "Hit hangAfterThrowWouldChangeOwningShardRetryableWrite failpoint");
                hangAfterThrowWouldChangeOwningShardRetryableWrite.pauseWhileSet(opCtx);
            }
            RouterOperationContextSession routerSession(opCtx);
            try {
                // Start transaction and re-run the original update command
                auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
                readConcernArgs = repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);

                // Ensure the retried operation does not include WC inside the transaction.  The
                // transaction commit will still use the WC, because it uses the WC from the opCtx
                // (which has been set previously in Strategy).
                request->unsetWriteConcern();

                documentShardKeyUpdateUtil::startTransactionForShardKeyUpdate(opCtx);
                // Clear the error details from the response object before sending the write again
                response->unsetErrDetails();

                cluster::write(opCtx, *request, &stats, response);
                wouldChangeOwningShardErrorInfo = getWouldChangeOwningShardErrorInfo(
                    opCtx, *request, response, !isRetryableWrite);
                if (!wouldChangeOwningShardErrorInfo)
                    uassertStatusOK(response->toStatus());

                // If we do not get WouldChangeOwningShard when re-running the update, the document
                // has been modified or deleted concurrently and we do not need to delete it and
                // insert a new one.
                updatedShardKey = wouldChangeOwningShardErrorInfo &&
                    documentShardKeyUpdateUtil::updateShardKeyForDocumentLegacy(
                                      opCtx, request->getNS(), *wouldChangeOwningShardErrorInfo);

                // If the operation was an upsert, record the _id of the new document.
                if (updatedShardKey && wouldChangeOwningShardErrorInfo->getShouldUpsert()) {
                    upsertedId = wouldChangeOwningShardErrorInfo->getPostImage()["_id"].wrap();
                }

                // Commit the transaction
                auto commitResponse =
                    documentShardKeyUpdateUtil::commitShardKeyUpdateTransaction(opCtx);

                uassertStatusOK(getStatusFromCommandResult(commitResponse));

                auto writeConcernDetail = getWriteConcernErrorDetailFromBSONObj(commitResponse);
                if (writeConcernDetail && !writeConcernDetail->toStatus().isOK())
                    response->setWriteConcernError(writeConcernDetail.release());
            } catch (DBException& e) {
                if (e.code() == ErrorCodes::DuplicateKey &&
                    e.extraInfo<DuplicateKeyErrorInfo>()->getKeyPattern().hasField("_id")) {
                    e.addContext(documentShardKeyUpdateUtil::kDuplicateKeyErrorContext);
                } else {
                    e.addContext(documentShardKeyUpdateUtil::kNonDuplicateKeyErrorContext);
                }

                if (!response->isErrDetailsSet()) {
                    response->addToErrDetails(
                        {0,
                         Status(ErrorCodes::InternalError, "Will be replaced by the code below")});
                }

                // Set the error status to the status of the failed command and abort the
                // transaction
                auto status = e.toStatus();
                response->getErrDetails().back().setStatus(status);

                auto txnRouterForAbort = TransactionRouter::get(opCtx);
                if (txnRouterForAbort)
                    txnRouterForAbort.implicitlyAbortTransaction(opCtx, status);

                return false;
            }
        } else {
            try {
                // Delete the original document and insert the new one
                updatedShardKey = documentShardKeyUpdateUtil::updateShardKeyForDocumentLegacy(
                    opCtx, request->getNS(), *wouldChangeOwningShardErrorInfo);

                // If the operation was an upsert, record the _id of the new document.
                if (updatedShardKey && wouldChangeOwningShardErrorInfo->getShouldUpsert()) {
                    upsertedId = wouldChangeOwningShardErrorInfo->getPostImage()["_id"].wrap();
                }
            } catch (const ExceptionFor<ErrorCodes::DuplicateKey>& ex) {
                Status status = ex->getKeyPattern().hasField("_id")
                    ? ex.toStatus().withContext(
                          documentShardKeyUpdateUtil::kDuplicateKeyErrorContext)
                    : ex.toStatus();
                uassertStatusOK(status);
            }
        }
    }

    if (updatedShardKey) {
        // If we get here, the batch size is 1 and we have successfully deleted the old doc
        // and inserted the new one, so it is safe to unset the error details.
        response->unsetErrDetails();
        response->setN(response->getN() + 1);

        if (upsertedId) {
            auto upsertDetail = std::make_unique<BatchedUpsertDetail>();
            upsertDetail->setIndex(0);
            upsertDetail->setUpsertedID(upsertedId.value());
            response->addToUpsertDetails(upsertDetail.release());
        } else {
            response->setNModified(response->getNModified() + 1);
        }
    }

    return updatedShardKey;
}

void ClusterWriteCmd::_commandOpWrite(OperationContext* opCtx,
                                      const NamespaceString& nss,
                                      const BSONObj& command,
                                      BatchItemRef targetingBatchItem,
                                      std::vector<AsyncRequestsSender::Response>* results) {
    auto endpoints = [&] {
        // Note that this implementation will not handle targeting retries and does not
        // completely emulate write behavior
        CollectionRoutingInfoTargeter targeter(opCtx, nss);

        if (targetingBatchItem.getOpType() == BatchedCommandRequest::BatchType_Insert) {
            return std::vector{targeter.targetInsert(opCtx, targetingBatchItem.getDocument())};
        } else if (targetingBatchItem.getOpType() == BatchedCommandRequest::BatchType_Update) {
            return targeter.targetUpdate(opCtx, targetingBatchItem);
        } else if (targetingBatchItem.getOpType() == BatchedCommandRequest::BatchType_Delete) {
            return targeter.targetDelete(opCtx, targetingBatchItem);
        }
        MONGO_UNREACHABLE;
    }();

    // Assemble requests
    std::vector<AsyncRequestsSender::Request> requests;
    for (const auto& endpoint : endpoints) {
        BSONObj cmdObjWithVersions = BSONObj(command);
        if (endpoint.databaseVersion) {
            cmdObjWithVersions =
                appendDbVersionIfPresent(cmdObjWithVersions, *endpoint.databaseVersion);
        }
        if (endpoint.shardVersion) {
            cmdObjWithVersions = appendShardVersion(cmdObjWithVersions, *endpoint.shardVersion);
        }
        requests.emplace_back(endpoint.shardName, cmdObjWithVersions);
    }

    // Send the requests.

    const ReadPreferenceSetting readPref(ReadPreference::PrimaryOnly, TagSet());
    MultiStatementTransactionRequestsSender ars(
        opCtx,
        Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
        nss.dbName(),
        requests,
        readPref,
        Shard::RetryPolicy::kNoRetry);

    while (!ars.done()) {
        // Block until a response is available.
        auto response = ars.next();
        uassertStatusOK(response.swResponse);

        // If the response status was OK, the response must contain which host was targeted.
        invariant(response.shardHostAndPort);
        results->push_back(response);
    }
}

bool ClusterWriteCmd::InvocationBase::runImpl(OperationContext* opCtx,
                                              const OpMsgRequest& request,
                                              BatchedCommandRequest& batchedRequest,
                                              BSONObjBuilder& result) const {
    BatchWriteExecStats stats;
    BatchedCommandResponse response;

    // The batched request will only have WC if it was supplied by the client. Otherwise, the
    // batched request should use the WC from the opCtx.
    if (!batchedRequest.hasWriteConcern()) {
        batchedRequest.setWriteConcern(opCtx->getWriteConcern().toBSON());
    }

    // Write ops are never allowed to have writeConcern inside transactions. Normally
    // disallowing WC on non-terminal commands in a transaction is handled earlier, during
    // command dispatch. However, if this is a regular write operation being automatically
    // retried inside a transaction (such as changing a document's shard key across shards),
    // then batchedRequest will have a writeConcern (added by the if() above) from when it was
    // initially run outside a transaction. Thus it's necessary to unconditionally clear the
    // writeConcern when in a transaction.
    if (TransactionRouter::get(opCtx)) {
        batchedRequest.unsetWriteConcern();
    }

    cluster::write(opCtx, batchedRequest, &stats, &response);

    bool updatedShardKey = false;
    if (_batchedRequest.getBatchType() == BatchedCommandRequest::BatchType_Update) {
        updatedShardKey =
            handleWouldChangeOwningShardError(opCtx, &batchedRequest, &response, stats);
    }

    // Populate the 'NotPrimaryErrorTracker' object based on the write response
    batchErrorToNotPrimaryErrorTracker(
        batchedRequest, response, &NotPrimaryErrorTracker::get(opCtx->getClient()));
    size_t numAttempts;

    if (!response.getOk()) {
        numAttempts = 0;
    } else if (batchedRequest.getWriteCommandRequestBase().getOrdered() &&
               response.isErrDetailsSet()) {
        // Add one failed attempt
        numAttempts = response.getErrDetailsAt(0).getIndex() + 1;
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

            // The response.getN() count is the sum of documents matched and upserted.
            if (response.isUpsertDetailsSet()) {
                debug.additiveMetrics.nMatched = response.getN() - response.sizeUpsertDetails();
                debug.additiveMetrics.nUpserted = response.sizeUpsertDetails();
            } else {
                debug.additiveMetrics.nMatched = response.getN();
            }
            debug.additiveMetrics.nModified = response.getNModified();

            invariant(_updateMetrics);
            for (auto&& update : _batchedRequest.getUpdateRequest().getUpdates()) {
                // If this was a pipeline style update, record that pipeline-style was used and
                // which stages were being used.
                auto updateMod = update.getU();
                if (updateMod.type() == write_ops::UpdateModification::Type::kPipeline) {
                    auto pipeline =
                        LiteParsedPipeline(_batchedRequest.getNS(), updateMod.getUpdatePipeline());
                    pipeline.tickGlobalStageCounters();
                    _updateMetrics->incrementExecutedWithAggregationPipeline();
                }

                // If this command had arrayFilters option, record that it was used.
                if (update.getArrayFilters()) {
                    _updateMetrics->incrementExecutedWithArrayFilters();
                }
            }
            break;
        case BatchedCommandRequest::BatchType_Delete:
            for (size_t i = 0; i < numAttempts; ++i) {
                globalOpCounters.gotDelete();
            }
            debug.additiveMetrics.ndeleted = response.getN();
            break;
    }

    // Record the number of shards targeted by this write.
    CurOp::get(opCtx)->debug().nShards =
        stats.getTargetedShards().size() + (updatedShardKey ? 1 : 0);

    if (stats.getNumShardsOwningChunks().has_value())
        updateHostsTargetedMetrics(opCtx,
                                   _batchedRequest.getBatchType(),
                                   stats.getNumShardsOwningChunks().value(),
                                   stats.getTargetedShards().size() + (updatedShardKey ? 1 : 0));

    if (auto txnRouter = TransactionRouter::get(opCtx)) {
        auto writeCmdStatus = response.toStatus();
        if (!writeCmdStatus.isOK()) {
            txnRouter.implicitlyAbortTransaction(opCtx, writeCmdStatus);
        }
    }

    result.appendElements(response.toBSON());
    return response.getOk();
}

void ClusterWriteCmd::InvocationBase::run(OperationContext* opCtx,
                                          rpc::ReplyBuilderInterface* result) {
    preRunImplHook(opCtx);

    BSONObjBuilder bob = result->getBodyBuilder();
    bool ok = runImpl(opCtx, *_request, _batchedRequest, bob);
    if (!ok)
        CommandHelpers::appendSimpleCommandStatus(bob, ok);
}

void ClusterWriteCmd::InvocationBase::explain(OperationContext* opCtx,
                                              ExplainOptions::Verbosity verbosity,
                                              rpc::ReplyBuilderInterface* result) {
    preExplainImplHook(opCtx);

    uassert(ErrorCodes::InvalidLength,
            "explained write batches must be of size 1",
            _batchedRequest.sizeWriteOps() == 1U);


    std::unique_ptr<BatchedCommandRequest> req;
    if (_batchedRequest.hasEncryptionInformation() &&
        (_batchedRequest.getBatchType() == BatchedCommandRequest::BatchType_Delete ||
         _batchedRequest.getBatchType() == BatchedCommandRequest::BatchType_Update)) {
        req = processFLEBatchExplain(opCtx, _batchedRequest);
    }

    auto nss = req ? req->getNS() : _batchedRequest.getNS();
    auto requestBSON = req ? req->toBSON() : _request->body;
    auto requestPtr = req ? req.get() : &_batchedRequest;

    const auto explainCmd = ClusterExplain::wrapAsExplain(requestBSON, verbosity);

    // We will time how long it takes to run the commands on the shards.
    Timer timer;

    // Target the command to the shards based on the singleton batch item.
    BatchItemRef targetingBatchItem(requestPtr, 0);
    std::vector<AsyncRequestsSender::Response> shardResponses;
    _commandOpWrite(opCtx, nss, explainCmd, targetingBatchItem, &shardResponses);
    auto bodyBuilder = result->getBodyBuilder();
    uassertStatusOK(ClusterExplain::buildExplainResult(opCtx,
                                                       shardResponses,
                                                       ClusterExplain::kWriteOnShards,
                                                       timer.millis(),
                                                       requestBSON,
                                                       &bodyBuilder));
}

}  // namespace mongo
