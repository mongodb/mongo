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

StatusWith<std::vector<BSONObj>> getClusterParametersFromConfigServer(
    OperationContext* opCtx, const LogicalTime& latestTime) {
    BSONObjBuilder queryObjBuilder;
    BSONObjBuilder clusterParameterTimeObjBuilder =
        queryObjBuilder.subobjStart("clusterParameterTime"_sd);
    clusterParameterTimeObjBuilder.appendTimestamp("$gt"_sd, latestTime.asTimestamp().asInt64());
    clusterParameterTimeObjBuilder.doneFast();

    BSONObj query = queryObjBuilder.obj();

    // Attempt to retrieve cluster parameter documents from the config server.
    // exhaustiveFindOnConfig makes up to 3 total attempts if it receives a retriable error before
    // giving up.
    LOGV2_DEBUG(6226404, 3, "Retrieving cluster server parameters from config server");
    auto configServers = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    auto swFindResponse =
        configServers->exhaustiveFindOnConfig(opCtx,
                                              ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                              repl::ReadConcernLevel::kMajorityReadConcern,
                                              NamespaceString::kClusterParametersNamespace,
                                              query,
                                              BSONObj(),
                                              boost::none);

    // If the error is not retriable or persists beyond the max number of retry attempts, give up
    // and throw an error.
    if (!swFindResponse.isOK()) {
        return swFindResponse.getStatus();
    }

    return swFindResponse.getValue().docs;
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
    // Query the config servers for all cluster parameter documents with
    // clusterParameterTime greater than the largest in-memory timestamp.
    auto swClusterParameterDocs =
        getClusterParametersFromConfigServer(opCtx, _latestClusterParameterTime);
    if (!swClusterParameterDocs.isOK()) {
        LOGV2_WARNING(6226401,
                      "Could not refresh cluster server parameters from config servers. Will retry "
                      "after refresh interval elapses",
                      "clusterServerParameterRefreshIntervalSecs"_attr = loadInterval(),
                      "reason"_attr = swClusterParameterDocs.getStatus().reason());
        return swClusterParameterDocs.getStatus();
    }

    // Set each in-memory cluster parameter that was returned in the response. Then, advance the
    // latest clusterParameterTime to the latest one returned if all of the cluster parameters are
    // successfully set in-memory.
    Timestamp latestTime;
    bool isSuccessful = true;
    Status setStatus = Status::OK();
    ServerParameterSet* clusterParameterCache = ServerParameterSet::getClusterParameterSet();
    std::vector<BSONObj> clusterParameterDocs = swClusterParameterDocs.getValue();
    std::vector<BSONObj> updatedParameters;
    updatedParameters.reserve(clusterParameterDocs.size());

    for (const auto& clusterParameterDoc : clusterParameterDocs) {
        Timestamp clusterParameterTime = clusterParameterDoc["clusterParameterTime"_sd].timestamp();
        latestTime = (clusterParameterTime > latestTime) ? clusterParameterTime : latestTime;

        auto clusterParameterName = clusterParameterDoc["_id"_sd].String();
        ServerParameter* sp = clusterParameterCache->get(clusterParameterName);

        BSONObjBuilder oldClusterParameterBob;
        sp->append(opCtx, &oldClusterParameterBob, clusterParameterName, boost::none);

        setStatus = sp->set(clusterParameterDoc, boost::none);
        if (!setStatus.isOK()) {
            LOGV2_WARNING(6226402,
                          "Could not set in-memory cluster server parameter",
                          "parameter"_attr = clusterParameterName,
                          "reason"_attr = setStatus.reason());
            isSuccessful = false;
        }

        BSONObjBuilder updatedClusterParameterBob;
        sp->append(opCtx, &updatedClusterParameterBob, clusterParameterName, boost::none);
        BSONObj updatedClusterParameterBSON = updatedClusterParameterBob.obj().getOwned();

        audit::logUpdateCachedClusterParameter(opCtx->getClient(),
                                               oldClusterParameterBob.obj().getOwned(),
                                               updatedClusterParameterBSON);

        updatedParameters.emplace_back(
            updatedClusterParameterBSON.removeField("clusterParameterTime"_sd));
    }

    if (isSuccessful) {
        _latestClusterParameterTime = LogicalTime(latestTime);
        LOGV2_DEBUG(6226403,
                    3,
                    "Updated cluster server parameters",
                    "clusterParameterDocuments"_attr = updatedParameters);
    }

    return setStatus;
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
