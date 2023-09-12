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


#include <algorithm>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/curop.h"
#include "mongo/db/database_name.h"
#include "mongo/db/drop_indexes_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/legacy_runtime_constants_gen.h"
#include "mongo/db/s/ddl_lock_manager.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_id.h"
#include "mongo/db/timeseries/catalog_helper.h"
#include "mongo/db/timeseries/timeseries_commands_conversion_helper.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"
#include "mongo/s/stale_shard_version_helpers.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

struct StaleConfigRetryState {
    std::set<ShardId> shardsWithSuccessResponses;
    std::vector<AsyncRequestsSender::Response> shardSuccessResponses;
};

void updateStateForStaleConfigRetry(OperationContext* opCtx,
                                    const RawResponsesResult& response,
                                    StaleConfigRetryState* retryState) {
    std::set<ShardId> okShardIds;
    std::set_union(response.shardsWithSuccessResponses.begin(),
                   response.shardsWithSuccessResponses.end(),
                   retryState->shardsWithSuccessResponses.begin(),
                   retryState->shardsWithSuccessResponses.end(),
                   std::inserter(okShardIds, okShardIds.begin()));

    retryState->shardsWithSuccessResponses = std::move(okShardIds);
    retryState->shardSuccessResponses = std::move(response.successResponses);
}

class ShardsvrDropIndexesCommand final : public TypedCommand<ShardsvrDropIndexesCommand> {
public:
    using Request = ShardsvrDropIndexes;

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command. Do not call directly. Drops indexes.";
    }

    bool adminOnly() const override {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        /**
         * Intermediate wrapper to interface with ReplyBuilderInterface.
         */
        class Response {
        public:
            Response(BSONObj obj) : _obj(std::move(obj)) {}

            void serialize(BSONObjBuilder* builder) const {
                builder->appendElements(_obj);
            }

        private:
            const BSONObj _obj;
        };

        Response typedRun(OperationContext* opCtx);

    private:
        NamespaceString ns() const override {
            return request().getNamespace();
        }

        bool supportsWriteConcern() const override {
            return true;
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
};
MONGO_REGISTER_COMMAND(ShardsvrDropIndexesCommand).forShard();

ShardsvrDropIndexesCommand::Invocation::Response ShardsvrDropIndexesCommand::Invocation::typedRun(
    OperationContext* opCtx) {
    uassertStatusOK(ShardingState::get(opCtx)->canAcceptShardedCommands());
    CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName, opCtx->getWriteConcern());

    // Since this operation is not directly writing locally we need to force its db profile level
    // increase in order to be logged in "<db>.system.profile".
    CurOp::get(opCtx)->raiseDbProfileLevel(
        CollectionCatalog::get(opCtx)->getDatabaseProfileLevel(ns().dbName()));

    DropIndexes dropIdxCmd(ns());
    dropIdxCmd.setDropIndexesRequest(request().getDropIndexesRequest());

    // Acquire the DDL lock to serialize with other DDL operations. It also makes sure that we are
    // targeting the primary shard for this database.
    static constexpr StringData lockReason{"dropIndexes"_sd};
    const DDLLockManager::ScopedCollectionDDLLock collDDLLock{opCtx, ns(), lockReason, MODE_X};

    auto resolvedNs = ns();
    auto dropIdxBSON = dropIdxCmd.toBSON({});

    // Checking if it is a timeseries collection under the collection DDL lock
    boost::optional<DDLLockManager::ScopedCollectionDDLLock> timeseriesCollDDLLock;
    if (auto timeseriesOptions = timeseries::getTimeseriesOptions(opCtx, ns(), true)) {
        dropIdxBSON =
            timeseries::makeTimeseriesCommand(dropIdxBSON,
                                              ns(),
                                              DropIndexes::kCommandName,
                                              DropIndexes::kIsTimeseriesNamespaceFieldName);

        resolvedNs = ns().makeTimeseriesBucketsNamespace();

        // If it is a timeseries collection, we actually need to acquire the bucket namespace DDL
        // lock
        timeseriesCollDDLLock.emplace(opCtx, resolvedNs, lockReason, MODE_X);
    }

    StaleConfigRetryState retryState;
    return shardVersionRetry(
        opCtx, Grid::get(opCtx)->catalogCache(), resolvedNs, "dropIndexes", [&] {
            // If the collection is sharded, we target only the primary shard and the shards that
            // own chunks for the collection.
            const auto cri = uassertStatusOK(
                Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, resolvedNs));

            auto cmdToBeSent = CommandHelpers::filterCommandRequestForPassthrough(
                CommandHelpers::appendMajorityWriteConcern(dropIdxBSON));

            auto shardResponses =
                scatterGatherVersionedTargetByRoutingTableNoThrowOnStaleShardVersionErrors(
                    opCtx,
                    resolvedNs.dbName(),
                    resolvedNs,
                    cri,
                    retryState.shardsWithSuccessResponses,
                    applyReadWriteConcern(
                        opCtx,
                        this,
                        CommandHelpers::filterCommandRequestForPassthrough(cmdToBeSent)),
                    ReadPreferenceSetting::get(opCtx),
                    Shard::RetryPolicy::kNotIdempotent,
                    BSONObj() /*query*/,
                    BSONObj() /*collation*/,
                    boost::none /*letParameters*/,
                    boost::none /*runtimeConstants*/);

            // Append responses we've received from previous retries of this operation due to a
            // stale config error.
            shardResponses.insert(shardResponses.end(),
                                  retryState.shardSuccessResponses.begin(),
                                  retryState.shardSuccessResponses.end());

            std::string errmsg;
            BSONObjBuilder output, rawResBuilder;
            bool isShardedCollection = cri.cm.isSharded();
            const auto aggregateResponse = appendRawResponses(
                opCtx, &errmsg, &rawResBuilder, shardResponses, isShardedCollection);

            // If we have a stale config error, update the success shards for the upcoming retry.
            if (!aggregateResponse.responseOK && aggregateResponse.firstStaleConfigError) {
                updateStateForStaleConfigRetry(opCtx, aggregateResponse, &retryState);
                uassertStatusOK(*aggregateResponse.firstStaleConfigError);
            }

            if (!isShardedCollection && aggregateResponse.responseOK) {
                CommandHelpers::filterCommandReplyForPassthrough(
                    shardResponses[0].swResponse.getValue().data, &output);
            }

            output.appendElements(rawResBuilder.obj());
            CommandHelpers::appendSimpleCommandStatus(output, aggregateResponse.responseOK, errmsg);
            return Response(output.obj());
        });
}

}  // namespace
}  // namespace mongo
