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

#include "mongo/idl/cluster_server_parameter_initializer.h"

#include "mongo/base/string_data.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/repl/replica_set_aware_service.h"
#include "mongo/db/service_context.h"
#include "mongo/idl/cluster_parameter_synchronization_helpers.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

namespace mongo {

namespace {
const auto getInstance = ServiceContext::declareDecoration<ClusterServerParameterInitializer>();
const ReplicaSetAwareServiceRegistry::Registerer<ClusterServerParameterInitializer> _registerer(
    "ClusterServerParameterInitializerRegistry");

constexpr auto kIdField = "_id"_sd;
constexpr auto kCPTField = "clusterParameterTime"_sd;
constexpr auto kOplog = "oplog"_sd;

}  // namespace

ClusterServerParameterInitializer* ClusterServerParameterInitializer::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

ClusterServerParameterInitializer* ClusterServerParameterInitializer::get(
    ServiceContext* serviceContext) {
    return &getInstance(serviceContext);
}

void ClusterServerParameterInitializer::onInitialDataAvailable(OperationContext* opCtx,
                                                               bool isMajorityDataAvailable) {
    synchronizeAllParametersFromDisk(opCtx);
}

void ClusterServerParameterInitializer::synchronizeAllParametersFromDisk(OperationContext* opCtx) {
    LOGV2_INFO(6608200, "Initializing cluster server parameters from disk");
    if (gMultitenancySupport) {
        std::set<TenantId> tenantIds;
        auto catalog = CollectionCatalog::get(opCtx);
        {
            Lock::GlobalLock lk(opCtx, MODE_IS);
            tenantIds = catalog->getAllTenants();
        }

        for (const auto& tenantId : tenantIds) {
            cluster_parameters::initializeAllTenantParametersFromDisk(opCtx, tenantId);
        }
    }
    cluster_parameters::initializeAllTenantParametersFromDisk(opCtx, boost::none);
}

}  // namespace mongo
