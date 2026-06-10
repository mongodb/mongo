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
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/query_cmd/plan_cache_clear_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/collation/collation_spec.h"
#include "mongo/db/router_role/cluster_commands_helpers.h"
#include "mongo/db/router_role/router_role.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/timeseries/timeseries_commands_conversion_helper.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <string>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

namespace mongo {
namespace {

/**
 * Mongos implementation of the 'planCacheClear' command. Forwards the command to one node in each
 * targeted shard. For example, with the default read preference ("primary"), clears plan cache
 * entries on the primary node of each shard.
 */
class ClusterPlanCacheClearCmd final : public TypedCommand<ClusterPlanCacheClearCmd> {
public:
    using Request = PlanCacheClearCommandRequest;

    ClusterPlanCacheClearCmd() : TypedCommand(Request::kCommandName) {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kOptIn;
    }

    std::string help() const override {
        return "Drops one or all plan cache entries for a collection.";
    }

    class Invocation final : public MinimalInvocationBase {
    public:
        using MinimalInvocationBase::MinimalInvocationBase;

    private:
        void run(OperationContext* opCtx, rpc::ReplyBuilderInterface* reply) override {
            const auto& nss = ns();
            const auto& cmdObj = unparsedRequest().body;
            BSONObjBuilder result;
            // Empty query to satisfy scatter-gather API.
            const BSONObj query;
            constexpr auto cmdName = Request::kCommandName;

            uassert(ErrorCodes::InvalidOptions,
                    "isTimeseriesNamespace field should not be set on a planCacheClear command "
                    "against a mongos",
                    !request().getIsTimeseriesNamespace().has_value());

            sharding::router::CollectionRouter router(opCtx, nss);
            router.routeWithRoutingContext(
                cmdName, [&](OperationContext* opCtx, RoutingContext& unusedRoutingCtx) {
                    // The CollectionRouter is not capable of implicitly translate the namespace to
                    // a timeseries buckets collection, which is required in this command. Hence,
                    // we'll use the CollectionRouter to handle StaleConfig errors but will ignore
                    // its RoutingContext. Instead, we'll use a CollectionRoutingInfoTargeter object
                    // to properly get the RoutingContext when the collection is timeseries.
                    // TODO (SERVER-117193) Use the RoutingContext provided by the CollectionRouter
                    // once all timeseries collections become viewless.
                    unusedRoutingCtx.skipValidation();

                    // Build a targeter from the user namespace. For a legacy time-series collection
                    // this will target the underlying buckets namespace and provide a
                    // RoutingContext for it.
                    auto targeter = CollectionRoutingInfoTargeter(opCtx, nss);

                    return routing_context_utils::runAndValidate(
                        targeter.getRoutingCtx(), [&](RoutingContext& routingCtx) {
                            // Clear the result builder since this lambda function may be retried if
                            // the router cache is stale.
                            result.resetToEmpty();

                            auto cmdToSend = [&] {
                                if (targeter.timeseriesNamespaceNeedsRewrite(nss)) {
                                    return timeseries::makeTimeseriesCommand(
                                        cmdObj,
                                        nss,
                                        cmdName,
                                        timeseries::kIsTimeseriesNamespaceFieldName);
                                }
                                return cmdObj;
                            }();

                            auto shardResponses = scatterGatherVersionedTargetByRoutingTable(
                                opCtx,
                                routingCtx,
                                targeter.getNS(),
                                applyReadWriteConcern(
                                    opCtx,
                                    this,
                                    CommandHelpers::filterCommandRequestForPassthrough(cmdToSend)),
                                ReadPreferenceSetting::get(opCtx),
                                Shard::RetryPolicy::kIdempotent,
                                query,
                                CollationSpec::kSimpleSpec,
                                boost::none /*letParameters*/,
                                boost::none /*runtimeConstants*/);

                            // Sort shard responses by shard id.
                            std::sort(shardResponses.begin(),
                                      shardResponses.end(),
                                      [](const AsyncRequestsSender::Response& response1,
                                         const AsyncRequestsSender::Response& response2) {
                                          return response1.shardId < response2.shardId;
                                      });

                            // Set value of first shard result's "ok" field.
                            bool clusterCmdResult = true;

                            for (auto i = shardResponses.begin(); i != shardResponses.end(); ++i) {
                                const auto& response = *i;
                                auto status = response.swResponse.getStatus();
                                uassertStatusOK(status.withContext(
                                    str::stream() << "failed on: " << response.shardId));
                                const auto& cmdResult = response.swResponse.getValue().data;

                                // In absence of sensible aggregation strategy, promote first
                                // shard's result to top level.
                                if (i == shardResponses.begin()) {
                                    CommandHelpers::filterCommandReplyForPassthrough(cmdResult,
                                                                                     &result);
                                    status = getStatusFromCommandResult(cmdResult);
                                    clusterCmdResult = status.isOK();
                                }

                                // Append shard result as a sub object. Name the field after the
                                // shard.
                                result.append(ShardId(response.shardId), cmdResult);
                            }
                            return clusterCmdResult;
                        });
                });

            auto bodyBuilder = reply->getBodyBuilder();
            bodyBuilder.appendElements(result.obj());
        }

        NamespaceString ns() const override {
            return request().getNamespace();
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            AuthorizationSession* authzSession = AuthorizationSession::get(opCtx->getClient());
            ResourcePattern pattern = CommandHelpers::resourcePatternForNamespace(ns());

            if (!authzSession->isAuthorizedForActionsOnResource(pattern,
                                                                ActionType::planCacheWrite)) {
                uasserted(ErrorCodes::Unauthorized, "unauthorized");
            }
        }
    };
};
MONGO_REGISTER_COMMAND(ClusterPlanCacheClearCmd).forRouter();
}  // namespace
}  // namespace mongo
