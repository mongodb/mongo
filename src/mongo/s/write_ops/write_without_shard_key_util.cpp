/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/s/write_ops/write_without_shard_key_util.h"

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/smart_ptr.hpp>
#include <memory>
#include <string>
#include <vector>

#include <boost/optional/optional.hpp>

#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/exec/mutable_bson/document.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/field_ref_set.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/collation/collation_index_key.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/write_ops/update_request.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/resource_yielder.h"
#include "mongo/db/server_options.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/timeseries/timeseries_update_delete_util.h"
#include "mongo/db/timeseries/timeseries_write_util.h"
#include "mongo/db/transaction/transaction_api.h"
#include "mongo/db/update/update_driver.h"
#include "mongo/db/update/update_util.h"
#include "mongo/executor/inline_executor.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/collection_routing_info_targeter.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/cluster_commands_without_shard_key_gen.h"
#include "mongo/s/shard_key_pattern_query_util.h"
#include "mongo/s/transaction_router_resource_yielder.h"
#include "mongo/s/type_collection_common_types_gen.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/out_of_line_executor.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace write_without_shard_key {
namespace {

constexpr auto kIdFieldName = "_id"_sd;
const FieldRef idFieldRef(kIdFieldName);

bool shardKeyHasCollatableType(const BSONObj& shardKey) {
    for (BSONElement elt : shardKey) {
        if (CollationIndexKey::isCollatableType(elt.type())) {
            return true;
        }
    }
    return false;
}
}  // namespace

std::pair<BSONObj, BSONObj> generateUpsertDocument(
    OperationContext* opCtx,
    const UpdateRequest& updateRequest,
    const UUID& collectionUUID,
    boost::optional<TimeseriesOptions> timeseriesOptions,
    const StringDataComparator* comparator) {
    // We are only using this to parse the query for producing the upsert document.
    ParsedUpdateForMongos parsedUpdate(opCtx, &updateRequest);
    uassertStatusOK(parsedUpdate.parseRequest());

    const CanonicalQuery* canonicalQuery =
        parsedUpdate.hasParsedQuery() ? parsedUpdate.getParsedQuery() : nullptr;
    FieldRefSet immutablePaths;
    immutablePaths.insert(&idFieldRef);
    update::produceDocumentForUpsert(opCtx,
                                     &updateRequest,
                                     parsedUpdate.getDriver(),
                                     canonicalQuery,
                                     immutablePaths,
                                     parsedUpdate.getDriver()->getDocument());

    auto upsertDoc = parsedUpdate.getDriver()->getDocument().getObject();
    if (!timeseriesOptions) {
        return {upsertDoc, BSONObj()};
    }

    tassert(7777500,
            "Expected timeseries buckets collection namespace",
            updateRequest.getNamespaceString().isTimeseriesBucketsCollection());
    auto upsertBucketObj = timeseries::makeBucketDocument(std::vector{upsertDoc},
                                                          updateRequest.getNamespaceString(),
                                                          collectionUUID,
                                                          *timeseriesOptions,
                                                          comparator);
    return {upsertBucketObj, upsertDoc};
}

BSONObj constructUpsertResponse(BatchedCommandResponse& writeRes,
                                const BSONObj& targetDoc,
                                StringData commandName,
                                bool appendPostImage) {
    BSONObj reply;
    auto upsertedId = IDLAnyTypeOwned::parseFromBSON(targetDoc.getField(kIdFieldName));

    if (commandName == BulkWriteCommandRequest::kCommandName) {
        BulkWriteReplyItem replyItem(0);
        replyItem.setOk(1);
        replyItem.setN(writeRes.getN());
        replyItem.setUpserted(upsertedId);
        BulkWriteCommandReply bulkWriteReply(
            BulkWriteCommandResponseCursor(
                0, {replyItem}, NamespaceString::makeBulkWriteNSS(boost::none)),
            0 /* nErrors */,
            0 /* nInserted */,
            writeRes.getN() /* nMatched */,
            0 /* nModified */,
            1 /* nUpserted */,
            0 /* nDeleted */);
        reply = bulkWriteReply.toBSON();
    } else if (commandName == write_ops::FindAndModifyCommandRequest::kCommandName ||
               commandName == write_ops::FindAndModifyCommandRequest::kCommandAlias) {
        write_ops::FindAndModifyLastError lastError;
        lastError.setNumDocs(writeRes.getN());
        lastError.setUpdatedExisting(false);
        lastError.setUpserted(upsertedId);

        write_ops::FindAndModifyCommandReply result;
        result.setLastErrorObject(std::move(lastError));
        if (appendPostImage) {
            result.setValue(targetDoc);
        }

        reply = result.toBSON();
    } else {
        write_ops::UpdateCommandReply updateReply = write_ops::UpdateCommandReply::parse(
            IDLParserContext("upsertWithoutShardKeyResult"), writeRes.toBSON());
        write_ops::Upserted upsertedType;

        // It is guaranteed that the index of this update is 0 since shards evaluate one
        // targetedWrite per batch in a singleWriteWithoutShardKey.
        upsertedType.setIndex(0);
        upsertedType.set_id(upsertedId);
        updateReply.setUpserted(std::vector<mongo::write_ops::Upserted>{upsertedType});

        reply = updateReply.toBSON();
    }

    BSONObjBuilder responseBob(reply);
    responseBob.append("ok", 1);

    BSONObjBuilder bob;
    bob.append("response", responseBob.obj());
    bob.append("shardId", "");

    return bob.obj();
}

