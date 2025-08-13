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
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/cluster_parameters/cluster_server_parameter_cmds_gen.h"
#include "mongo/db/cluster_parameters/cluster_server_parameter_refresher.h"
#include "mongo/db/cluster_parameters/get_cluster_parameter_invocation.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/service_context.h"
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
