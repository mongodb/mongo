// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/query_cmd/clear_join_plan_cache_gen.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/query_knobs/query_knob_configuration.h"
#include "mongo/db/router_role/cluster_commands_helpers.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {

/**
 * Router counterpart of 'clearJoinPlanCache'. The join plan cache is node-global rather than keyed
 * by collection, so rather than targeting by routing table this broadcasts to every shard, clearing
 * each shard's cache. The router itself holds no join plan cache.
 */
class ClusterClearJoinPlanCacheCmd final : public TypedCommand<ClusterClearJoinPlanCacheCmd> {
public:
    using Request = ClearJoinPlanCacheCommandRequest;

    ClusterClearJoinPlanCacheCmd() : TypedCommand(Request::kCommandName) {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return true;
    }

    std::string help() const override {
        return "Drops all entries from every shard's join plan cache.";
    }

    class Invocation final : public MinimalInvocationBase {
    public:
        using MinimalInvocationBase::MinimalInvocationBase;

    private:
        void run(OperationContext* opCtx, rpc::ReplyBuilderInterface* reply) override {
            // Gate on the knobs here as well, matching how $joinPlanCacheStats is gated on the
            // router before being dispatched to the shards.
            auto& qkc = QueryKnobConfiguration::get(opCtx);
            uassert(ErrorCodes::QueryFeatureNotAllowed,
                    str::stream() << Request::kCommandName
                                  << " requires 'internalEnableJoinOptimization' and "
                                     "'internalEnableJoinPlanCache' to be enabled",
                    qkc.isJoinOrderingEnabled() && qkc.getEnableJoinPlanCache());

            auto shardResponses = scatterGatherUnversionedTargetAllShards(
                opCtx,
                request().getDbName(),
                CommandHelpers::filterCommandRequestForPassthrough(unparsedRequest().body),
                ReadPreferenceSetting::get(opCtx),
                Shard::RetryPolicy::kIdempotent);

            std::string errmsg;
            BSONObjBuilder result;
            const auto rawResponses = appendRawResponses(opCtx, &errmsg, &result, shardResponses);
            uassert(ErrorCodes::OperationFailed, errmsg, rawResponses.responseOK);

            reply->getBodyBuilder().appendElements(result.obj());
        }

        NamespaceString ns() const override {
            // The command is collectionless: it operates on every shard's node-global join plan
            // cache.
            return NamespaceString::kEmpty;
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(
                            ResourcePattern::forDatabaseName(request().getDbName()),
                            ActionType::planCacheWrite));
        }
    };
};
MONGO_REGISTER_COMMAND(ClusterClearJoinPlanCacheCmd).forRouter();

}  // namespace
}  // namespace mongo
