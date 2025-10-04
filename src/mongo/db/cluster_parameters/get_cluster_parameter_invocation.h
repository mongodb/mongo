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

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/cluster_parameters/cluster_server_parameter_cmds_gen.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/tenant_id.h"

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
