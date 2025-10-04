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

#include "mongo/s/commands/query_cmd/cluster_write_cmd.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/error_extra_info.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/commands/query_cmd/write_commands_common.h"
#include "mongo/db/curop.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/fle_crud.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/ddl/cluster_ddl.h"
#include "mongo/db/global_catalog/router_role_api/cluster_commands_helpers.h"
#include "mongo/db/global_catalog/router_role_api/collection_routing_info_targeter.h"
#include "mongo/db/internal_transactions_feature_flag_gen.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/db/raw_data_operation.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/sharding_environment/client/num_hosts_targeted_metrics.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/storage/duplicate_key_error_info.h"
#include "mongo/db/transaction/transaction_api.h"
#include "mongo/executor/inline_executor.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/write_concern_error_detail.h"
#include "mongo/s/cluster_write.h"
#include "mongo/s/commands/document_shard_key_update_util.h"
#include "mongo/s/commands/query_cmd/cluster_explain.h"
#include "mongo/s/multi_statement_transaction_requests_sender.h"
#include "mongo/s/request_types/cluster_commands_without_shard_key_gen.h"
#include "mongo/s/transaction_router.h"
#include "mongo/s/transaction_router_resource_yielder.h"
#include "mongo/s/would_change_owning_shard_exception.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/s/write_ops/batched_upsert_detail.h"
#include "mongo/s/write_ops/write_without_shard_key_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/timer.h"

#include <cstddef>
#include <tuple>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {

MONGO_FAIL_POINT_DEFINE(hangAfterThrowWouldChangeOwningShardRetryableWrite);
namespace {

using QuerySamplingOptions = OperationContext::QuerySamplingOptions;

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
    // Strip runtime constants because they will be added again when the API sends this command
    // through the service entry point.
    request->unsetLegacyRuntimeConstants();

    // Unset error details because they will be repopulated below.
    response->unsetErrDetails();

    // Strip out any transaction-related arguments specified in the original request before running
    // it in an internal transaction.
    generic_argument_util::prepareRequestForInternalTransactionPassthrough(
        request->getGenericArguments());

    auto& executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
    auto inlineExecutor = std::make_shared<executor::InlineExecutor>();

    auto txn = txn_api::SyncTransactionWithRetries(
        opCtx, executor, nullptr /* resourceYielder */, inlineExecutor);

    // Shared state for the transaction API use below.
    struct SharedBlock {
        SharedBlock(BSONObj cmdObj_, NamespaceString nss_) : cmdObj(cmdObj_), nss(nss_) {}

        BSONObj cmdObj;
        NamespaceString nss;
        BSONObj response;
    };

    BSONObjBuilder cmdWithStmtId(
        CommandHelpers::filterCommandRequestForPassthrough(request->toBSON()));
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
    const NamespaceString& nss,
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
    auto sharedBlock = std::make_shared<SharedBlock>(changeInfo, nss);
    auto& executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
    auto inlineExecutor = std::make_shared<executor::InlineExecutor>();
    auto txn = txn_api::SyncTransactionWithRetries(
        opCtx, executor, TransactionRouterResourceYielder::makeForLocalHandoff(), inlineExecutor);

