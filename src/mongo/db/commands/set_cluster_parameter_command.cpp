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


#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <memory>
#include <string>
#include <utility>

#include "mongo/base/error_codes.h"
#include "mongo/base/shim.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/cluster_role.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/cluster_server_parameter_cmds_gen.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/commands/set_cluster_parameter_invocation.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {
const WriteConcernOptions kMajorityWriteConcern{WriteConcernOptions::kMajority,
                                                WriteConcernOptions::SyncMode::UNSET,
                                                WriteConcernOptions::kNoTimeout};

void setClusterParameterImpl(OperationContext* opCtx,
                             const SetClusterParameter& request,
                             boost::optional<Timestamp> clusterParameterTime,
                             boost::optional<LogicalTime> previousTime) {
    uassert(ErrorCodes::ErrorCodes::NotImplemented,
            "setClusterParameter can only run on mongos in sharded clusters",
            (serverGlobalParams.clusterRole.has(ClusterRole::None)));

    if (!feature_flags::gFeatureFlagAuditConfigClusterParameter.isEnabled(
            serverGlobalParams.featureCompatibility)) {
        uassert(ErrorCodes::IllegalOperation,
                str::stream() << SetClusterParameter::kCommandName
                              << " cannot be run on standalones",
                repl::ReplicationCoordinator::get(opCtx)->getSettings().isReplSet());
    }

    // setClusterParameter is serialized against setFeatureCompatibilityVersion.
    FixedFCVRegion fcvRegion(opCtx);

    std::unique_ptr<ServerParameterService> parameterService =
        std::make_unique<ClusterParameterService>();

    DBDirectClient dbClient(opCtx);
    ClusterParameterDBClientService dbService(dbClient);

    SetClusterParameterInvocation invocation{std::move(parameterService), dbService};

    invocation.invoke(opCtx, request, clusterParameterTime, previousTime, kMajorityWriteConcern);
}

class SetClusterParameterCommand final : public TypedCommand<SetClusterParameterCommand> {
public:
    using Request = SetClusterParameter;

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    std::string help() const override {
        return "Set cluster parameter on replica set or node";
    }

    bool allowedWithSecurityToken() const final {
        return true;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            setClusterParameterImpl(opCtx,
                                    request(),
                                    boost::none /* clusterParameterTime */,
                                    boost::none /* previousTime */);
        }

    private:
        bool supportsWriteConcern() const override {
            return false;
        }

        NamespaceString ns() const override {
            return NamespaceString();
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
MONGO_REGISTER_COMMAND(SetClusterParameterCommand).forShard();

auto setClusterParameterRegistration =
    MONGO_WEAK_FUNCTION_REGISTRATION(setClusterParameter, setClusterParameterImpl);

}  // namespace
}  // namespace mongo
