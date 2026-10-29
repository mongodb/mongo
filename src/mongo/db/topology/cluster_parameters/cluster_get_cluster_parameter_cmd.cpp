// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/error_codes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/service_context.h"
#include "mongo/db/topology/cluster_parameters/cluster_server_parameter_cmds_gen.h"
#include "mongo/db/topology/cluster_parameters/cluster_server_parameter_refresher.h"
#include "mongo/db/topology/cluster_parameters/get_cluster_parameter_invocation.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

class GetClusterParameterCmd final : public TypedCommand<GetClusterParameterCmd> {
public:
    using Request = GetClusterParameter;
    using Reply = GetClusterParameter::Reply;
    using Map = ServerParameterSet::Map;

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return true;
    }

    std::string help() const override {
        return "Refresh and get cached cluster server parameter value from mongos.";
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Reply typedRun(OperationContext* opCtx) {
            GetClusterParameterInvocation invocation;

            if (serverGlobalParams.clusterRole.hasExclusively(ClusterRole::RouterServer)) {
                // Refresh cached cluster server parameters via a majority read from the config
                // servers.
                uassertStatusOK(
                    ClusterServerParameterRefresher::get(opCtx)->refreshParameters(opCtx));
            } else {
                // If we're an embedded router, we don't have a separate cache of the parameters and
                // refresh mechanism. We rely on the co-process shard-role to update our in-memory
                // parameters. Double check that we have the router-role if we're running this
                // router command.
                bool isEmbeddedRouter =
                    serverGlobalParams.clusterRole.has(ClusterRole::RouterServer) &&
                    serverGlobalParams.clusterRole.has(ClusterRole::ShardServer);
                invariant(isEmbeddedRouter);
            }

            return invocation.getCachedParameters(opCtx, request());
        }

    private:
        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const final {
            auto* authzSession = AuthorizationSession::get(opCtx->getClient());
            uassert(ErrorCodes::Unauthorized,
                    "Not authorized to retrieve cluster parameters",
                    authzSession->isAuthorizedForPrivilege(Privilege{
                        ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                        ActionType::getClusterParameter}));
        }

        NamespaceString ns() const override {
            return NamespaceString(request().getDbName());
        }
    };
};
MONGO_REGISTER_COMMAND(GetClusterParameterCmd).forRouter();

}  // namespace
}  // namespace mongo
