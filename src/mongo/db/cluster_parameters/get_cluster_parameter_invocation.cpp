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


#include "mongo/db/cluster_parameters/get_cluster_parameter_invocation.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/audit.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/query_settings/query_settings_service.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <map>

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {

std::pair<std::vector<std::string>, std::vector<BSONObj>>
GetClusterParameterInvocation::retrieveRequestedParameters(
    OperationContext* opCtx,
    const CmdBody& cmdBody,
    bool shouldOmitInFTDC,
    const boost::optional<TenantId>& tenantId,
    bool excludeClusterParameterTime) {
    ServerParameterSet* clusterParameters = ServerParameterSet::getClusterParameterSet();
    std::vector<std::string> parameterNames;
    std::vector<BSONObj> parameterValues;

    audit::logGetClusterParameter(opCtx->getClient(), cmdBody);

    // For each parameter, generate a BSON representation of it and retrieve its name.
    auto makeBSON = [&](ServerParameter* requestedParameter, bool skipOnError) {
        if (!requestedParameter->isEnabled()) {
            uassert(ErrorCodes::BadValue,
                    str::stream() << "Server parameter: '" << requestedParameter->name()
                                  << "' is disabled",
                    skipOnError);
            return;
        }

        // If the command is invoked with shouldOmitInFTDC, then any parameter that has that
        // flag set should be omitted.
        if (shouldOmitInFTDC && requestedParameter->isOmittedInFTDC()) {
            return;
        }

        // The persistent query settings are stored in a cluster parameter, however, since this is
        // an implementation detail, we don't want to expose it to our users.
        auto querySettingsClusterParameterName =
            query_settings::QuerySettingsService::getQuerySettingsClusterParameterName();
        if (requestedParameter->name() == querySettingsClusterParameterName) {
            uassert(ErrorCodes::NoSuchKey,
                    str::stream() << "Unknown server parameter: " << requestedParameter->name(),
                    skipOnError);
            return;
        }

        // Scans do not generate warnings for deprecated server parameters.
        if (!skipOnError) {
            requestedParameter->warnIfDeprecated("getClusterParameter");
        }

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
    };

    visit(OverloadedVisitor{[&](const std::string& strParameterName) {
                                if (strParameterName == "*"_sd) {
                                    // Retrieve all cluster parameter values.
                                    const Map& clusterParameterMap = clusterParameters->getMap();
                                    parameterValues.reserve(clusterParameterMap.size());
                                    parameterNames.reserve(clusterParameterMap.size());
                                    for (const auto& param : clusterParameterMap) {
                                        makeBSON(param.second.get(), true);
                                    }
                                } else {
                                    // Any other string must correspond to a single parameter name.
                                    // Return an error if a disabled cluster parameter is explicitly
                                    // requested.
                                    makeBSON(clusterParameters->get(strParameterName), false);
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
                                    makeBSON(clusterParameters->get(requestedParameterName), false);
                                }
                            }},
          cmdBody);

    return {std::move(parameterNames), std::move(parameterValues)};
}

GetClusterParameterInvocation::Reply GetClusterParameterInvocation::getCachedParameters(
    OperationContext* opCtx, const GetClusterParameter& request) {
    const CmdBody& cmdBody = request.getCommandParameter();
    bool shouldOmitInFTDC = request.getOmitInFTDC();

    auto* repl = repl::ReplicationCoordinator::get(opCtx);
    bool isStandalone = repl && !repl->getSettings().isReplSet() &&
        serverGlobalParams.clusterRole.has(ClusterRole::None);

    auto [parameterNames, parameterValues] = retrieveRequestedParameters(
        opCtx, cmdBody, shouldOmitInFTDC, request.getDbName().tenantId(), isStandalone);

    LOGV2_DEBUG(6226100,
                2,
                "Retrieved parameter values for cluster server parameters",
                "parameterNames"_attr = parameterNames,
                "tenantId"_attr = request.getDbName().tenantId());

    return Reply(parameterValues);
}

}  // namespace mongo
