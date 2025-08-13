/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/cluster_parameters/set_cluster_parameter_replset_impl.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/cluster_parameters/set_cluster_parameter_invocation.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/query_settings/query_settings_service_dependencies.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
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
