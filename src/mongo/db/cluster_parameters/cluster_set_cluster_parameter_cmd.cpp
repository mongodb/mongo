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
#include "mongo/base/shim.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/cluster_parameters/cluster_server_parameter_cmds_gen.h"
#include "mongo/db/cluster_parameters/set_cluster_server_parameter_router_impl.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/query_settings/query_settings_service.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/update/storage_validation.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

class SetClusterParameterCmd final : public TypedCommand<SetClusterParameterCmd> {
public:
    using Request = SetClusterParameter;

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    std::string help() const override {
        return "Set a cluster wide parameter on every node";
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            auto service = opCtx->getService();
            invariant(service->role().hasExclusively(ClusterRole::RouterServer),
                      "Attempted to run a router-only command directly from the shard role.");

            uassert(ErrorCodes::NoSuchKey,
                    "No cluster parameter provided",
                    request().getCommandParameter().nFields() > 0);

            uassert(ErrorCodes::InvalidOptions,
                    fmt::format("{} only supports setting exactly one parameter",
                                Request::kCommandName),
                    request().getCommandParameter().nFields() == 1);

            auto querySettingsClusterParameterName =
                query_settings::QuerySettingsService::getQuerySettingsClusterParameterName();
            uassert(ErrorCodes::NoSuchKey,
                    fmt::format("Unknown server parameter: {}", querySettingsClusterParameterName),
                    !request().getCommandParameter()[querySettingsClusterParameterName]);

            {
                bool ignore;
                mutablebson::Document mutableUpdate(request().getCommandParameter());
                storage_validation::scanDocument(mutableUpdate, false, true, &ignore);
            }

            setClusterParameterImplRouter(opCtx,
                                          request(),
                                          boost::none /* clusterParameterTime */,
                                          boost::none /* previousTime */);
        }

    private:
        bool supportsWriteConcern() const override {
            return false;
        }

        NamespaceString ns() const override {
            return NamespaceString::kEmpty;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForPrivilege(Privilege{
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::setClusterParameter}));
        }
    };
};
MONGO_REGISTER_COMMAND(SetClusterParameterCmd).forRouter();

}  // namespace
}  // namespace mongo
