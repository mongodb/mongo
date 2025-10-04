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

#include "mongo/db/cluster_parameters/cluster_server_parameter_server_status.h"

#include "mongo/db/server_parameter_with_storage.h"

namespace mongo {

namespace {
std::set<std::string> getDefaultReportedParameters() {
    return {"pauseMigrationsDuringMultiUpdates"};
}

bool isParameterSet(ServerParameter* param) {
    if (!param) {
        return false;
    }
    if (!param->isEnabled()) {
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
        if (!isParameterSet(param)) {
            continue;
        }
        BSONObjBuilder elementBuilder(mapBuilder.subobjStart(name));
        param->append(opCtx, &elementBuilder, name, boost::none);
    }
    return mapBuilder.obj();
}

}  // namespace mongo