bool useTwoPhaseProtocol(OperationContext* opCtx,
                         NamespaceString nss,
                         bool isUpdateOrDelete,
                         bool isUpsert,
                         const BSONObj& query,
                         const BSONObj& collation,
                         const boost::optional<BSONObj>& let,
                         const boost::optional<LegacyRuntimeConstants>& legacyRuntimeConstants,
                         bool isTimeseriesViewRequest) {
    auto cri =
        uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));

    // Unsharded collections always target one single shard.
    if (!cri.cm.isSharded()) {
        return false;
    }

    auto tsFields = cri.cm.getTimeseriesFields();

    // updateOne and deleteOne do not use the two phase protocol for single writes that specify
    // _id in their queries, unless a document is being upserted. An exact _id match requires
    // default collation if the _id value is a collatable type.
    if (isUpdateOrDelete && query.hasField("_id") &&
        CollectionRoutingInfoTargeter::isExactIdQuery(opCtx, nss, query, collation, cri.cm) &&
        !isUpsert && !isTimeseriesViewRequest) {
        return false;
    }

    auto expCtx = makeExpressionContextWithDefaultsForTargeter(opCtx,
                                                               nss,
                                                               cri,
                                                               collation,
                                                               boost::none,  // explain
                                                               let,
                                                               legacyRuntimeConstants);

    bool arbitraryTimeseriesWritesEnabled =
        feature_flags::gTimeseriesDeletesSupport.isEnabled(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot()) ||
        feature_flags::gTimeseriesUpdatesSupport.isEnabled(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot());
    auto shardKey = uassertStatusOK(extractShardKeyFromBasicQueryWithContext(
        expCtx,
        cri.cm.getShardKeyPattern(),
        !isTimeseriesViewRequest
            ? query
            : timeseries::getBucketLevelPredicateForRouting(query,
                                                            expCtx,
                                                            tsFields->getTimeseriesOptions(),
                                                            arbitraryTimeseriesWritesEnabled)));

    // 'shardKey' will only be populated only if a full equality shard key is extracted.
    if (shardKey.isEmpty()) {
        return true;
    } else {
        // Check if the query has specified a different collation than the default collation.
        auto hasDefaultCollation = [&] {
            if (collation.isEmpty()) {
                return true;
            }
            auto collator = uassertStatusOK(
                CollatorFactoryInterface::get(opCtx->getServiceContext())->makeFromBSON(collation));
            return CollatorInterface::collatorsMatch(collator.get(), cri.cm.getDefaultCollator());
        }();

        // If the default collection collation is not used or the default collation is not the
        // simple collation and any field of the shard key is a collatable type, then we will use
        // the two phase write protocol since we cannot target directly to a shard.
        if ((!hasDefaultCollation || cri.cm.getDefaultCollator()) &&
            shardKeyHasCollatableType(shardKey)) {
            return true;
        } else {
            return false;
        }
    }

    return true;
}

