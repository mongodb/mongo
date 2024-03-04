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
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/bulk_write_common.h"
#include "mongo/db/commands/bulk_write_crud_op.h"
#include "mongo/db/commands/bulk_write_gen.h"
#include "mongo/db/database_name.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/explain_gen.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/internal_transactions_feature_flag_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/ops/write_ops_parsers.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/sort_pattern.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/shard_id.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/db/update/update_util.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/collection_routing_info_targeter.h"
#include "mongo/s/commands/cluster_explain.h"
#include "mongo/s/grid.h"
#include "mongo/s/multi_statement_transaction_requests_sender.h"
#include "mongo/s/query/async_results_merger_params_gen.h"
#include "mongo/s/query/cluster_query_result.h"
#include "mongo/s/query/router_exec_stage.h"
#include "mongo/s/query/router_stage_merge.h"
#include "mongo/s/query/router_stage_remove_metadata_fields.h"
#include "mongo/s/request_types/cluster_commands_without_shard_key_gen.h"
#include "mongo/s/shard_key_pattern_query_util.h"
#include "mongo/s/type_collection_common_types_gen.h"
#include "mongo/s/write_ops/write_without_shard_key_util.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"
#include "mongo/util/timer.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(hangBeforeMetadataRefreshClusterQuery);
constexpr auto kIdFieldName = "_id"_sd;

struct ParsedCommandInfo {
    NamespaceString nss;
    BSONObj query;
    BSONObj collation;
    boost::optional<BSONObj> let;
    boost::optional<BSONObj> sort;
    bool upsert = false;
    int stmtId = kUninitializedStmtId;
    boost::optional<UpdateRequest> updateRequest;
    boost::optional<BSONObj> hint;
    bool isTimeseriesNamespace = false;
};

struct AsyncRequestSenderResponseData {
    ShardId shardId;
    CursorResponse cursorResponse;

    AsyncRequestSenderResponseData(ShardId shardId, CursorResponse cursorResponse)
        : shardId(shardId), cursorResponse(std::move(cursorResponse)) {}
};

// Computes the final sort pattern if necessary metadata is needed.
BSONObj parseSortPattern(OperationContext* opCtx,
                         NamespaceString nss,
                         const ParsedCommandInfo& parsedInfo) {
    std::unique_ptr<CollatorInterface> collator;
    if (!parsedInfo.collation.isEmpty()) {
        collator = uassertStatusOK(CollatorFactoryInterface::get(opCtx->getServiceContext())
                                       ->makeFromBSON(parsedInfo.collation));
    }
    auto expCtx = make_intrusive<ExpressionContext>(opCtx, std::move(collator), nss);
    auto sortPattern = SortPattern(parsedInfo.sort.value_or(BSONObj()), expCtx);
    return sortPattern.serialize(SortPattern::SortKeySerialization::kForSortKeyMerging).toBson();
}

std::set<ShardId> getShardsToTarget(OperationContext* opCtx,
                                    const CollectionRoutingInfo& cri,
                                    NamespaceString nss,
                                    const ParsedCommandInfo& parsedInfo) {
    const auto& cm = cri.cm;
    std::set<ShardId> allShardsContainingChunksForNs;
    uassert(ErrorCodes::NamespaceNotSharded, "The collection was dropped", cm.isSharded());

    auto query = parsedInfo.query;
    auto collation = parsedInfo.collation;
    auto expCtx =
        makeExpressionContextWithDefaultsForTargeter(opCtx,
                                                     nss,
                                                     cri,
                                                     collation,
                                                     boost::none,  // explain
                                                     parsedInfo.let,
                                                     boost::none /* legacyRuntimeConstants */);
    getShardIdsForQuery(
        expCtx, query, collation, cm, &allShardsContainingChunksForNs, nullptr /* info */);

    // We must either get a subset of shards to target in the case of a partial shard key or we must
    // target all shards.
    invariant(allShardsContainingChunksForNs.size() > 0);

    return allShardsContainingChunksForNs;
}

