// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/query_cmd/clear_join_plan_cache_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/plan_cache/join_plan_cache.h"
#include "mongo/db/query/query_knobs/query_knob_configuration.h"
#include "mongo/db/service_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {

/**
 * The 'clearJoinPlanCache' command drops every entry from this node's join plan cache:
 *
 *    db.adminCommand({clearJoinPlanCache: 1})
 *
 * Unlike 'planCacheClear', the join plan cache is node-global rather than owned by a collection, so
 * the command is collectionless and admin-only and takes no arguments. The cache is not replicated,
 * so the command affects only the node it is sent to; use the router counterpart to clear every
 * shard in a cluster.
 */
class ClearJoinPlanCacheCommand final : public TypedCommand<ClearJoinPlanCacheCommand> {
public:
    using Request = ClearJoinPlanCacheCommandRequest;
    using Reply = OkReply;

    ClearJoinPlanCacheCommand() : TypedCommand(Request::kCommandName) {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        // The join plan cache is node-local and not replicated, so this command must be allowed to
        // run on secondaries to clear their local caches.
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return true;
    }

    std::string help() const override {
        return "Drops all entries from this node's join plan cache.";
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            // The join plan cache is only populated when join optimization and the join plan cache
            // are enabled, so gate the command on both server parameters, matching
            // $joinPlanCacheStats. Checked after authorization so that an unauthorized user cannot
            // use the command to probe the server's configuration.
            auto& qkc = QueryKnobConfiguration::get(opCtx);
            uassert(ErrorCodes::QueryFeatureNotAllowed,
                    str::stream() << Request::kCommandName
                                  << " requires 'internalEnableJoinOptimization' and "
                                     "'internalEnableJoinPlanCache' to be enabled",
                    qkc.isJoinOrderingEnabled() && qkc.getEnableJoinPlanCache());

            JoinPlanCache::get(opCtx->getServiceContext()).clear();
        }

    private:
        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(
                            ResourcePattern::forDatabaseName(request().getDbName()),
                            ActionType::planCacheWrite));
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        NamespaceString ns() const override {
            // The command is collectionless: it operates on the node-global join plan cache.
            return NamespaceString::kEmpty;
        }
    };
};
MONGO_REGISTER_COMMAND(ClearJoinPlanCacheCommand).forShard();

}  // namespace
}  // namespace mongo