StatusWith<ClusterWriteWithoutShardKeyResponse> runTwoPhaseWriteProtocol(OperationContext* opCtx,
                                                                         NamespaceString nss,
                                                                         BSONObj cmdObj) {
    if (opCtx->isRetryableWrite()) {
        tassert(7260900,
                "Retryable writes must have an explicit stmtId",
                cmdObj.hasField(write_ops::WriteCommandRequestBase::kStmtIdsFieldName) ||
                    cmdObj.hasField(write_ops::WriteCommandRequestBase::kStmtIdFieldName));
    }

    // Shared state for the transaction below.
    struct SharedBlock {
        SharedBlock(NamespaceString nss_, BSONObj cmdObj_)
            : nss(std::move(nss_)), cmdObj(cmdObj_) {}
        NamespaceString nss;
        BSONObj cmdObj;
        ClusterWriteWithoutShardKeyResponse clusterWriteResponse;
    };

    auto& executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
    auto inlineExecutor = std::make_shared<executor::InlineExecutor>();

    auto txn = txn_api::SyncTransactionWithRetries(
        opCtx, executor, TransactionRouterResourceYielder::makeForLocalHandoff(), inlineExecutor);

    auto sharedBlock = std::make_shared<SharedBlock>(nss, cmdObj);
    auto swResult = txn.runNoThrow(
        opCtx, [sharedBlock](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            ClusterQueryWithoutShardKey clusterQueryWithoutShardKeyCommand(sharedBlock->cmdObj);

            auto queryRes = txnClient.runCommandSync(sharedBlock->nss.dbName(),
                                                     clusterQueryWithoutShardKeyCommand.toBSON());

            uassertStatusOK(getStatusFromCommandResult(queryRes));

            ClusterQueryWithoutShardKeyResponse queryResponse =
                ClusterQueryWithoutShardKeyResponse::parseOwned(
                    IDLParserContext("_clusterQueryWithoutShardKeyResponse"), std::move(queryRes));

            // The target document can contain the target document's _id or a generated upsert
            // document. If there's no targetDocument, then no modification needs to be made.
            if (!queryResponse.getTargetDoc()) {
                return SemiFuture<void>::makeReady();
            }

            // If upsertRequired, insert target document directly into the database.
            if (queryResponse.getUpsertRequired()) {
                std::vector<BSONObj> docs;
                docs.push_back(queryResponse.getTargetDoc().get());
                write_ops::InsertCommandRequest insertRequest(sharedBlock->nss, docs);

                // Append "encryptionInformation" if the original command is an encrypted command.
                boost::optional<EncryptionInformation> encryptionInformation;
                if (sharedBlock->cmdObj.hasField(
                        write_ops::WriteCommandRequestBase::kEncryptionInformationFieldName)) {
                    encryptionInformation = EncryptionInformation(BSONObj());
                    encryptionInformation->setCrudProcessed(true);
                }
                insertRequest.getWriteCommandRequestBase().setEncryptionInformation(
                    encryptionInformation);

                if (sharedBlock->cmdObj.hasField(
                        write_ops::WriteCommandRequestBase::kBypassEmptyTsReplacementFieldName)) {
                    auto bypassEmptyTsReplacementField = sharedBlock->cmdObj.getField(
                        write_ops::WriteCommandRequestBase::kBypassEmptyTsReplacementFieldName);

                    if (bypassEmptyTsReplacementField.type() == BSONType::Bool) {
                        insertRequest.getWriteCommandRequestBase().setBypassEmptyTsReplacement(
                            bypassEmptyTsReplacementField.Bool());
                    }
                }

                auto writeRes = txnClient.runCRUDOpSync(insertRequest,
                                                        std::vector<StmtId>{kUninitializedStmtId});

                auto upsertResponse = constructUpsertResponse(
                    writeRes,
                    queryResponse.getUserUpsertDocForTimeseries()
                        ? queryResponse.getUserUpsertDocForTimeseries().get()
                        : queryResponse.getTargetDoc().get(),
                    sharedBlock->cmdObj.firstElementFieldNameStringData(),
                    sharedBlock->cmdObj.getBoolField("new"));

                sharedBlock->clusterWriteResponse = ClusterWriteWithoutShardKeyResponse::parseOwned(
                    IDLParserContext("_clusterWriteWithoutShardKeyResponse"),
                    std::move(upsertResponse));
            } else {
                BSONObjBuilder bob(sharedBlock->cmdObj);
                ClusterWriteWithoutShardKey clusterWriteWithoutShardKeyCommand(
                    bob.obj(),
                    std::string(*queryResponse.getShardId()) /* shardId */,
                    *queryResponse.getTargetDoc() /* targetDocId */);

                auto writeRes = txnClient.runCommandSync(
                    sharedBlock->nss.dbName(), clusterWriteWithoutShardKeyCommand.toBSON());
                uassertStatusOK(getStatusFromCommandResult(writeRes));

                sharedBlock->clusterWriteResponse = ClusterWriteWithoutShardKeyResponse::parseOwned(
                    IDLParserContext("_clusterWriteWithoutShardKeyResponse"), std::move(writeRes));
            }
            uassertStatusOK(
                getStatusFromWriteCommandReply(sharedBlock->clusterWriteResponse.getResponse()));

            return SemiFuture<void>::makeReady();
        });

    if (swResult.isOK()) {
        if (swResult.getValue().getEffectiveStatus().isOK()) {
            return StatusWith<ClusterWriteWithoutShardKeyResponse>(
                sharedBlock->clusterWriteResponse);
        } else {
            return StatusWith<ClusterWriteWithoutShardKeyResponse>(
                swResult.getValue().getEffectiveStatus());
        }
    } else {
        return StatusWith<ClusterWriteWithoutShardKeyResponse>(swResult.getStatus());
    }
}

