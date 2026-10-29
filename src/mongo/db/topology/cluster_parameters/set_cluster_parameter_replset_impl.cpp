// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/topology/cluster_parameters/set_cluster_parameter_replset_impl.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/query_settings/query_settings_service_dependencies.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/topology/cluster_parameters/set_cluster_parameter_invocation.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {
MONGO_FAIL_POINT_DEFINE(hangInSetClusterParameter);

const WriteConcernOptions kMajorityWriteConcern{WriteConcernOptions::kMajority,
                                                WriteConcernOptions::SyncMode::UNSET,
                                                WriteConcernOptions::kNoTimeout};

void hangInSetClusterParameterFailPointCheck(const SetClusterParameter& request) {
    if (MONGO_unlikely(hangInSetClusterParameter.shouldFail())) {
        hangInSetClusterParameter.pauseWhileSet();
    }
}

ServiceContext::ConstructorActionRegisterer setClusterParameterReplSetRegisterer(
    "SetClusterParameterReplset",
    {},
    {"QuerySettingsService"},
    [](ServiceContext* serviceContext) {
        query_settings::getServiceDependencies(serviceContext).setClusterParameterReplSet =
            setClusterParameterImplReplicaSetOrStandalone;
    },
    {});
}  // namespace

void setClusterParameterImplReplicaSetOrStandalone(OperationContext* opCtx,
                                                   const SetClusterParameter& request,
                                                   boost::optional<Timestamp> clusterParameterTime,
                                                   boost::optional<LogicalTime> previousTime) {
    uassert(ErrorCodes::ErrorCodes::NotImplemented,
            "setClusterParameter can only run on mongos in sharded clusters",
            (serverGlobalParams.clusterRole.has(ClusterRole::None)));

    // setClusterParameter is serialized against setFeatureCompatibilityVersion.
    FixedFCVRegion fcvRegion(opCtx);

    hangInSetClusterParameterFailPointCheck(request);

    std::unique_ptr<ServerParameterService> parameterService =
        std::make_unique<ClusterParameterService>();

    DBDirectClient dbClient(opCtx);
    ClusterParameterDBClientService dbService(dbClient);

    SetClusterParameterInvocation invocation{std::move(parameterService), dbService};

    invocation.invoke(opCtx, request, clusterParameterTime, previousTime, kMajorityWriteConcern);
}

}  // namespace mongo
