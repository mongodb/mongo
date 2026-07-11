// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/topology/cluster_parameters/cluster_server_parameter_server_status.h"

#include "mongo/db/server_parameter_with_storage.h"

namespace mongo {

namespace {
std::set<std::string> getDefaultReportedParameters() {
    return {"pauseMigrationsDuringMultiUpdates"};
}

bool isParameterSet(OperationContext* opCtx, ServerParameter* param) {
    if (!param) {
        return false;
    }
    if (!param->isEnabled(VersionContext::getDecoration(opCtx))) {
        return false;
    }
    if (param->getClusterParameterTime(boost::none) == LogicalTime::kUninitialized) {
        return false;
    }
    return true;
}
}  // namespace

ClusterServerParameterServerStatus::ClusterServerParameterServerStatus()
    : ClusterServerParameterServerStatus(ServerParameterSet::getClusterParameterSet(),
                                         getDefaultReportedParameters()) {}

ClusterServerParameterServerStatus::ClusterServerParameterServerStatus(
    ServerParameterSet* clusterParameters, std::set<std::string> reportedParameters)
    : _clusterParameters{clusterParameters}, _reportedParameters{std::move(reportedParameters)} {}

void ClusterServerParameterServerStatus::report(OperationContext* opCtx,
                                                BSONObjBuilder* bob) const {
    auto reportedParameters = _getParametersToReport(opCtx);
    if (reportedParameters.isEmpty()) {
        return;
    }
    bob->append(kClusterParameterFieldName, reportedParameters);
}

BSONObj ClusterServerParameterServerStatus::_getParametersToReport(OperationContext* opCtx) const {
    BSONObjBuilder mapBuilder;
    for (const auto& name : _reportedParameters) {
        auto param = _clusterParameters->getIfExists(name);
        if (!isParameterSet(opCtx, param)) {
            continue;
        }
        BSONObjBuilder elementBuilder(mapBuilder.subobjStart(name));
        param->append(opCtx, &elementBuilder, name, boost::none);
    }
    return mapBuilder.obj();
}

}  // namespace mongo