void validateFindAndModifyCommand(const write_ops::FindAndModifyCommandRequest& request) {
    uassert(ErrorCodes::FailedToParse,
            "Either an update or remove=true must be specified",
            request.getRemove().value_or(false) || request.getUpdate());
    if (request.getRemove().value_or(false)) {
        uassert(ErrorCodes::FailedToParse,
                "Cannot specify both an 'update' and 'remove'=true",
                !request.getUpdate());

        uassert(ErrorCodes::FailedToParse,
                "Cannot specify both 'upsert'=true and 'remove'=true ",
                !request.getUpsert() || !*request.getUpsert());

        uassert(
            ErrorCodes::FailedToParse,
            "Cannot specify both 'new'=true and 'remove'=true; 'remove' always returns the deleted "
            "document",
            !request.getNew() || !*request.getNew());

        uassert(ErrorCodes::FailedToParse,
                "Cannot specify 'arrayFilters' and 'remove'=true",
                !request.getArrayFilters());
    }

    if (request.getUpdate() &&
        request.getUpdate()->type() == write_ops::UpdateModification::Type::kPipeline &&
        request.getArrayFilters()) {
        uasserted(ErrorCodes::FailedToParse, "Cannot specify 'arrayFilters' and a pipeline update");
    }
}

BSONObj createAggregateCmdObj(
    OperationContext* opCtx,
    const ParsedCommandInfo& parsedInfo,
    NamespaceString nss,
    const boost::optional<TypeCollectionTimeseriesFields>& timeseriesFields) {
    AggregateCommandRequest aggregate(nss);

    aggregate.setCollation(parsedInfo.collation);
    aggregate.setIsClusterQueryWithoutShardKeyCmd(true);
    aggregate.setFromMongos(true);

    if (parsedInfo.sort) {
        aggregate.setNeedsMerge(true);
    }

    if (parsedInfo.stmtId != kUninitializedStmtId) {
        aggregate.setStmtId(parsedInfo.stmtId);
    }

    if (parsedInfo.hint) {
        aggregate.setHint(parsedInfo.hint);
    }

    aggregate.setLet(parsedInfo.let);

    aggregate.setPipeline([&]() {
        std::vector<BSONObj> pipeline;
        if (timeseriesFields) {
            // We cannot aggregate on the buckets namespace with a query on the timeseries view, so
            // we must generate a bucket unpack stage to correctly aggregate on the time-series
            // collection.
            pipeline.emplace_back(
                timeseries::generateViewPipeline(timeseriesFields->getTimeseriesOptions(), false));
        }
        pipeline.emplace_back(BSON(DocumentSourceMatch::kStageName << parsedInfo.query));
        if (parsedInfo.sort) {
            pipeline.emplace_back(BSON(DocumentSourceSort::kStageName << *parsedInfo.sort));
        }
        pipeline.emplace_back(BSON(DocumentSourceLimit::kStageName << 1));
        pipeline.emplace_back(BSON(DocumentSourceProject::kStageName << BSON(kIdFieldName << 1)));
        return pipeline;
    }());

    return aggregate.toBSON({});
}

