// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/topology/cluster_parameters/get_cluster_parameter_invocation.h"

#include "mongo/base/error_codes.h"
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
#include <string_view>

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
using namespace std::literals::string_view_literals;

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
        if (!requestedParameter->isEnabled(VersionContext::getDecoration(opCtx))) {
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
                                if (strParameterName == "*"sv) {
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
