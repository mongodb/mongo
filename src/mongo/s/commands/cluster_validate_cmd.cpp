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


#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/router_role_api/cluster_commands_helpers.h"
#include "mongo/db/global_catalog/router_role_api/router_role.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/legacy_runtime_constants_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/timeseries/timeseries_commands_conversion_helper.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"

#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

class ValidateCmd : public BasicCommand {
public:
    ValidateCmd() : BasicCommand("validate") {}

    NamespaceString parseNs(const DatabaseName& dbName, const BSONObj& cmdObj) const override {
        return CommandHelpers::parseNsCollectionRequired(dbName, cmdObj);
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return false;
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj& cmdObj) const override {
        auto* as = AuthorizationSession::get(opCtx->getClient());
        if (!as->isAuthorizedForActionsOnResource(parseResourcePattern(dbName, cmdObj),
                                                  ActionType::validate)) {
            return {ErrorCodes::Unauthorized, "unauthorized"};
        }

        return Status::OK();
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    bool run(OperationContext* opCtx,
             const DatabaseName& dbName,
             const BSONObj& providedCmdObj,
             BSONObjBuilder& output) override {
        const NamespaceString nss(parseNs(dbName, providedCmdObj));

        sharding::router::CollectionRouter router{opCtx->getServiceContext(), nss};
        return router.routeWithRoutingContext(
            opCtx, getName(), [&](OperationContext* opCtx, RoutingContext& unusedRoutingCtx) {
                // The CollectionRouter is not capable of implicitly translate the namespace
                // to a timeseries buckets collection, which is required in this command.
                // Hence, we'll use the CollectionRouter to handle StaleConfig errors but
                // will ignore its RoutingContext. Instead, we'll use a
                // CollectionRoutingInfoTargeter object to properly get the RoutingContext
                // when the collection is timeseries.
                // TODO (SPM-3830) Use the RoutingContext provided by the CollectionRouter
                // once all timeseries collections become viewless.
                unusedRoutingCtx.skipValidation();

                // Clear the `result` BSON builder since this lambda function may be retried
                // if the router cache is stale.
                output.resetToEmpty();

                auto targeter = CollectionRoutingInfoTargeter(opCtx, nss);
                auto routingInfo = targeter.getRoutingInfo();
                auto cmdObj = [&]() {
                    if (targeter.timeseriesNamespaceNeedsRewrite(nss)) {
                        return timeseries::makeTimeseriesCommand(
                            providedCmdObj,
                            nss,
                            getName(),
                            CreateIndexesCommand::kIsTimeseriesNamespaceFieldName);
                    }
                    return providedCmdObj;
                }();

                return routing_context_utils::runAndValidate(
                    targeter.getRoutingCtx(), [&](RoutingContext& routingCtx) {
                        auto results = scatterGatherVersionedTargetByRoutingTable(
                            opCtx,
                            routingCtx,
                            targeter.getNS(),
                            applyReadWriteConcern(
                                opCtx,
                                this,
                                CommandHelpers::filterCommandRequestForPassthrough(cmdObj)),
                            ReadPreferenceSetting::get(opCtx),
                            Shard::RetryPolicy::kIdempotent,
                            {} /*query*/,
                            {} /*collation*/,
                            boost::none /*letParameters*/,
                            boost::none /*runtimeConstants*/);

                        Status firstFailedShardStatus = Status::OK();
                        bool isValid = true;
                        BSONObjBuilder rawResBuilder;

                        for (const auto& cmdResult : results) {
                            const auto& shardId = cmdResult.shardId;

                            const auto& swResponse = cmdResult.swResponse;
                            if (!swResponse.isOK()) {
                                rawResBuilder.append(
                                    shardId.toString(),
                                    BSON("error" << swResponse.getStatus().toString()));
                                if (firstFailedShardStatus.isOK())
                                    firstFailedShardStatus = swResponse.getStatus();
                                continue;
                            }

                            const auto& response = swResponse.getValue();
                            if (!response.isOK()) {
                                rawResBuilder.append(shardId.toString(),
                                                     BSON("error" << response.status.toString()));
                                if (firstFailedShardStatus.isOK())
                                    firstFailedShardStatus = response.status;
                                continue;
                            }

                            rawResBuilder.append(shardId.toString(), response.data);

                            const auto status = getStatusFromCommandResult(response.data);
                            if (!status.isOK()) {
                                if (firstFailedShardStatus.isOK())
                                    firstFailedShardStatus = status;
                                continue;
                            }

                            if (!response.data["valid"].trueValue()) {
                                isValid = false;
                            }
                        }
                        rawResBuilder.done();

                        if (firstFailedShardStatus.isOK()) {
                            if (!routingCtx.getCollectionRoutingInfo(targeter.getNS())
                                     .isSharded()) {
                                CommandHelpers::filterCommandReplyForPassthrough(
                                    results[0].swResponse.getValue().data, &output);
                            } else {
                                output.appendBool("valid", isValid);
                            }
                        }
                        output.append("raw", rawResBuilder.obj());

                        uassertStatusOK(firstFailedShardStatus);
                        return true;
                    });
            });
    }
};
MONGO_REGISTER_COMMAND(ValidateCmd).forRouter();

}  // namespace
}  // namespace mongo
