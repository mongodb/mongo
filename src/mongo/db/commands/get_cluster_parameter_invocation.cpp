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

#include "mongo/db/audit.h"
#include "mongo/db/commands/get_cluster_parameter_invocation.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/idl/cluster_server_parameter_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/s/grid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {

std::pair<std::vector<std::string>, std::vector<BSONObj>>
GetClusterParameterInvocation::retrieveRequestedParameters(
    OperationContext* opCtx,
    const CmdBody& cmdBody,
    const boost::optional<TenantId>& tenantId,
    bool excludeClusterParameterTime) {
    ServerParameterSet* clusterParameters = ServerParameterSet::getClusterParameterSet();
    std::vector<std::string> parameterNames;
    std::vector<BSONObj> parameterValues;

    audit::logGetClusterParameter(opCtx->getClient(), cmdBody);

    // For each parameter, generate a BSON representation of it and retrieve its name.
    auto makeBSON = [&](ServerParameter* requestedParameter) {
        // Skip any disabled cluster parameters.
        if (requestedParameter->isEnabled()) {
            BSONObjBuilder bob;
            requestedParameter->append(opCtx, &bob, requestedParameter->name(), tenantId);
            auto paramObj = bob.obj().getOwned();
            if (excludeClusterParameterTime) {
                parameterValues.push_back(
                    paramObj.filterFieldsUndotted(BSON("clusterParameterTime" << true), false));
            } else {
                parameterValues.push_back(paramObj);
            }
            parameterNames.push_back(requestedParameter->name());
        }
    };

    stdx::visit(OverloadedVisitor{
                    [&](const std::string& strParameterName) {
                        if (strParameterName == "*"_sd) {
                            // Retrieve all cluster parameter values.
                            Map clusterParameterMap = clusterParameters->getMap();
                            parameterValues.reserve(clusterParameterMap.size());
                            parameterNames.reserve(clusterParameterMap.size());
                            for (const auto& param : clusterParameterMap) {
                                makeBSON(param.second);
                            }
                        } else {
                            // Any other string must correspond to a single parameter name.
                            // Return an error if a disabled cluster parameter is explicitly
                            // requested.
                            ServerParameter* sp = clusterParameters->get(strParameterName);
                            uassert(ErrorCodes::BadValue,
                                    str::stream() << "Server parameter: '" << strParameterName
                                                  << "' is disabled",
                                    sp->isEnabled());
                            makeBSON(sp);
                        }
                    },
                    [&](const std::vector<std::string>& listParameterNames) {
                        uassert(ErrorCodes::BadValue,
                                "Must supply at least one cluster server parameter name to "
                                "getClusterParameter",
                                listParameterNames.size() > 0);
                        parameterValues.reserve(listParameterNames.size());
                        parameterNames.reserve(listParameterNames.size());
                        for (const auto& requestedParameterName : listParameterNames) {
                            ServerParameter* sp = clusterParameters->get(requestedParameterName);
                            uassert(ErrorCodes::BadValue,
                                    str::stream() << "Server parameter: '" << requestedParameterName
                                                  << "' is disabled'",
                                    sp->isEnabled());
                            makeBSON(sp);
                        }
                    }},
                cmdBody);

    return {std::move(parameterNames), std::move(parameterValues)};
}

GetClusterParameterInvocation::Reply GetClusterParameterInvocation::getCachedParameters(
    OperationContext* opCtx, const GetClusterParameter& request) {
    const CmdBody& cmdBody = request.getCommandParameter();

    auto* repl = repl::ReplicationCoordinator::get(opCtx);
    bool isStandalone = repl &&
        repl->getReplicationMode() == repl::ReplicationCoordinator::modeNone &&
        serverGlobalParams.clusterRole.has(ClusterRole::None);

    auto [parameterNames, parameterValues] =
        retrieveRequestedParameters(opCtx, cmdBody, request.getDbName().tenantId(), isStandalone);

    LOGV2_DEBUG(6226100,
                2,
                "Retrieved parameter values for cluster server parameters",
                "parameterNames"_attr = parameterNames,
                "tenantId"_attr = request.getDbName().tenantId());

    return Reply(parameterValues);
}

}  // namespace mongo