    try {
        txn.run(opCtx,
                [sharedBlock, opCtx](const txn_api::TransactionClient& txnClient,
                                     ExecutorPtr txnExec) -> SemiFuture<void> {
                    return documentShardKeyUpdateUtil::updateShardKeyForDocument(
                               txnClient, opCtx, txnExec, sharedBlock->nss, sharedBlock->changeInfo)
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

}  // namespace

bool ClusterWriteCmd::handleWouldChangeOwningShardError(OperationContext* opCtx,
                                                        BatchedCommandRequest* request,
                                                        const NamespaceString& nss,
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
            VersionContext::getDecoration(opCtx),
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        if (txnRouter) {
            auto updateResult = handleWouldChangeOwningShardErrorTransaction(
                opCtx, nss, response, *wouldChangeOwningShardErrorInfo);
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
            std::tie(updatedShardKey, upsertedId) =
                documentShardKeyUpdateUtil::handleWouldChangeOwningShardErrorRetryableWriteLegacy(
                    opCtx,
                    nss,
                    // RerunOriginalWriteFn:
                    [&]() {
                        // Clear the error details from the response object before sending the write
                        // again
                        response->unsetErrDetails();
                        cluster::write(opCtx, *request, nullptr /* nss */, &stats, response);
                        wouldChangeOwningShardErrorInfo = getWouldChangeOwningShardErrorInfo(
                            opCtx, *request, response, !isRetryableWrite);
                        if (!wouldChangeOwningShardErrorInfo)
                            uassertStatusOK(response->toStatus());
                        return wouldChangeOwningShardErrorInfo;
                    },
                    // ProcessWCEFn:
                    [&](std::unique_ptr<WriteConcernErrorDetail> wce) {
                        response->setWriteConcernError(wce.release());
                    },
                    // ProcessWriteErrorFn:
                    [&](DBException& e) {
                        if (!response->isErrDetailsSet()) {
                            response->addToErrDetails(
                                {0,
                                 Status(ErrorCodes::InternalError,
                                        "Will be replaced by the code below")});
                        }
                        // Set the error status to the status of the failed command.
                        auto status = e.toStatus();
                        response->getErrDetails().back().setStatus(status);
                    });
        } else {
            std::tie(updatedShardKey, upsertedId) =
                documentShardKeyUpdateUtil::handleWouldChangeOwningShardErrorTransactionLegacy(
                    opCtx, nss, *wouldChangeOwningShardErrorInfo);
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

void ClusterWriteCmd::commandOpWrite(OperationContext* opCtx,
                                     const NamespaceString& nss,
                                     const BSONObj& command,
                                     BatchItemRef targetingBatchItem,
                                     const CollectionRoutingInfoTargeter& targeter,
                                     std::vector<AsyncRequestsSender::Response>* results) {
    auto endpoints = [&] {
        // Note that this implementation will not handle targeting retries and does not
        // completely emulate write behavior.
        if (targetingBatchItem.getOpType() == BatchedCommandRequest::BatchType_Insert) {
            return std::vector{
                targeter.targetInsert(opCtx, targetingBatchItem.getInsertOp().getDocument())};
        } else if (targetingBatchItem.getOpType() == BatchedCommandRequest::BatchType_Update) {
            return targeter.targetUpdate(opCtx, targetingBatchItem).endpoints;
        } else if (targetingBatchItem.getOpType() == BatchedCommandRequest::BatchType_Delete) {
            return targeter.targetDelete(opCtx, targetingBatchItem).endpoints;
        }
        MONGO_UNREACHABLE;
    }();

    routing_context_utils::runAndValidate(
        targeter.getRoutingCtx(), [&](RoutingContext& routingCtx) {
            // Assemble requests
            std::vector<AsyncRequestsSender::Request> requests;
            requests.reserve(endpoints.size());
            for (const auto& endpoint : endpoints) {
                BSONObj cmdObjWithVersions = BSONObj(command);
                if (endpoint.databaseVersion) {
                    cmdObjWithVersions =
                        appendDbVersionIfPresent(cmdObjWithVersions, *endpoint.databaseVersion);
                }
                if (endpoint.shardVersion) {
                    cmdObjWithVersions =
                        appendShardVersion(cmdObjWithVersions, *endpoint.shardVersion);
                }
                requests.emplace_back(endpoint.shardName, std::move(cmdObjWithVersions));
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

            // Validate the RoutingContext once a request has been scheduled and sent to the shards
            // via the ARS.
            routingCtx.onRequestSentForNss(targeter.getNS());

            while (!ars.done()) {
                // Block until a response is available.
                auto response = ars.next();
                uassertStatusOK(response.swResponse);

                // If the response status was OK, the response must contain which host was targeted.
                invariant(response.shardHostAndPort);
                results->push_back(std::move(response));
            }
        });
}

bool ClusterWriteCmd::runExplainWithoutShardKey(OperationContext* opCtx,
                                                const BatchedCommandRequest& req,
                                                const NamespaceString& originalNss,
                                                ExplainOptions::Verbosity verbosity,
                                                BSONObjBuilder* result) {
    if (req.getBatchType() != BatchedCommandRequest::BatchType_Delete &&
        req.getBatchType() != BatchedCommandRequest::BatchType_Update) {
        return false;
    }

    bool isMultiWrite = false;
    BSONObj query;
    BSONObj collation;
    bool isUpsert = false;
    if (req.getBatchType() == BatchedCommandRequest::BatchType_Update) {
        auto updateOp = req.getUpdateRequest().getUpdates().begin();
        isMultiWrite = updateOp->getMulti();
        query = updateOp->getQ();
        collation = updateOp->getCollation().value_or(BSONObj());
        isUpsert = updateOp->getUpsert();
    } else {
        auto deleteOp = req.getDeleteRequest().getDeletes().begin();
        isMultiWrite = deleteOp->getMulti();
        query = deleteOp->getQ();
        collation = deleteOp->getCollation().value_or(BSONObj());
    }

    if (isMultiWrite) {
        return false;
    }

    sharding::router::CollectionRouter router{opCtx->getServiceContext(), originalNss};
    return router.routeWithRoutingContext(
        opCtx,
        "explain write"_sd,
        [&](OperationContext* opCtx, RoutingContext& originalRoutingCtx) {
            auto translatedReqBSON = req.toBSON();
            auto translatedNss = originalNss;
            const auto targeter = CollectionRoutingInfoTargeter(opCtx, originalNss);
            auto& unusedRoutingCtx = translateNssForRawDataAccordingToRoutingInfo(
                opCtx,
                originalNss,
                targeter,
                originalRoutingCtx,
                [&](const NamespaceString& bucketsNss) {
                    translatedNss = bucketsNss;
                    switch (req.getBatchType()) {
                        case BatchedCommandRequest::BatchType_Insert:
                            translatedReqBSON =
                                rewriteCommandForRawDataOperation<write_ops::InsertCommandRequest>(
                                    translatedReqBSON, translatedNss.coll());
                            break;
                        case BatchedCommandRequest::BatchType_Update:
                            translatedReqBSON =
                                rewriteCommandForRawDataOperation<write_ops::UpdateCommandRequest>(
                                    translatedReqBSON, translatedNss.coll());
                            break;
                        case BatchedCommandRequest::BatchType_Delete:
                            translatedReqBSON =
                                rewriteCommandForRawDataOperation<write_ops::DeleteCommandRequest>(
                                    translatedReqBSON, translatedNss.coll());
                            break;
                        default:
                            MONGO_UNREACHABLE_TASSERT(10370603);
                    }
                });
            unusedRoutingCtx.skipValidation();

            if (!write_without_shard_key::useTwoPhaseProtocol(
                    opCtx,
                    translatedNss,
                    true /* isUpdateOrDelete */,
                    isUpsert,
                    query,
                    collation,
                    req.getLet(),
                    req.getLegacyRuntimeConstants(),
                    false /* isTimeseriesViewRequest */)) {
                return false;
            }

            // Explain currently cannot be run within a transaction, so each command is instead run
            // separately outside of a transaction, and we compose the results at the end.
            auto vts = auth::ValidatedTenancyScope::get(opCtx);
            auto clusterQueryWithoutShardKeyExplainRes = [&] {
                ClusterQueryWithoutShardKey clusterQueryWithoutShardKeyCommand(
                    ClusterExplain::wrapAsExplain(translatedReqBSON, verbosity));
                const auto explainClusterQueryWithoutShardKeyCmd = ClusterExplain::wrapAsExplain(
                    clusterQueryWithoutShardKeyCommand.toBSON(), verbosity);
                auto opMsg = OpMsgRequestBuilder::create(
                    vts, translatedNss.dbName(), explainClusterQueryWithoutShardKeyCmd);
                return CommandHelpers::runCommandDirectly(opCtx, opMsg).getOwned();
            }();

            // Since 'explain' does not return the results of the query, we do not have an _id
            // document to target by from the 'Read Phase'. We instead will use a dummy _id target
            // document 'Write Phase'.
            auto clusterWriteWithoutShardKeyExplainRes = [&] {
                ClusterWriteWithoutShardKey clusterWriteWithoutShardKeyCommand(
                    ClusterExplain::wrapAsExplain(req.toBSON(), verbosity),
                    std::string{
                        clusterQueryWithoutShardKeyExplainRes.getStringField("targetShardId")},
                    write_without_shard_key::targetDocForExplain);
                const auto explainClusterWriteWithoutShardKeyCmd = ClusterExplain::wrapAsExplain(
                    clusterWriteWithoutShardKeyCommand.toBSON(), verbosity);

                auto opMsg = OpMsgRequestBuilder::create(
                    vts, translatedNss.dbName(), explainClusterWriteWithoutShardKeyCmd);
                return CommandHelpers::runCommandDirectly(opCtx, opMsg).getOwned();
            }();

            auto output = write_without_shard_key::generateExplainResponseForTwoPhaseWriteProtocol(
                clusterQueryWithoutShardKeyExplainRes, clusterWriteWithoutShardKeyExplainRes);
            result->appendElementsUnique(output);
            return true;
        });
}

void ClusterWriteCmd::executeWriteOpExplain(OperationContext* opCtx,
                                            const BatchedCommandRequest& batchedRequest,
                                            const BSONObj& requestObj,
                                            ExplainOptions::Verbosity verbosity,
                                            rpc::ReplyBuilderInterface* result) {
    std::unique_ptr<BatchedCommandRequest> req;
    if (batchedRequest.hasEncryptionInformation() &&
        (batchedRequest.getBatchType() == BatchedCommandRequest::BatchType_Delete ||
         batchedRequest.getBatchType() == BatchedCommandRequest::BatchType_Update)) {
        req = processFLEBatchExplain(opCtx, batchedRequest);
    }

    const NamespaceString originalNss = req ? req->getNS() : batchedRequest.getNS();
    auto requestPtr = req ? req.get() : &batchedRequest;
    auto bodyBuilder = result->getBodyBuilder();
    const auto originalRequestBSON = req ? req->toBSON() : requestObj;

    const size_t kMaxDatabaseCreationAttempts = 3;
    size_t attempts = 1;
    while (true) {
        try {
            // Implicitly create the db if it doesn't exist. There is no way right now to return an
            // explain on a sharded cluster if the database doesn't exist.
            // TODO (SERVER-108882) Stop creating the db once explain can be executed when th db
            // doesn't exist.
            cluster::createDatabase(opCtx, originalNss.dbName());

            // If we aren't running an explain for updateOne or deleteOne without shard key,
            // continue and run the original explain path.
            if (runExplainWithoutShardKey(
                    opCtx, batchedRequest, originalNss, verbosity, &bodyBuilder)) {
                return;
            }

            sharding::router::CollectionRouter router{opCtx->getServiceContext(), originalNss};
            router.routeWithRoutingContext(
                opCtx,
                "explain write"_sd,
                [&](OperationContext* opCtx, RoutingContext& originalRoutingCtx) {
                    auto translatedReqBSON = originalRequestBSON;
                    auto translatedNss = originalNss;
                    const auto targeter = CollectionRoutingInfoTargeter(opCtx, originalNss);

                    auto& unusedRoutingCtx = translateNssForRawDataAccordingToRoutingInfo(
                        opCtx,
                        originalNss,
                        targeter,
                        originalRoutingCtx,
                        [&](const NamespaceString& bucketsNss) {
                            translatedNss = bucketsNss;
                            switch (batchedRequest.getBatchType()) {
                                case BatchedCommandRequest::BatchType_Insert:
                                    translatedReqBSON = rewriteCommandForRawDataOperation<
                                        write_ops::InsertCommandRequest>(originalRequestBSON,
                                                                         translatedNss.coll());
                                    break;
                                case BatchedCommandRequest::BatchType_Update:
                                    translatedReqBSON = rewriteCommandForRawDataOperation<
                                        write_ops::UpdateCommandRequest>(originalRequestBSON,
                                                                         translatedNss.coll());
                                    break;
                                case BatchedCommandRequest::BatchType_Delete:
                                    translatedReqBSON = rewriteCommandForRawDataOperation<
                                        write_ops::DeleteCommandRequest>(originalRequestBSON,
                                                                         translatedNss.coll());
                                    break;
                            }
                        });
                    unusedRoutingCtx.skipValidation();

                    const auto explainCmd =
                        ClusterExplain::wrapAsExplain(translatedReqBSON, verbosity);

                    // We will time how long it takes to run the commands on the shards.
                    Timer timer;

                    // Target the command to the shards based on the singleton batch item.
                    BatchItemRef targetingBatchItem(requestPtr, 0);
                    std::vector<AsyncRequestsSender::Response> shardResponses;
                    commandOpWrite(opCtx,
                                   translatedNss,
                                   explainCmd,
                                   std::move(targetingBatchItem),
                                   targeter,
                                   &shardResponses);
                    uassertStatusOK(ClusterExplain::buildExplainResult(
                        makeBlankExpressionContext(opCtx, translatedNss),
                        shardResponses,
                        ClusterExplain::kWriteOnShards,
                        timer.millis(),
                        originalRequestBSON,
                        &bodyBuilder));
                });
            break;
        } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
            LOGV2_INFO(10370602,
                       "Failed initialization of routing info because the database has been "
                       "concurrently dropped",
                       logAttrs(originalNss.dbName()),
                       "attemptNumber"_attr = attempts,
                       "maxAttempts"_attr = kMaxDatabaseCreationAttempts);

            if (++attempts >= kMaxDatabaseCreationAttempts) {
                // The maximum number of attempts has been reached, so the procedure fails as it
                // could be a logical error. At this point, it is unlikely that the error is caused
                // by concurrent drop database operations.
                throw;
            }
        }
    }
}

bool ClusterWriteCmd::InvocationBase::runImpl(OperationContext* opCtx,
                                              const OpMsgRequest& request,
                                              BatchedCommandRequest& batchedRequest,
                                              BSONObjBuilder& result) const {
    BatchWriteExecStats stats;
    BatchedCommandResponse response;

    // If 'batchedRequest' has any let parameters, evaluate them and stash them back on the original
    // request.
    batchedRequest.evaluateAndReplaceLetParams(opCtx);

    // Record the namespace that the write must be run on. It may differ from the request if this is
    // a timeseries collection.
    NamespaceString nss = batchedRequest.getNS();
    cluster::write(opCtx, batchedRequest, &nss, &stats, &response);

    bool updatedShardKey = false;
    if (_batchedRequest.getBatchType() == BatchedCommandRequest::BatchType_Update) {
        updatedShardKey =
            handleWouldChangeOwningShardError(opCtx, &batchedRequest, nss, &response, stats);
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
                serviceOpCounters(opCtx).gotInsert();
            }
            debug.additiveMetrics.ninserted = response.getN();
            break;
        case BatchedCommandRequest::BatchType_Update:
            for (size_t i = 0; i < numAttempts; ++i) {
                serviceOpCounters(opCtx).gotUpdate();
            }

            // The response.getN() count is the sum of documents matched and upserted.
            if (response.isUpsertDetailsSet()) {
                debug.additiveMetrics.nMatched = response.getN() - response.sizeUpsertDetails();
                debug.additiveMetrics.nUpserted = response.sizeUpsertDetails();
            } else {
                debug.additiveMetrics.nMatched = response.getN();
            }
            debug.additiveMetrics.nModified = response.getNModified();

            for (auto&& update : _batchedRequest.getUpdateRequest().getUpdates()) {
                incrementUpdateMetrics(update.getU(),
                                       _batchedRequest.getNS(),
                                       *const_cast<ClusterWriteCmd*>(command())->getUpdateMetrics(),
                                       update.getArrayFilters());
            }
            break;
        case BatchedCommandRequest::BatchType_Delete:
            for (size_t i = 0; i < numAttempts; ++i) {
                serviceOpCounters(opCtx).gotDelete();
            }
            debug.additiveMetrics.ndeleted = response.getN();
            break;
    }

    if (!stats.getIgnore()) {
        int nShards = stats.getTargetedShards().size();

        // If we have no information on the shards targeted, ignore updatedShardKey,
        // updateHostsTargetedMetrics will report this as TargetType::kManyShards.
        if (nShards != 0 && updatedShardKey) {
            nShards += 1;
        }

        // Record the number of shards targeted by this write.
        CurOp::get(opCtx)->debug().nShards = nShards;

        if (stats.getNumShardsOwningChunks().has_value()) {
            updateHostsTargetedMetrics(opCtx,
                                       _batchedRequest.getBatchType(),
                                       stats.getNumShardsOwningChunks().value(),
                                       nShards,
                                       stats.hasTargetedShardedCollection());
        }
    }

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

    executeWriteOpExplain(opCtx, _batchedRequest, _request->body, verbosity, result);
}

}  // namespace mongo