ParsedCommandInfo parseWriteRequest(OperationContext* opCtx, const OpMsgRequest& writeReq) {
    const auto& writeCmdObj = writeReq.body;
    auto commandName = writeReq.getCommandName();

    ParsedCommandInfo parsedInfo;

    // For bulkWrite request, we set the nss when we parse the bulkWrite command.
    if (commandName != BulkWriteCommandRequest::kCommandName) {
        parsedInfo.nss =
            CommandHelpers::parseNsCollectionRequired(writeReq.getDbName(), writeCmdObj);
    }

    if (commandName == BulkWriteCommandRequest::kCommandName) {
        auto bulkWriteRequest = BulkWriteCommandRequest::parse(
            IDLParserContext("_clusterQueryWithoutShardKeyForBulkWrite"), writeCmdObj);
        tassert(7298303,
                "Only bulkWrite with a single op is allowed in _clusterQueryWithoutShardKey",
                bulkWriteRequest.getOps().size() == 1);
        auto op = BulkWriteCRUDOp(bulkWriteRequest.getOps()[0]);
        tassert(7298304,
                str::stream()
                    << op.getType()
                    << " is not a supported opType for bulkWrite in _clusterQueryWithoutShardKey",
                op.getType() == BulkWriteCRUDOp::kUpdate ||
                    op.getType() == BulkWriteCRUDOp::kDelete);
        auto& nsInfo = bulkWriteRequest.getNsInfo()[op.getNsInfoIdx()];
        parsedInfo.nss = nsInfo.getNs();
        parsedInfo.isTimeseriesNamespace = nsInfo.getIsTimeseriesNamespace();
        parsedInfo.let = bulkWriteRequest.getLet();
        if (op.getType() == BulkWriteCRUDOp::kUpdate) {
            // The update case.
            auto updateOp = op.getUpdate();
            parsedInfo.query = updateOp->getFilter();
            parsedInfo.hint = updateOp->getHint();
            if ((parsedInfo.upsert = updateOp->getUpsert())) {
                parsedInfo.updateRequest =
                    bulk_write_common::makeUpdateOpEntryFromUpdateOp(updateOp);
                parsedInfo.updateRequest->setNamespaceString(parsedInfo.nss);
            }
            if (auto parsedCollation = updateOp->getCollation()) {
                parsedInfo.collation = parsedCollation.value();
            }
        } else {
            // The delete case.
            auto deleteOp = op.getDelete();
            parsedInfo.query = deleteOp->getFilter();
            parsedInfo.hint = deleteOp->getHint();
            if (auto parsedCollation = deleteOp->getCollation()) {
                parsedInfo.collation = parsedCollation.value();
            }
        }
        if (auto stmtIds = bulkWriteRequest.getStmtIds()) {
            parsedInfo.stmtId = stmtIds->front();
        }
    } else if (commandName == write_ops::UpdateCommandRequest::kCommandName) {
        auto updateRequest = write_ops::UpdateCommandRequest::parse(
            IDLParserContext("_clusterQueryWithoutShardKeyForUpdate"), writeCmdObj);
        parsedInfo.query = updateRequest.getUpdates().front().getQ();
        parsedInfo.hint = updateRequest.getUpdates().front().getHint();
        parsedInfo.let = updateRequest.getLet();
        parsedInfo.isTimeseriesNamespace = updateRequest.getIsTimeseriesNamespace();

        // In the batch write path, when the request is reconstructed to be passed to
        // the two phase write protocol, only the stmtIds field is used.
        if (auto stmtIds = updateRequest.getStmtIds()) {
            parsedInfo.stmtId = stmtIds->front();
        }

        if ((parsedInfo.upsert = updateRequest.getUpdates().front().getUpsert())) {
            parsedInfo.updateRequest = updateRequest.getUpdates().front();
            parsedInfo.updateRequest->setNamespaceString(updateRequest.getNamespace());
        }

        if (auto parsedCollation = updateRequest.getUpdates().front().getCollation()) {
            parsedInfo.collation = *parsedCollation;
        }
    } else if (commandName == write_ops::DeleteCommandRequest::kCommandName) {
        auto deleteRequest = write_ops::DeleteCommandRequest::parse(
            IDLParserContext("_clusterQueryWithoutShardKeyForDelete"), writeCmdObj);
        parsedInfo.query = deleteRequest.getDeletes().front().getQ();
        parsedInfo.hint = deleteRequest.getDeletes().front().getHint();
        parsedInfo.let = deleteRequest.getLet();
        parsedInfo.isTimeseriesNamespace = deleteRequest.getIsTimeseriesNamespace();

        // In the batch write path, when the request is reconstructed to be passed to
        // the two phase write protocol, only the stmtIds field is used.
        if (auto stmtIds = deleteRequest.getStmtIds()) {
            parsedInfo.stmtId = stmtIds->front();
        }

        if (auto parsedCollation = deleteRequest.getDeletes().front().getCollation()) {
            parsedInfo.collation = *parsedCollation;
        }
    } else if (commandName == write_ops::FindAndModifyCommandRequest::kCommandName ||
               commandName == write_ops::FindAndModifyCommandRequest::kCommandAlias) {
        auto findAndModifyRequest = write_ops::FindAndModifyCommandRequest::parse(
            IDLParserContext("_clusterQueryWithoutShardKeyFindAndModify"), writeCmdObj);
        validateFindAndModifyCommand(findAndModifyRequest);

        parsedInfo.query = findAndModifyRequest.getQuery();
        parsedInfo.stmtId = findAndModifyRequest.getStmtId().value_or(kUninitializedStmtId);
        parsedInfo.hint = findAndModifyRequest.getHint();
        parsedInfo.sort =
            findAndModifyRequest.getSort() && !findAndModifyRequest.getSort()->isEmpty()
            ? findAndModifyRequest.getSort()
            : boost::none;
        parsedInfo.let = findAndModifyRequest.getLet();
        parsedInfo.isTimeseriesNamespace = findAndModifyRequest.getIsTimeseriesNamespace();

        if ((parsedInfo.upsert = findAndModifyRequest.getUpsert().get_value_or(false))) {
            parsedInfo.updateRequest = UpdateRequest{};
            parsedInfo.updateRequest->setNamespaceString(findAndModifyRequest.getNamespace());
            update::makeUpdateRequest(
                opCtx, findAndModifyRequest, boost::none, parsedInfo.updateRequest.get_ptr());
        }

        if (auto parsedCollation = findAndModifyRequest.getCollation()) {
            parsedInfo.collation = *parsedCollation;
        }
    } else {
        uasserted(ErrorCodes::InvalidOptions,
                  str::stream() << commandName << " is not a supported batch write command");
    }
    return parsedInfo;
}

