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

#include "mongo/idl/cluster_server_parameter_refresher.h"

#include "mongo/db/audit.h"
#include "mongo/db/commands/list_databases_for_all_tenants_gen.h"
#include "mongo/db/multitenancy_gen.h"
#include "mongo/idl/cluster_server_parameter_common.h"
#include "mongo/idl/cluster_server_parameter_refresher_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/s/grid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

namespace mongo {
namespace {

const auto getClusterServerParameterRefresher =
    ServiceContext::declareDecoration<std::unique_ptr<ClusterServerParameterRefresher>>();

Seconds loadInterval() {
    return Seconds(clusterServerParameterRefreshIntervalSecs.load());
}

StatusWith<TenantIdMap<stdx::unordered_map<std::string, BSONObj>>>
getClusterParametersFromConfigServer(OperationContext* opCtx) {
    // Attempt to retrieve cluster parameter documents from the config server.
    // exhaustiveFindOnConfig makes up to 3 total attempts if it receives a retriable error before
    // giving up.
    LOGV2_DEBUG(6226404, 3, "Retrieving cluster server parameters from config server");
    auto configServers = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    auto swTenantIds = getTenantsWithConfigDbsOnShard(opCtx, configServers.get());
    if (!swTenantIds.isOK()) {
        return swTenantIds.getStatus();
    }
    auto tenantIds = std::move(swTenantIds.getValue());

    TenantIdMap<stdx::unordered_map<std::string, BSONObj>> allDocs;
    for (const auto& tenantId : tenantIds) {
        auto swFindResponse = configServers->exhaustiveFindOnConfig(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            repl::ReadConcernLevel::kMajorityReadConcern,
            NamespaceString::makeClusterParametersNSS(tenantId),
            BSONObj(),
            BSONObj(),
            boost::none);

        // If the error is not retriable or persists beyond the max number of retry attempts, give
        // up and throw an error.
        if (!swFindResponse.isOK()) {
            return swFindResponse.getStatus();
        }
        stdx::unordered_map<std::string, BSONObj> docsMap;
        for (const auto& doc : swFindResponse.getValue().docs) {
            auto name = doc["_id"].String();
            docsMap.insert({std::move(name), doc});
        }
        allDocs.insert({std::move(tenantId), std::move(docsMap)});
    }

    return allDocs;
}

}  // namespace

Status clusterServerParameterRefreshIntervalSecsNotify(const int& newValue) {
    LOGV2_DEBUG(6226400,
                5,
                "Set clusterServerParameterRefresher interval seconds",
                "clusterServerParameterRefreshIntervalSecs"_attr = loadInterval());
    if (hasGlobalServiceContext()) {
        auto service = getGlobalServiceContext();
        if (getClusterServerParameterRefresher(service)) {
            getClusterServerParameterRefresher(service)->setPeriod(loadInterval());
        }
    }

    return Status::OK();
}

ClusterServerParameterRefresher* ClusterServerParameterRefresher::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

ClusterServerParameterRefresher* ClusterServerParameterRefresher::get(ServiceContext* serviceCtx) {
    return getClusterServerParameterRefresher(serviceCtx).get();
}

void ClusterServerParameterRefresher::setPeriod(Milliseconds period) {
    _job->setPeriod(period);
}

Status ClusterServerParameterRefresher::refreshParameters(OperationContext* opCtx) {
    // Query the config servers for all cluster parameter documents.
    auto swClusterParameterDocs = getClusterParametersFromConfigServer(opCtx);
    if (!swClusterParameterDocs.isOK()) {
        LOGV2_WARNING(6226401,
                      "Could not refresh cluster server parameters from config servers. Will retry "
                      "after refresh interval elapses",
                      "clusterServerParameterRefreshIntervalSecs"_attr = loadInterval(),
                      "reason"_attr = swClusterParameterDocs.getStatus().reason());
        return swClusterParameterDocs.getStatus();
    }

    // Set each in-memory cluster parameter that was returned in the response.
    bool isSuccessful = true;
    Status status = Status::OK();
    ServerParameterSet* clusterParameterCache = ServerParameterSet::getClusterParameterSet();

    auto clusterParameterDocs = std::move(swClusterParameterDocs.getValue());
    std::vector<BSONObj> allUpdatedParameters;
    allUpdatedParameters.reserve(clusterParameterDocs.size());

    for (const auto& [tenantId, tenantParamDocs] : clusterParameterDocs) {
        std::vector<BSONObj> updatedParameters;
        updatedParameters.reserve(tenantParamDocs.size());
        for (const auto& [name, sp] : clusterParameterCache->getMap()) {
            if (!sp->isEnabled()) {
                continue;
            }
            BSONObjBuilder oldClusterParameterBob;
            sp->append(opCtx, &oldClusterParameterBob, name, tenantId);

            auto it = tenantParamDocs.find(name);
            if (it == tenantParamDocs.end()) {
                // Reset the local parameter to its default value.
                status = sp->reset(tenantId);
            } else {
                // Set the local parameter to the pulled value.
                const auto& clusterParameterDoc = it->second;
                status = sp->set(clusterParameterDoc, tenantId);
            }

            if (!status.isOK()) {
                LOGV2_WARNING(6226402,
                              "Could not (re)set in-memory cluster server parameter",
                              "parameter"_attr = name,
                              "tenantId"_attr = tenantId,
                              "presentOnConfigSvr"_attr = it != tenantParamDocs.end(),
                              "reason"_attr = status.reason());
                isSuccessful = false;
            }

            BSONObjBuilder updatedClusterParameterBob;
            sp->append(opCtx, &updatedClusterParameterBob, name, tenantId);
            BSONObj updatedClusterParameterBSON = updatedClusterParameterBob.obj().getOwned();

            audit::logUpdateCachedClusterParameter(opCtx->getClient(),
                                                   oldClusterParameterBob.obj().getOwned(),
                                                   updatedClusterParameterBSON,
                                                   tenantId);
            if (it != tenantParamDocs.end()) {
                updatedParameters.emplace_back(
                    updatedClusterParameterBSON.removeField("clusterParameterTime"_sd));
            }
        }
        auto tenantIdStr = tenantId ? tenantId->toString() : "none";
        allUpdatedParameters.emplace_back(
            BSON("tenantId" << tenantIdStr << "updatedParameters" << updatedParameters));
    }

    if (isSuccessful) {
        LOGV2_DEBUG(6226403,
                    3,
                    "Updated cluster server parameters",
                    "clusterParameterDocuments"_attr = allUpdatedParameters);
    }

    return status;
}

void ClusterServerParameterRefresher::start(ServiceContext* serviceCtx, OperationContext* opCtx) {
    auto refresher = std::make_unique<ClusterServerParameterRefresher>();

    auto periodicRunner = serviceCtx->getPeriodicRunner();
    invariant(periodicRunner);

    PeriodicRunner::PeriodicJob job(
        "ClusterServerParameterRefresher",
        [serviceCtx](Client* client) { getClusterServerParameterRefresher(serviceCtx)->run(); },
        loadInterval());

    refresher->_job = std::make_unique<PeriodicJobAnchor>(periodicRunner->makeJob(std::move(job)));

    // Make sure the invalidator is moved to the service context by the time we call start()
    getClusterServerParameterRefresher(serviceCtx) = std::move(refresher);
    getClusterServerParameterRefresher(serviceCtx)->_job->start();
}

void ClusterServerParameterRefresher::run() {
    auto opCtx = cc().makeOperationContext();
    auto status = refreshParameters(opCtx.get());
    if (!status.isOK()) {
        LOGV2_DEBUG(
            6226405, 1, "Cluster server parameter refresh failed", "reason"_attr = status.reason());
    }
}

}  // namespace mongo
