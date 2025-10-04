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


#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/curop.h"
#include "mongo/db/database_name.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache.h"
#include "mongo/db/global_catalog/ddl/ddl_lock_manager.h"
#include "mongo/db/global_catalog/ddl/drop_indexes_coordinator.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/router_role_api/cluster_commands_helpers.h"
#include "mongo/db/local_catalog/ddl/drop_indexes_gen.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/legacy_runtime_constants_gen.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/timeseries/catalog_helper.h"
#include "mongo/db/timeseries/timeseries_commands_conversion_helper.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"

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
    retryState->shardSuccessResponses = response.successResponses;
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
    ShardingState::get(opCtx)->assertCanAcceptShardedCommands();

    opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

    CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName, opCtx->getWriteConcern());

    // Since this operation is not directly writing locally we need to force its db profile level
    // increase in order to be logged in "<db>.system.profile".
    CurOp::get(opCtx)->raiseDbProfileLevel(DatabaseProfileSettings::get(opCtx->getServiceContext())
                                               .getDatabaseProfileLevel(ns().dbName()));

    while (true) {
        boost::optional<FixedFCVRegion> optFixedFcvRegion{boost::in_place_init, opCtx};

        bool useCoordinator = feature_flags::gFeatureFlagDropIndexesDDLCoordinator.isEnabled(
            VersionContext::getDecoration(opCtx), optFixedFcvRegion.get()->acquireFCVSnapshot());

        if (useCoordinator) {
            auto coordinatorDoc = [&] {
                auto doc = DropIndexesCoordinatorDocument();
                doc.setShardingDDLCoordinatorMetadata(
                    {{ns(), DDLCoordinatorTypeEnum::kDropIndexes}});
                doc.setDropIndexesRequest(request().getDropIndexesRequest());
                return doc;
            }();

            const auto coordinator = [&] {
                auto service = ShardingDDLCoordinatorService::getService(opCtx);
                return checked_pointer_cast<DropIndexesCoordinator>(service->getOrCreateInstance(
                    opCtx, coordinatorDoc.toBSON(), *optFixedFcvRegion, false /*checkOptions*/));
            }();

            try {
                coordinator->checkIfOptionsConflict(coordinatorDoc.toBSON());
            } catch (const ExceptionFor<ErrorCodes::ConflictingOperationInProgress>& ex) {
                LOGV2_DEBUG(10695003,
                            1,
                            "Drop indexes coordinator already running, waiting for completion",
                            "namespace"_attr = ns(),
                            "error"_attr = ex);

                optFixedFcvRegion.reset();
                coordinator->getCompletionFuture().getNoThrow(opCtx).ignore();
                continue;
            }

            optFixedFcvRegion.reset();
            auto completionStatus = coordinator->getCompletionFuture().getNoThrow(opCtx);
            auto result = coordinator->getResult(opCtx);

            if (!result) {
                uassertStatusOK(completionStatus);

                // Result must be populated if the coordinator succeeded.
                tasserted(10710101, "DropIndexes result unavailable");
            }
            return Response(result->getOwned());
        }

        optFixedFcvRegion.reset();
        break;
    }

    DropIndexes dropIdxCmd(ns());
    dropIdxCmd.setDropIndexesRequest(request().getDropIndexesRequest());

    generic_argument_util::setMajorityWriteConcern(dropIdxCmd);

    // Acquire the DDL lock to serialize with other DDL operations. It also makes sure that we are
    // targeting the primary shard for this database.
    static constexpr StringData lockReason{"dropIndexes"_sd};
    const DDLLockManager::ScopedCollectionDDLLock collDDLLock{opCtx, ns(), lockReason, MODE_X};

    setReadWriteConcern(opCtx, dropIdxCmd, this);

    auto resolvedNs = ns();
    auto dropIdxBSON = dropIdxCmd.toBSON();

    // Checking if it is a timeseries collection under the collection DDL lock
    if (auto timeseriesOptions = timeseries::getTimeseriesOptions(opCtx, ns(), true)) {
        dropIdxBSON =
            timeseries::makeTimeseriesCommand(dropIdxBSON,
                                              ns(),
                                              DropIndexes::kCommandName,
                                              DropIndexes::kIsTimeseriesNamespaceFieldName);

        resolvedNs = ns().makeTimeseriesBucketsNamespace();
    }

    StaleConfigRetryState retryState;
    sharding::router::CollectionRouter router(opCtx->getServiceContext(), resolvedNs);
    return router.routeWithRoutingContext(
        opCtx, "dropIndexes", [&](OperationContext* opCtx, RoutingContext& routingCtx) {
            auto shardResponses =
                scatterGatherVersionedTargetByRoutingTableNoThrowOnStaleShardVersionErrors(
                    opCtx,
                    routingCtx,
                    resolvedNs,
                    retryState.shardsWithSuccessResponses,
                    CommandHelpers::filterCommandRequestForPassthrough(dropIdxBSON),
                    ReadPreferenceSetting::get(opCtx),
                    Shard::RetryPolicy::kNotIdempotent,
                    BSONObj() /*query*/,
                    BSONObj() /*collation*/,
                    boost::none /*letParameters*/,
                    boost::none /*runtimeConstants*/);

            // Append responses we've received from previous retries of this operation due
            // to a stale config error.
            shardResponses.insert(shardResponses.end(),
                                  retryState.shardSuccessResponses.begin(),
                                  retryState.shardSuccessResponses.end());

            std::string errmsg;
            BSONObjBuilder output, rawResBuilder;
            bool isShardedCollection = routingCtx.getCollectionRoutingInfo(resolvedNs).isSharded();
            const auto aggregateResponse = appendRawResponses(
                opCtx, &errmsg, &rawResBuilder, shardResponses, isShardedCollection);

            // If we have a stale config error, update the success shards for the upcoming
            // retry.
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

            // TODO SERVER-104795 we can fail to validate routing tables for a nss in the
            // event of a concurrent chink migration that moves requested chunks from one
            // shard to a shard that had previously been marked as returning a successful
            // response for dropIndexes(). We skip RoutingContext validation here to
            // avoiding hitting the assertion in validateOnDestroy(), but this should be
            // removed after the bug is patched.
            routingCtx.skipValidation();

            return Response(output.obj());
        });
}

}  // namespace
}  // namespace mongo