class ClusterQueryWithoutShardKeyCmd : public TypedCommand<ClusterQueryWithoutShardKeyCmd> {
public:
    using Request = ClusterQueryWithoutShardKey;
    using Response = ClusterQueryWithoutShardKeyResponse;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Response typedRun(OperationContext* opCtx) {
            LOGV2_DEBUG(6962300,
                        2,
                        "Running read phase for a write without a shard key.",
                        "clientWriteRequest"_attr = redact(request().getWriteCmd()));

            const auto writeCmdObj = request().getWriteCmd();

            // Parse into OpMsgRequest to append the $db field, which is required for command
            // parsing.
            const auto opMsgRequest = OpMsgRequestBuilder::create(
                auth::ValidatedTenancyScope::get(opCtx), ns().dbName(), writeCmdObj);
            auto parsedInfoFromRequest = parseWriteRequest(opCtx, opMsgRequest);
            const auto& nss = parsedInfoFromRequest.nss;

            // Get all shard ids for shards that have chunks in the desired namespace.
            hangBeforeMetadataRefreshClusterQuery.pauseWhileSet(opCtx);
            const auto cri = uassertStatusOK(getCollectionRoutingInfoForTxnCmd(opCtx, nss));

            auto allShardsContainingChunksForNs =
                getShardsToTarget(opCtx, cri, nss, parsedInfoFromRequest);

            // If the request omits the collation use the collection default collation. If
            // the collection just has the simple collation, we can leave the collation as
            // an empty BSONObj.
            if (parsedInfoFromRequest.collation.isEmpty()) {
                if (cri.cm.getDefaultCollator()) {
                    parsedInfoFromRequest.collation =
                        cri.cm.getDefaultCollator()->getSpec().toBSON();
                }
            }

            const auto& collectionUUID = cri.cm.getUUID();
            const auto& timeseriesFields = cri.cm.isSharded() &&
                    cri.cm.getTimeseriesFields().has_value() &&
                    parsedInfoFromRequest.isTimeseriesNamespace
                ? cri.cm.getTimeseriesFields()
                : boost::none;

            auto cmdObj =
                createAggregateCmdObj(opCtx, parsedInfoFromRequest, nss, timeseriesFields);

            std::vector<AsyncRequestsSender::Request> requests;
            requests.reserve(allShardsContainingChunksForNs.size());
            for (const auto& shardId : allShardsContainingChunksForNs) {
                requests.emplace_back(shardId,
                                      appendShardVersion(cmdObj, cri.getShardVersion(shardId)));
            }

            MultiStatementTransactionRequestsSender ars(
                opCtx,
                Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
                request().getDbName(),
                requests,
                ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                Shard::RetryPolicy::kNoRetry);

            BSONObj targetDoc;
            Response res;
            bool wasStatementExecuted = false;
            std::vector<RemoteCursor> remoteCursors;

            // The MultiStatementTransactionSender expects all statements executed by it to be a
            // part of the transaction. If we break after finding a target document and then
            // destruct the MultiStatementTransactionSender, we register the remaining responses as
            // failed requests. This has implications when we go to commit the internal transaction,
            // since the transaction router will notice that a request "failed" during execution and
            // try to abort the transaction, which in turn will force the internal transaction to
            // retry (potentially indefinitely). Thus, we need to wait for all of the responses from
            // the MultiStatementTransactionSender.
            while (!ars.done()) {
                auto response = ars.next();
                uassertStatusOK(response.swResponse);

                if (wasStatementExecuted) {
                    continue;
                }

                auto cursor = uassertStatusOK(
                    CursorResponse::parseFromBSON(response.swResponse.getValue().data));

                // Return the first target doc/shard id pair that has already applied the write
                // for a retryable write.
                if (cursor.getWasStatementExecuted()) {
                    // Since the retryable write history check happens before a write is executed,
                    // we can just use an empty BSONObj for the target doc.
                    res.setTargetDoc(BSONObj::kEmptyObject);
                    res.setShardId(boost::optional<mongo::StringData>(response.shardId));
                    wasStatementExecuted = true;
                    continue;
                }

                remoteCursors.emplace_back(RemoteCursor(
                    response.shardId.toString(), *response.shardHostAndPort, std::move(cursor)));
            }

            // For retryable writes, if the statement had already been executed successfully on a
            // particular shard, return that response immediately.
            if (wasStatementExecuted) {
                return res;
            }

            // Return a target document. If a sort order is specified, return the first target
            // document corresponding to the sort order for a particular sort key.
            AsyncResultsMergerParams params(std::move(remoteCursors), nss);
            if (auto sortPattern = parseSortPattern(opCtx, nss, parsedInfoFromRequest);
                !sortPattern.isEmpty()) {
                params.setSort(sortPattern);
            } else {
                params.setSort(boost::none);
            }

            std::unique_ptr<RouterExecStage> root = std::make_unique<RouterStageMerge>(
                opCtx,
                Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
                std::move(params));

            if (parsedInfoFromRequest.sort) {
                root = std::make_unique<RouterStageRemoveMetadataFields>(
                    opCtx, std::move(root), Document::allMetadataFieldNames);
            }

            if (auto nextResponse = uassertStatusOK(root->next()); !nextResponse.isEOF()) {
                res.setTargetDoc(nextResponse.getResult());
                res.setShardId(boost::optional<mongo::StringData>(nextResponse.getShardId()));
            }

            // If there are no targetable documents and {upsert: true}, create the document to
            // upsert.
            if (!res.getTargetDoc() && parsedInfoFromRequest.upsert) {
                auto [upsertDoc, userUpsertDoc] = write_without_shard_key::generateUpsertDocument(
                    opCtx,
                    parsedInfoFromRequest.updateRequest.get(),
                    collectionUUID,
                    timeseriesFields
                        ? boost::make_optional(timeseriesFields->getTimeseriesOptions())
                        : boost::none,
                    cri.cm.getDefaultCollator());
                res.setTargetDoc(upsertDoc);
                res.setUpsertRequired(true);

                if (timeseriesFields) {
                    res.setUserUpsertDocForTimeseries(userUpsertDoc);
                }
            }

            return res;
        }

