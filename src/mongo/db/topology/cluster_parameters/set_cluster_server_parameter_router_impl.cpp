// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/topology/cluster_parameters/set_cluster_server_parameter_router_impl.h"

#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/query/query_settings/query_settings_service_dependencies.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

void setClusterParameterImplRouter(OperationContext* opCtx,
                                   const SetClusterParameter& request,
                                   boost::optional<Timestamp>,
                                   boost::optional<LogicalTime> previousTime) {
    ConfigsvrSetClusterParameter configsvrSetClusterParameter(request.getCommandParameter());
    configsvrSetClusterParameter.setDbName(request.getDbName());
    configsvrSetClusterParameter.setPreviousTime(previousTime);

    const auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

    const auto cmdResponse =
        uassertStatusOK(configShard->runCommand(opCtx,
                                                ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                                DatabaseName::kAdmin,
                                                configsvrSetClusterParameter.toBSON(),
                                                Shard::RetryPolicy::kIdempotent));

    uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(std::move(cmdResponse)));
}

}  // namespace mongo
