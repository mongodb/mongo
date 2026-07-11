// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"

namespace mongo {

class [[MONGO_MOD_PARENT_PRIVATE]] ClusterServerParameterServerStatus {
public:
    static constexpr auto kClusterParameterFieldName = "clusterParameters";

    ClusterServerParameterServerStatus();
    ClusterServerParameterServerStatus(ServerParameterSet* clusterParameters,
                                       std::set<std::string> reportedParameters);
    void report(OperationContext* opCtx, BSONObjBuilder* bob) const;

private:
    BSONObj _getParametersToReport(OperationContext* opCtx) const;

    ServerParameterSet* _clusterParameters;
    std::set<std::string> _reportedParameters;
};

}  // namespace mongo
