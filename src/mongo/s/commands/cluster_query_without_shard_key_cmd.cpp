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
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/logv2/log.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/is_mongos.h"
#include "mongo/s/multi_statement_transaction_requests_sender.h"
#include "mongo/s/request_types/cluster_commands_without_shard_key_gen.h"
#include "mongo/s/write_ops/batch_write_op.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {

struct ParsedCommandInfo {
    BSONObj query;
    BSONObj collation;

    ParsedCommandInfo(BSONObj query, BSONObj collation) : query(query), collation(collation) {}
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
    cm.getShardIdsForQuery(expCtx, query, collation, &allShardsContainingChunksForNs);

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
    return aggregate.toBSON({});
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
                  "clientWriteRequest"_attr = request().getWriteCmd(),
                  "stmtIdInBatch"_attr = request().getStmtId());

            // Get all shard ids for shards that have chunks in the desired namespace.
            const NamespaceString nss(
                CommandHelpers::parseNsCollectionRequired(ns().dbName(), request().getWriteCmd()));
            const auto cm = uassertStatusOK(getCollectionRoutingInfoForTxnCmd(opCtx, nss));
            auto parsedInfoFromRequest = [&] {
                const auto commandName = request().getWriteCmd().firstElementFieldNameStringData();
                BSONObjBuilder bob(request().getWriteCmd());
                bob.append("$db", ns().dbName().toString());
                auto writeCmdObj = bob.obj();
                BSONObj query;
                BSONObj collation;
                if (commandName == "update") {
                    auto updateRequest = write_ops::UpdateCommandRequest::parse(
                        IDLParserContext("_clusterQueryWithoutShardKey"), writeCmdObj);
                    query = updateRequest.getUpdates().front().getQ();
                    if (auto parsedCollation = updateRequest.getUpdates().front().getCollation()) {
                        collation = *parsedCollation;
                    }
                } else if (commandName == "delete") {
                    auto deleteRequest = write_ops::DeleteCommandRequest::parse(
                        IDLParserContext("_clusterQueryWithoutShardKey"), writeCmdObj);
                    query = deleteRequest.getDeletes().front().getQ();
                    if (auto parsedCollation = deleteRequest.getDeletes().front().getCollation()) {
                        collation = *parsedCollation;
                    }
                } else if (commandName == "findAndModify" || commandName == "findandmodify") {
                    auto findAndModifyRequest = write_ops::FindAndModifyCommandRequest::parse(
                        IDLParserContext("_clusterQueryWithoutShardKey"), writeCmdObj);
                    query = findAndModifyRequest.getQuery();
                    if (auto parsedCollation = findAndModifyRequest.getCollation()) {
                        collation = *parsedCollation;
                    }
                } else {
                    uasserted(ErrorCodes::InvalidOptions, "Not a supported batch write command");
                }
                return ParsedCommandInfo(query.getOwned(), collation.getOwned());
            }();

            auto allShardsContainingChunksForNs =
                getShardsToTarget(opCtx, cm, nss, parsedInfoFromRequest);
            auto cmdObj = createAggregateCmdObj(opCtx, parsedInfoFromRequest, nss);

            std::vector<AsyncRequestsSender::Request> requests;
            for (const auto& shardId : allShardsContainingChunksForNs) {
                ChunkVersion placementVersion = cm.getVersion(shardId);
                requests.emplace_back(
                    shardId,
                    appendShardVersion(
                        cmdObj,
                        ShardVersion(placementVersion,
                                     boost::optional<CollectionIndexes>(boost::none))));
            }

            MultiStatementTransactionRequestsSender ars(
                opCtx,
                Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
                request().getDbName().toString(),
                requests,
                ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                Shard::RetryPolicy::kNoRetry);

            std::vector<AsyncRequestsSender::Response> results;
            while (!ars.done()) {
                auto response = ars.next();
                uassertStatusOK(response.swResponse);
                results.push_back(response);
            }

            BSONObj targetDoc;
            Response res;
            for (const auto& arsRes : results) {
                auto cursorResponse = uassertStatusOK(
                    CursorResponse::parseFromBSON(arsRes.swResponse.getValue().data));

                // Return the first response that contains a matching document.
                if (cursorResponse.getBatch().size() > 0) {
                    res.setTargetDoc(cursorResponse.releaseBatch().front().getOwned());
                    res.setShardId(boost::optional<mongo::StringData>(arsRes.shardId));
                    break;
                }
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
        return true;
    }

    bool allowedInTransactions() const final {
        return true;
    }
};

MONGO_REGISTER_FEATURE_FLAGGED_COMMAND(ClusterQueryWithoutShardKeyCmd,
                                       feature_flags::gFeatureFlagUpdateOneWithoutShardKey);

}  // namespace
}  // namespace mongo
