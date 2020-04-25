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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/commands.h"
#include "mongo/logv2/log.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"

namespace mongo {
namespace {

struct StaleConfigRetryState {
    std::set<ShardId> shardsWithSuccessResponses;
    std::vector<AsyncRequestsSender::Response> shardSuccessResponses;
};

const OperationContext::Decoration<std::unique_ptr<StaleConfigRetryState>> staleConfigRetryState =
    OperationContext::declareDecoration<std::unique_ptr<StaleConfigRetryState>>();

StaleConfigRetryState createAndRetrieveStateFromStaleConfigRetry(OperationContext* opCtx) {
    if (!staleConfigRetryState(opCtx)) {
        staleConfigRetryState(opCtx) = std::make_unique<StaleConfigRetryState>();
    }

    return *staleConfigRetryState(opCtx);
}

void updateStateForStaleConfigRetry(OperationContext* opCtx,
                                    const StaleConfigRetryState& retryState,
                                    const RawResponsesResult& response) {
    std::set<ShardId> okShardIds;
    std::set_union(response.shardsWithSuccessResponses.begin(),
                   response.shardsWithSuccessResponses.end(),
                   retryState.shardsWithSuccessResponses.begin(),
                   retryState.shardsWithSuccessResponses.end(),
                   std::inserter(okShardIds, okShardIds.begin()));

    staleConfigRetryState(opCtx)->shardsWithSuccessResponses = std::move(okShardIds);
    staleConfigRetryState(opCtx)->shardSuccessResponses = std::move(response.successResponses);
}

class DropIndexesCmd : public ErrmsgCommandDeprecated {
public:
    DropIndexesCmd() : ErrmsgCommandDeprecated("dropIndexes", "deleteIndexes") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return false;
    }

    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) const override {
        ActionSet actions;
        actions.addAction(ActionType::dropIndex);
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    bool errmsgRun(OperationContext* opCtx,
                   const std::string& dbName,
                   const BSONObj& cmdObj,
                   std::string& errmsg,
                   BSONObjBuilder& output) override {
        const NamespaceString nss(CommandHelpers::parseNsCollectionRequired(dbName, cmdObj));
        LOGV2_DEBUG(22751,
                    1,
                    "dropIndexes: {namespace} cmd: {command}",
                    "CMD: dropIndexes",
                    "namespace"_attr = nss,
                    "command"_attr = redact(cmdObj));

        // dropIndexes can be retried on a stale config error. If a previous attempt already
        // successfully dropped the index on shards, those shards will return an IndexNotFound
        // error when retried. We instead maintain the record of shards that have already
        // successfully dropped the index, so that we don't try to contact those shards again
        // across stale config retries.
        const auto retryState = createAndRetrieveStateFromStaleConfigRetry(opCtx);

        // If the collection is sharded, we target only the primary shard and the shards that own
        // chunks for the collection.
        auto routingInfo =
            uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));
        auto shardResponses =
            scatterGatherVersionedTargetByRoutingTableNoThrowOnStaleShardVersionErrors(
                opCtx,
                nss.db(),
                nss,
                routingInfo,
                retryState.shardsWithSuccessResponses,
                applyReadWriteConcern(
                    opCtx, this, CommandHelpers::filterCommandRequestForPassthrough(cmdObj)),
                ReadPreferenceSetting::get(opCtx),
                Shard::RetryPolicy::kNotIdempotent,
                BSONObj() /* query */,
                BSONObj() /* collation */);

        // Append responses we've received from previous retries of this operation due to a stale
        // config error.
        shardResponses.insert(shardResponses.end(),
                              retryState.shardSuccessResponses.begin(),
                              retryState.shardSuccessResponses.end());

        const auto aggregateResponse =
            appendRawResponses(opCtx, &errmsg, &output, std::move(shardResponses));

        // If we have a stale config error, update the success shards for the upcoming retry.
        if (!aggregateResponse.responseOK && aggregateResponse.firstStaleConfigError) {
            updateStateForStaleConfigRetry(opCtx, retryState, aggregateResponse);
            uassertStatusOK(*aggregateResponse.firstStaleConfigError);
        }

        return aggregateResponse.responseOK;
    }

} dropIndexesCmd;

}  // namespace
}  // namespace mongo
