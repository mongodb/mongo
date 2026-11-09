// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/topology/cluster_parameters/cluster_server_parameter_initializer.h"

#include "mongo/db/repl/replica_set_aware_service.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/topology/cluster_parameters/cluster_parameter_synchronization_helpers.h"
#include "mongo/logv2/log.h"
#include "mongo/util/decorable.h"

#include <memory>
#include <string_view>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

namespace mongo {

namespace {
using namespace std::literals::string_view_literals;
const auto getInstance = ServiceContext::declareDecoration<ClusterServerParameterInitializer>();
const ReplicaSetAwareServiceRegistry::Registerer<ClusterServerParameterInitializer> _registerer(
    "ClusterServerParameterInitializerRegistry");

constexpr auto kIdField = "_id"sv;
constexpr auto kCPTField = "clusterParameterTime"sv;
constexpr auto kOplog = "oplog"sv;

}  // namespace

ClusterServerParameterInitializer* ClusterServerParameterInitializer::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

ClusterServerParameterInitializer* ClusterServerParameterInitializer::get(
    ServiceContext* serviceContext) {
    return &getInstance(serviceContext);
}

void ClusterServerParameterInitializer::onConsistentDataAvailable(OperationContext* opCtx,
                                                                  bool isMajority,
                                                                  bool isRollback) {
    // TODO (SERVER-91506): Determine if we should reload in-memory states on rollback.
    if (isRollback) {
        return;
    }
    synchronizeAllParametersFromDisk(opCtx);
}

void ClusterServerParameterInitializer::synchronizeAllParametersFromDisk(OperationContext* opCtx) {
    LOGV2_INFO(6608200, "Initializing cluster server parameters from disk");

    auto initializeTenantParameters = [opCtx](const boost::optional<TenantId>& tenantId) {
        const auto coll = acquireCollection(
            opCtx,
            CollectionAcquisitionRequest(NamespaceString::makeClusterParametersNSS(tenantId),
                                         PlacementConcern(boost::none, ShardVersion::UNTRACKED()),
                                         repl::ReadConcernArgs::get(opCtx),
                                         AcquisitionPrerequisites::kRead),
            MODE_IS);
        if (coll.exists()) {
            cluster_parameters::initializeAllTenantParametersFromCollection(
                opCtx, *coll.getCollectionPtr().get());
        }
    };

    // The in-memory cache is single-valued, so only the tenantless parameters are loaded.
    // Tenant-scoped cluster parameter collections are ignored.
    initializeTenantParameters(boost::none);
}

}  // namespace mongo