    private:
        void explain(OperationContext* opCtx,
                     ExplainOptions::Verbosity verbosity,
                     rpc::ReplyBuilderInterface* result) override {
            auto vts = auth::ValidatedTenancyScope::get(opCtx);
            const auto writeCmdObj = [&] {
                const auto explainCmdObj = request().getWriteCmd();
                const auto opMsgRequestExplainCmd =
                    OpMsgRequestBuilder::create(vts, ns().dbName(), explainCmdObj);
                auto explainRequest = ExplainCommandRequest::parse(
                    IDLParserContext("_clusterQueryWithoutShardKeyExplain"),
                    opMsgRequestExplainCmd.body);
                return explainRequest.getCommandParameter().getOwned();
            }();

            // Parse into OpMsgRequest to append the $db field, which is required for command
            // parsing.
            const auto opMsgRequestWriteCmd =
                OpMsgRequestBuilder::create(vts, ns().dbName(), writeCmdObj);
            auto parsedInfoFromRequest = parseWriteRequest(opCtx, opMsgRequestWriteCmd);

            // Get all shard ids for shards that have chunks in the desired namespace.
            const auto& nss = parsedInfoFromRequest.nss;
            const auto cri = uassertStatusOK(getCollectionRoutingInfoForTxnCmd(opCtx, nss));

            auto allShardsContainingChunksForNs =
                getShardsToTarget(opCtx, cri, nss, parsedInfoFromRequest);
            auto cmdObj = createAggregateCmdObj(opCtx, parsedInfoFromRequest, nss, boost::none);

            const auto aggExplainCmdObj = ClusterExplain::wrapAsExplain(cmdObj, verbosity);

            std::vector<AsyncRequestsSender::Request> requests;
            requests.reserve(allShardsContainingChunksForNs.size());
            for (const auto& shardId : allShardsContainingChunksForNs) {
                requests.emplace_back(
                    shardId, appendShardVersion(aggExplainCmdObj, cri.getShardVersion(shardId)));
            }

            Timer timer;
            MultiStatementTransactionRequestsSender ars(
                opCtx,
                Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
                request().getDbName(),
                requests,
                ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                Shard::RetryPolicy::kNoRetry);

            ShardId shardId;
            std::vector<AsyncRequestsSender::Response> responses;

            while (!ars.done()) {
                auto response = ars.next();
                uassertStatusOK(response.swResponse);
                responses.push_back(response);
                shardId = response.shardId;
            }

            const auto millisElapsed = timer.millis();

            auto bodyBuilder = result->getBodyBuilder();
            uassertStatusOK(ClusterExplain::buildExplainResult(
                opCtx,
                responses,
                parsedInfoFromRequest.sort ? ClusterExplain::kMergeSortFromShards
                                           : ClusterExplain::kMergeFromShards,
                millisElapsed,
                writeCmdObj,
                &bodyBuilder));
            bodyBuilder.append("targetShardId", shardId);
        }

        NamespaceString ns() const override {
            return NamespaceString(request().getDbName());
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::internal));
        }
    };

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kNever;
    }

    bool supportsRetryableWrite() const final {
        return false;
    }

    bool allowedInTransactions() const final {
        return true;
    }

    // In the current implementation of the Stable API, sub-operations run under a command in the
    // Stable API where a client specifies {apiStrict: true} are expected to also be Stable API
    // compliant, when they technically should not be. To satisfy this requirement,
    // this command is marked as part of the Stable API, but is not truly a part of
    // it, since it is an internal-only command.
    const std::set<std::string>& apiVersions() const {
        return kApiVersions1;
    }
};

MONGO_REGISTER_COMMAND(ClusterQueryWithoutShardKeyCmd)
    .requiresFeatureFlag(&feature_flags::gFeatureFlagUpdateOneWithoutShardKey)
    .forRouter();

}  // namespace
}  // namespace mongo
