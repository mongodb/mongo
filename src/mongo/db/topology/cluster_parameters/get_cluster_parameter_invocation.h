// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/topology/cluster_parameters/cluster_server_parameter_cmds_gen.h"
#include "mongo/util/modules.h"

#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {

class GetClusterParameterInvocation {
public:
    using Request = GetClusterParameter;
    using Reply = GetClusterParameter::Reply;
    using Map = ServerParameterSet::Map;
    using CmdBody = std::variant<std::string, std::vector<std::string>>;

    GetClusterParameterInvocation() = default;

    // Retrieves in-memory parameters.
    Reply getCachedParameters(OperationContext* opCtx, const GetClusterParameter& request);

private:
    // Parses the command body and retrieves the BSON representation and names of the requested
    // cluster parameters for the given tenant.
    std::pair<std::vector<std::string>, std::vector<BSONObj>> retrieveRequestedParameters(
        OperationContext* opCtx,
        const CmdBody& cmdBody,
        bool shouldOmitInFTDC,
        const boost::optional<TenantId>& tenantId,
        bool excludeClusterParameterTime);
};

}  // namespace mongo
