// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/topology/cluster_parameters/set_cluster_parameter_configsvr_impl.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
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
#include "mongo/db/topology/cluster_parameters/set_cluster_parameter_coordinator.h"
#include "mongo/db/topology/cluster_parameters/set_cluster_parameter_coordinator_document_gen.h"
#include "mongo/db/topology/cluster_parameters/set_cluster_parameter_invocation.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future.h"
#include "mongo/util/str.h"

#include <memory>
#include <string_view>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

namespace {

std::shared_ptr<SetClusterParameterCoordinator> makeSetClusterParameterCoordinator(
    OperationContext* opCtx, const ConfigsvrSetClusterParameter& request) {
    // configsvrSetClusterParameter must serialize against
    // setFeatureCompatibilityVersion.
    FixedFCVRegion fcvRegion(opCtx);

    std::unique_ptr<ServerParameterService> sps = std::make_unique<ClusterParameterService>();
    DBDirectClient dbClient(opCtx);
    ClusterParameterDBClientService dbService(dbClient);
    BSONObj cmdParamObj = request.getCommandParameter();
    std::string_view parameterName = cmdParamObj.firstElement().fieldName();
    ServerParameter* serverParameter = sps->get(parameterName);

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