BSONObj generateExplainResponseForTwoPhaseWriteProtocol(
    const BSONObj& clusterQueryWithoutShardKeyExplainObj,
    const BSONObj& clusterWriteWithoutShardKeyExplainObj) {
    // To express the two phase nature of the two phase write protocol, we use the output of the
    // 'Read Phase' explain as the 'inputStage' of the 'Write Phase' explain for both queryPlanner
    // and executionStats sections.
    //
    // An example output would look like:

    //  "queryPlanner" : {
    //      "winningPlan" : {
    // 	        "stage" : "SHARD_WRITE",
    // 	        ...
    //          “inputStage”: {
    // 		        queryPlanner: {
    // 		            winningPlan: {
    // 		                stage: "SHARD_MERGE",
    //                      ...
    //
    //                  }
    //              }
    //          }
    //      }
    //  }
    //
    // executionStats : {
    //     "executionStages" : {
    //         "stage" : "SHARD_WRITE",
    //          ...
    //     },
    //     "inputStage" : {
    //         "stage" : "SHARD_MERGE",
    //             ...
    //      }
    //
    // }
    // ... other explain result fields ...

    auto queryPlannerOutput = [&] {
        auto clusterQueryWithoutShardKeyQueryPlannerObj =
            clusterQueryWithoutShardKeyExplainObj.getObjectField("queryPlanner");
        auto clusterWriteWithoutShardKeyQueryPlannerObj =
            clusterWriteWithoutShardKeyExplainObj.getObjectField("queryPlanner");

        auto winningPlan = clusterWriteWithoutShardKeyQueryPlannerObj.getObjectField("winningPlan");
        BSONObjBuilder newWinningPlanBuilder(winningPlan);
        newWinningPlanBuilder.appendObject("inputStage",
                                           clusterQueryWithoutShardKeyQueryPlannerObj.objdata());
        auto newWinningPlan = newWinningPlanBuilder.obj();

        auto queryPlannerObjNoWinningPlan =
            clusterWriteWithoutShardKeyQueryPlannerObj.removeField("winningPlan");
        BSONObjBuilder newQueryPlannerBuilder(queryPlannerObjNoWinningPlan);
        newQueryPlannerBuilder.appendObject("winningPlan", newWinningPlan.objdata());
        return newQueryPlannerBuilder.obj();
    }();

    auto executionStatsOutput = [&] {
        auto clusterQueryWithoutShardKeyExecutionStatsObj =
            clusterQueryWithoutShardKeyExplainObj.getObjectField("executionStats");
        auto clusterWriteWithoutShardKeyExecutionStatsObj =
            clusterWriteWithoutShardKeyExplainObj.getObjectField("executionStats");

        if (clusterQueryWithoutShardKeyExecutionStatsObj.isEmpty() &&
            clusterWriteWithoutShardKeyExecutionStatsObj.isEmpty()) {
            return BSONObj();
        }

        BSONObjBuilder newExecutionStatsBuilder(clusterWriteWithoutShardKeyExecutionStatsObj);
        newExecutionStatsBuilder.appendObject(
            "inputStage", clusterQueryWithoutShardKeyExecutionStatsObj.objdata());
        return newExecutionStatsBuilder.obj();
    }();

    BSONObjBuilder explainOutputBuilder;
    if (!queryPlannerOutput.isEmpty()) {
        explainOutputBuilder.appendObject("queryPlanner", queryPlannerOutput.objdata());
    }
    if (!executionStatsOutput.isEmpty()) {
        explainOutputBuilder.appendObject("executionStats", executionStatsOutput.objdata());
    }
    // This step is to get 'command', 'serverInfo', and 'serverParamter' fields to return in the
    // final explain output.
    explainOutputBuilder.appendElementsUnique(clusterWriteWithoutShardKeyExplainObj);
    return explainOutputBuilder.obj();
}
}  // namespace write_without_shard_key
}  // namespace mongo
