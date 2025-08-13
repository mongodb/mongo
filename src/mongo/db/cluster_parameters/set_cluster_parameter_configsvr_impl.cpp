/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/cluster_parameters/set_cluster_parameter_configsvr_impl.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/cluster_parameters/set_cluster_parameter_coordinator.h"
#include "mongo/db/cluster_parameters/set_cluster_parameter_coordinator_document_gen.h"
#include "mongo/db/cluster_parameters/set_cluster_parameter_invocation.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/ddl/configsvr_coordinator.h"
#include "mongo/db/global_catalog/ddl/configsvr_coordinator_gen.h"
#include "mongo/db/global_catalog/ddl/configsvr_coordinator_service.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/query_settings/query_settings_service_dependencies.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/migration_blocking_operation/migration_blocking_operation_feature_flags_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future.h"
#include "mongo/util/str.h"

#include <memory>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

namespace {
StringData getParameterName(const BSONObj& command) {
    return command.firstElement().fieldName();
}

Status allowedToEnable(const BSONObj& command) {
    auto name = getParameterName(command);
    if (name == "pauseMigrationsDuringMultiUpdates" &&
        !migration_blocking_operation::gFeatureFlagPauseMigrationsDuringMultiUpdatesAvailable
             .isEnabled(serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        return Status{
            ErrorCodes::IllegalOperation,
            "Unable to enable pauseMigrationsDuringMultiUpdates cluster parameter because "
            "pauseMigrationsDuringMultiUpdatesAvailable feature flag is not enabled."};
    }
    return Status::OK();
}

std::shared_ptr<SetClusterParameterCoordinator> makeSetClusterParameterCoordinator(
    OperationContext* opCtx, const ConfigsvrSetClusterParameter& request) {
    // configsvrSetClusterParameter must serialize against
    // setFeatureCompatibilityVersion.
    FixedFCVRegion fcvRegion(opCtx);

    std::unique_ptr<ServerParameterService> sps = std::make_unique<ClusterParameterService>();
    DBDirectClient dbClient(opCtx);
    ClusterParameterDBClientService dbService(dbClient);
    BSONObj cmdParamObj = request.getCommandParameter();
    StringData parameterName = getParameterName(cmdParamObj);
    ServerParameter* serverParameter = sps->get(parameterName);

    uassertStatusOK(allowedToEnable(cmdParamObj));

    SetClusterParameterInvocation invocation{std::move(sps), dbService};

    auto tenantId = request.getDbName().tenantId();
    invocation.normalizeParameter(opCtx,
                                  cmdParamObj,
                                  boost::none /* clusterParameterTime */,
                                  boost::none /* previousTime */,
                                  serverParameter,
                                  tenantId,
                                  false /* skipValidation */);

    SetClusterParameterCoordinatorDocument coordinatorDoc;
    ConfigsvrCoordinatorId cid(ConfigsvrCoordinatorTypeEnum::kSetClusterParameter);
    cid.setSubId(tenantId ? tenantId->toString() : "");
    coordinatorDoc.setConfigsvrCoordinatorMetadata({cid});
    coordinatorDoc.setParameter(cmdParamObj);
    coordinatorDoc.setTenantId(tenantId);
    coordinatorDoc.setPreviousTime(request.getPreviousTime());
    coordinatorDoc.setCompatibleWithTopologyChange(request.get_compatibleWithTopologyChange());

    const auto service = ConfigsvrCoordinatorService::getService(opCtx);
    return dynamic_pointer_cast<SetClusterParameterCoordinator>(
        service->getOrCreateService(opCtx, coordinatorDoc.toBSON()));
}
}  // namespace

void _configsvrSetClusterParameterCmdHandler(OperationContext* opCtx,
                                             const ConfigsvrSetClusterParameter& request) {
    uassert(ErrorCodes::IllegalOperation,
            str::stream() << ConfigsvrSetClusterParameter::kCommandName
                          << " can only be run on config servers",
            serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));

    const auto setClusterParameterCoordinator = makeSetClusterParameterCoordinator(opCtx, request);
    setClusterParameterCoordinator->getCompletionFuture().get(opCtx);

    // If the coordinator detected an unexpected concurrent update, report the error.
    uassert(ErrorCodes::ConflictingOperationInProgress,
            "encountered concurrent cluster parameter update operations, please try again",
            !setClusterParameterCoordinator->detectedConcurrentUpdate());
}

void setClusterParameterImplConfigsvr(OperationContext* opCtx,
                                      const SetClusterParameter& request,
                                      boost::optional<Timestamp>,
                                      boost::optional<LogicalTime> previousTime) {
    ConfigsvrSetClusterParameter configsvrRequest(request.getCommandParameter());
    configsvrRequest.setDbName(request.getDbName());
    configsvrRequest.setPreviousTime(previousTime);

    _configsvrSetClusterParameterCmdHandler(opCtx, configsvrRequest);
}

namespace {
ServiceContext::ConstructorActionRegisterer SetClusterParameterConfigsvrRegisterer(
    "SetClusterParameterConfigsvr",
    {},
    {"QuerySettingsService"},
    [](ServiceContext* serviceContext) {
        query_settings::getServiceDependencies(serviceContext).setClusterParameterConfigsvr =
            setClusterParameterImplConfigsvr;
    },
    {});
}  // namespace
}  // namespace mongo
