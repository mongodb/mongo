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


#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/commands/set_cluster_parameter_invocation.h"
#include "mongo/db/s/config/configsvr_coordinator_service.h"
#include "mongo/db/s/config/set_cluster_parameter_coordinator.h"
#include "mongo/idl/cluster_server_parameter_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

class ConfigsvrSetClusterParameterCommand final
    : public TypedCommand<ConfigsvrSetClusterParameterCommand> {
public:
    using Request = ConfigsvrSetClusterParameter;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            uassert(ErrorCodes::IllegalOperation,
                    str::stream() << Request::kCommandName << " can only be run on config servers",
                    serverGlobalParams.clusterRole == ClusterRole::ConfigServer);

            const auto coordinatorCompletionFuture = [&]() -> SharedSemiFuture<void> {
                std::unique_ptr<ServerParameterService> sps =
                    std::make_unique<ClusterParameterService>();
                DBDirectClient dbClient(opCtx);
                ClusterParameterDBClientService dbService(dbClient);
                BSONObj cmdParamObj = request().getCommandParameter();
                StringData parameterName = cmdParamObj.firstElement().fieldName();
                ServerParameter* serverParameter = sps->get(parameterName);

                SetClusterParameterInvocation invocation{std::move(sps), dbService};

                invocation.normalizeParameter(opCtx,
                                              cmdParamObj,
                                              boost::none,
                                              serverParameter,
                                              parameterName,
                                              request().getDbName().tenantId());

                auto tenantId = request().getDbName().tenantId();

                SetClusterParameterCoordinatorDocument coordinatorDoc;
                ConfigsvrCoordinatorId cid(ConfigsvrCoordinatorTypeEnum::kSetClusterParameter);
                cid.setSubId(StringData(tenantId ? tenantId->toString() : ""));
                coordinatorDoc.setConfigsvrCoordinatorMetadata({cid});
                coordinatorDoc.setParameter(request().getCommandParameter());
                coordinatorDoc.setTenantId(tenantId);

                const auto service = ConfigsvrCoordinatorService::getService(opCtx);
                const auto instance = service->getOrCreateService(opCtx, coordinatorDoc.toBSON());

                return instance->getCompletionFuture();
            }();

            coordinatorCompletionFuture.get(opCtx);
        }

    private:
        NamespaceString ns() const override {
            return NamespaceString(request().getDbName());
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                           ActionType::internal));
        }
    };

    std::string help() const override {
        return "Internal command, which is exported by the config servers. Do not call "
               "directly. Sets a parameter in the cluster.";
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }
} configsvrSetClusterParameterCmd;

}  // namespace
}  // namespace mongo
