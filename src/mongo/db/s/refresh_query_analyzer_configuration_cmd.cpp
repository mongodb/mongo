// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/multitenancy_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/query_analysis_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/analyze_shard_key_common_gen.h"
#include "mongo/s/refresh_query_analyzer_configuration_cmd_gen.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {

namespace {

class RefreshQueryAnalyzerConfigurationCmd
    : public TypedCommand<RefreshQueryAnalyzerConfigurationCmd> {
public:
    using Request = RefreshQueryAnalyzerConfiguration;
    using Response = RefreshQueryAnalyzerConfigurationResponse;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Response typedRun(OperationContext* opCtx) {
            uassert(ErrorCodes::IllegalOperation,
                    "_refreshQueryAnalyzerConfiguration command is not supported on a standalone "
                    "mongod",
                    repl::ReplicationCoordinator::get(opCtx)->getSettings().isReplSet());
            uassert(ErrorCodes::IllegalOperation,
                    "_refreshQueryAnalyzerConfiguration command is not supported on a multitenant "
                    "replica set",
                    !gMultitenancySupport);
            uassert(
                ErrorCodes::IllegalOperation,
                "_refreshQueryAnalyzerConfiguration command is not supported on a shardsvr mongod",
                !serverGlobalParams.clusterRole.isShardOnly());

            auto coodinator = analyze_shard_key::QueryAnalysisCoordinator::get(opCtx);
            auto configurations = coodinator->getNewConfigurationsForSampler(
                opCtx, request().getName(), request().getNumQueriesExecutedPerSecond());
            return {configurations};
        }

    private:
        NamespaceString ns() const override {
            return NamespaceString(DatabaseName::kAdmin);
        }

        bool supportsWriteConcern() const override {
            return false;
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

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool maintenanceOk() const override {
        return false;
    }

    bool adminOnly() const override {
        return true;
    }

    std::string help() const override {
        return "Refreshes the query analyzer configurations for all collections.";
    }
};
MONGO_REGISTER_COMMAND(RefreshQueryAnalyzerConfigurationCmd).forShard();

}  // namespace

}  // namespace mongo
