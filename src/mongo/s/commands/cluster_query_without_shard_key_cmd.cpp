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
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/internal_transactions_feature_flag_gen.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/db/update/update_util.h"
#include "mongo/logv2/log.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/is_mongos.h"
#include "mongo/s/multi_statement_transaction_requests_sender.h"
#include "mongo/s/query/router_stage_merge.h"
#include "mongo/s/query/router_stage_remove_metadata_fields.h"
#include "mongo/s/request_types/cluster_commands_without_shard_key_gen.h"
#include "mongo/s/shard_key_pattern_query_util.h"
#include "mongo/s/write_ops/batch_write_op.h"
#include "mongo/s/write_ops/write_without_shard_key_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {

constexpr auto kIdFieldName = "_id"_sd;

struct ParsedCommandInfo {
    BSONObj query;
    BSONObj collation;
    boost::optional<BSONObj> sort;
    bool upsert = false;
    int stmtId = kUninitializedStmtId;
    boost::optional<UpdateRequest> updateRequest;
};

struct AsyncRequestSenderResponseData {
    ShardId shardId;
    CursorResponse cursorResponse;

    AsyncRequestSenderResponseData(ShardId shardId, CursorResponse cursorResponse)
        : shardId(shardId), cursorResponse(std::move(cursorResponse)) {}
};

std::set<ShardId> getShardsToTarget(OperationContext* opCtx,
                                    const ChunkManager& cm,
                                    NamespaceString nss,
                                    const ParsedCommandInfo& parsedInfo) {
    std::set<ShardId> allShardsContainingChunksForNs;
    uassert(ErrorCodes::InvalidOptions,
            "_clusterQueryWithoutShardKey can only be run against sharded collections",
            cm.isSharded());

    auto query = parsedInfo.query;
    auto collation = parsedInfo.collation;
    std::unique_ptr<CollatorInterface> collator;
    if (!collation.isEmpty()) {
        collator = uassertStatusOK(
            CollatorFactoryInterface::get(opCtx->getServiceContext())->makeFromBSON(collation));
    }
    auto expCtx = make_intrusive<ExpressionContext>(opCtx, std::move(collator), nss);
    getShardIdsForQuery(
        expCtx, query, collation, cm, &allShardsContainingChunksForNs, nullptr /* info */);

    // We must either get a subset of shards to target in the case of a partial shard key or we must
    // target all shards.
    invariant(allShardsContainingChunksForNs.size() > 0);

    return allShardsContainingChunksForNs;
}

// TODO: SERVER-75760 Remove this validation since mongos should be doing this upfront.
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
            // TODO (SERVER-73083): skip the sort option for 'findAndModify' calls on time-series
            // collections.
            pipeline.emplace_back(BSON(DocumentSourceSort::kStageName << *parsedInfo.sort));
        }
        pipeline.emplace_back(BSON(DocumentSourceLimit::kStageName << 1));
        pipeline.emplace_back(BSON(DocumentSourceProject::kStageName << BSON(kIdFieldName << 1)));
        return pipeline;
    }());

    return aggregate.toBSON({});
}

ParsedCommandInfo parseWriteCommand(OperationContext* opCtx,
                                    StringData commandName,
                                    const BSONObj& writeCmdObj) {
    ParsedCommandInfo parsedInfo;
    if (commandName == write_ops::UpdateCommandRequest::kCommandName) {
        auto updateRequest = write_ops::UpdateCommandRequest::parse(
            IDLParserContext("_clusterQueryWithoutShardKeyForUpdate"), writeCmdObj);
        parsedInfo.query = updateRequest.getUpdates().front().getQ();

        // In the batch write path, when the request is reconstructed to be passed to
        // the two phase write protocol, only the stmtIds field is used.
        if (auto stmtIds = updateRequest.getStmtIds()) {
            parsedInfo.stmtId = stmtIds->front();
        }

        if ((parsedInfo.upsert = updateRequest.getUpdates().front().getUpsert())) {
            parsedInfo.updateRequest = updateRequest.getUpdates().front();
        }

        if (auto parsedCollation = updateRequest.getUpdates().front().getCollation()) {
            parsedInfo.collation = *parsedCollation;
        }
    } else if (commandName == write_ops::DeleteCommandRequest::kCommandName) {
        auto deleteRequest = write_ops::DeleteCommandRequest::parse(
            IDLParserContext("_clusterQueryWithoutShardKeyForDelete"), writeCmdObj);
        parsedInfo.query = deleteRequest.getDeletes().front().getQ();

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
        parsedInfo.sort =
            findAndModifyRequest.getSort() && !findAndModifyRequest.getSort()->isEmpty()
            ? findAndModifyRequest.getSort()
            : boost::none;

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
        uasserted(ErrorCodes::InvalidOptions, "Not a supported write command");
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
            uassert(ErrorCodes::IllegalOperation,
                    "_clusterQueryWithoutShardKey can only be run on Mongos",
                    isMongos());

            LOGV2(6962300,
                  "Running read phase for a write without a shard key.",
                  "clientWriteRequest"_attr = request().getWriteCmd());

            // Get all shard ids for shards that have chunks in the desired namespace.
            const NamespaceString nss(
                CommandHelpers::parseNsCollectionRequired(ns().dbName(), request().getWriteCmd()));
            const auto cri = uassertStatusOK(getCollectionRoutingInfoForTxnCmd(opCtx, nss));

            // Parse into OpMsgRequest to append the $db field, which is required for command
            // parsing.
            const auto opMsgRequest =
                OpMsgRequest::fromDBAndBody(nss.db(), request().getWriteCmd());

            auto parsedInfoFromRequest =
                parseWriteCommand(opCtx,
                                  request().getWriteCmd().firstElementFieldNameStringData(),
                                  opMsgRequest.body);

            auto allShardsContainingChunksForNs =
                getShardsToTarget(opCtx, cri.cm, nss, parsedInfoFromRequest);

            const auto& timeseriesFields =
                (cri.cm.isSharded() && cri.cm.getTimeseriesFields().has_value())
                ? cri.cm.getTimeseriesFields()
                : boost::none;
            auto cmdObj =
                createAggregateCmdObj(opCtx, parsedInfoFromRequest, nss, timeseriesFields);

            std::vector<AsyncRequestsSender::Request> requests;
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
            params.setSort(parsedInfoFromRequest.sort);

            std::unique_ptr<RouterExecStage> root = std::make_unique<RouterStageMerge>(
                opCtx,
                Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
                std::move(params));

            if (parsedInfoFromRequest.sort) {
                root = std::make_unique<RouterStageRemoveMetadataFields>(
                    opCtx, std::move(root), StringDataSet{AsyncResultsMerger::kSortKeyField});
            }

            if (auto nextResponse = uassertStatusOK(root->next()); !nextResponse.isEOF()) {
                res.setTargetDoc(nextResponse.getResult());
                res.setShardId(boost::optional<mongo::StringData>(nextResponse.getShardId()));
            }

            // If there are no targetable documents and {upsert: true}, create the document to
            // upsert.
            if (!res.getTargetDoc() && parsedInfoFromRequest.upsert) {
                res.setTargetDoc(write_without_shard_key::generateUpsertDocument(
                    opCtx, parsedInfoFromRequest.updateRequest.get()));
                res.setUpsertRequired(true);
            }

            return res;
        }

    private:
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
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
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

MONGO_REGISTER_FEATURE_FLAGGED_COMMAND(ClusterQueryWithoutShardKeyCmd,
                                       feature_flags::gFeatureFlagUpdateOneWithoutShardKey);

}  // namespace
}  // namespace mongo
