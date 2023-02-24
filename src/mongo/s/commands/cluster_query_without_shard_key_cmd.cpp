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
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/update/update_util.h"
#include "mongo/logv2/log.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/is_mongos.h"
#include "mongo/s/multi_statement_transaction_requests_sender.h"
#include "mongo/s/request_types/cluster_commands_without_shard_key_gen.h"
#include "mongo/s/shard_key_pattern_query_util.h"
#include "mongo/s/write_ops/batch_write_op.h"
#include "mongo/s/write_ops/write_without_shard_key_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {

struct ParsedCommandInfo {
    BSONObj query;
    BSONObj collation;
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
    getShardIdsForQuery(expCtx,
                        query,
                        collation,
                        cm,
                        &allShardsContainingChunksForNs,
                        nullptr /* chunkRanges */,
                        nullptr /* targetMinKeyToMaxKey */);

    // We must either get a subset of shards to target in the case of a partial shard key or we must
    // target all shards.
    invariant(allShardsContainingChunksForNs.size() > 0);

    return allShardsContainingChunksForNs;
}

BSONObj createAggregateCmdObj(OperationContext* opCtx,
                              const ParsedCommandInfo& parsedInfo,
                              NamespaceString nss) {
    AggregateCommandRequest aggregate(nss,
                                      {BSON("$match" << parsedInfo.query),
                                       BSON("$limit" << 1),
                                       BSON("$project" << BSON("_id" << 1))});
    aggregate.setCollation(parsedInfo.collation);
    aggregate.setIsClusterQueryWithoutShardKeyCmd(true);
    aggregate.setFromMongos(true);

    if (parsedInfo.stmtId != kUninitializedStmtId) {
        aggregate.setStmtId(parsedInfo.stmtId);
    }
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
        parsedInfo.query = findAndModifyRequest.getQuery();
        parsedInfo.stmtId = findAndModifyRequest.getStmtId().value_or(kUninitializedStmtId);

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
        uasserted(ErrorCodes::InvalidOptions, "Not a supported batch write command");
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
            auto cmdObj = createAggregateCmdObj(opCtx, parsedInfoFromRequest, nss);

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
            std::vector<AsyncRequestSenderResponseData> responses;
            while (!ars.done()) {
                auto response = ars.next();
                uassertStatusOK(response.swResponse);
                responses.emplace_back(
                    AsyncRequestSenderResponseData(response.shardId,
                                                   uassertStatusOK(CursorResponse::parseFromBSON(
                                                       response.swResponse.getValue().data))));
            }

            for (auto& responseData : responses) {
                auto shardId = responseData.shardId;
                auto cursorResponse = std::move(responseData.cursorResponse);

                // Return the first target doc/shard id pair that has already applied the write
                // for a retryable write.
                if (cursorResponse.getWasStatementExecuted()) {
                    // Since the retryable write history check happens before a write is executed,
                    // we can just use an empty BSONObj for the target doc.
                    res.setTargetDoc(BSONObj::kEmptyObject);
                    res.setShardId(boost::optional<mongo::StringData>(shardId));
                    break;
                }

                if (cursorResponse.getBatch().size() > 0) {
                    res.setTargetDoc(cursorResponse.releaseBatch().front().getOwned());
                    res.setShardId(boost::optional<mongo::StringData>(shardId));
                }
            }

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
};

MONGO_REGISTER_FEATURE_FLAGGED_COMMAND(ClusterQueryWithoutShardKeyCmd,
                                       feature_flags::gFeatureFlagUpdateOneWithoutShardKey);

}  // namespace
}  // namespace mongo
