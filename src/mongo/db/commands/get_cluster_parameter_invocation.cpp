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
#include "mongo/idl/cluster_server_parameter_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/s/grid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {

std::pair<std::vector<std::string>, std::vector<BSONObj>>
GetClusterParameterInvocation::retrieveRequestedParameters(OperationContext* opCtx,
                                                           const CmdBody& cmdBody) {
    ServerParameterSet* clusterParameters = ServerParameterSet::getClusterParameterSet();
    std::vector<std::string> parameterNames;
    std::vector<BSONObj> parameterValues;

    audit::logGetClusterParameter(opCtx->getClient(), cmdBody);

    // For each parameter, generate a BSON representation of it and retrieve its name.
    auto makeBSON = [&](ServerParameter* requestedParameter) {
        // Skip any disabled cluster parameters.
        if (requestedParameter->isEnabled()) {
            BSONObjBuilder bob;
            requestedParameter->append(opCtx, &bob, requestedParameter->name(), boost::none);
            parameterValues.push_back(bob.obj().getOwned());
            parameterNames.push_back(requestedParameter->name());
        }
    };

    stdx::visit(
        OverloadedVisitor{[&](const std::string& strParameterName) {
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
                                                        << "' is currently disabled",
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
                                  ServerParameter* sp =
                                      clusterParameters->get(requestedParameterName);
                                  uassert(ErrorCodes::BadValue,
                                          str::stream()
                                              << "Server parameter: '" << requestedParameterName
                                              << "' is currently disabled'",
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

    auto [parameterNames, parameterValues] = retrieveRequestedParameters(opCtx, cmdBody);

    LOGV2_DEBUG(6226100,
                2,
                "Retrieved parameter values for cluster server parameters",
                "parameterNames"_attr = parameterNames);

    return Reply(parameterValues);
}

GetClusterParameterInvocation::Reply GetClusterParameterInvocation::getDurableParameters(
    OperationContext* opCtx, const GetClusterParameter& request) {
    auto configServers = Grid::get(opCtx)->shardRegistry()->getConfigShard();

    // Create the query document such that all documents in config.clusterParmeters with _id
    // in the requested list of ServerParameters are returned.
    const CmdBody& cmdBody = request.getCommandParameter();
    ServerParameterSet* clusterParameters = ServerParameterSet::getClusterParameterSet();

    BSONObjBuilder queryDocBuilder;
    BSONObjBuilder inObjBuilder = queryDocBuilder.subobjStart("_id"_sd);
    BSONArrayBuilder parameterNameBuilder = inObjBuilder.subarrayStart("$in"_sd);

    auto [requestedParameterNames, parameterValues] = retrieveRequestedParameters(opCtx, cmdBody);

    for (const auto& parameterValue : parameterValues) {
        parameterNameBuilder.append(parameterValue["_id"_sd].String());
    }

    parameterNameBuilder.doneFast();
    inObjBuilder.doneFast();

    // Perform the majority read on the config server primary.
    BSONObj query = queryDocBuilder.obj();
    LOGV2_DEBUG(6226101, 2, "Querying config servers for cluster parameters", "query"_attr = query);
    auto findResponse = uassertStatusOK(configServers->exhaustiveFindOnConfig(
        opCtx,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        repl::ReadConcernLevel::kMajorityReadConcern,
        NamespaceString::makeClusterParametersNSS(request.getDbName().tenantId()),
        query,
        BSONObj(),
        boost::none));

    // Any parameters that are not included in the response don't have a cluster parameter
    // document yet, which means they still are using the default value.
    std::vector<BSONObj> retrievedParameters = std::move(findResponse.docs);
    if (retrievedParameters.size() < requestedParameterNames.size()) {
        std::vector<std::string> onDiskParameterNames;
        onDiskParameterNames.reserve(retrievedParameters.size());
        std::transform(
            retrievedParameters.begin(),
            retrievedParameters.end(),
            std::back_inserter(onDiskParameterNames),
            [&](const auto& onDiskParameter) { return onDiskParameter["_id"_sd].String(); });

        // Sort and find the set difference of the requested parameters and the parameters
        // returned.
        std::vector<std::string> defaultParameterNames;

        defaultParameterNames.reserve(requestedParameterNames.size() - onDiskParameterNames.size());

        std::sort(onDiskParameterNames.begin(), onDiskParameterNames.end());
        std::sort(requestedParameterNames.begin(), requestedParameterNames.end());
        std::set_difference(requestedParameterNames.begin(),
                            requestedParameterNames.end(),
                            onDiskParameterNames.begin(),
                            onDiskParameterNames.end(),
                            std::back_inserter(defaultParameterNames));

        for (const auto& defaultParameterName : defaultParameterNames) {
            auto defaultParameter = clusterParameters->get(defaultParameterName);
            BSONObjBuilder bob;
            defaultParameter->append(opCtx, &bob, defaultParameterName, boost::none);
            retrievedParameters.push_back(bob.obj());
        }
    }

    return Reply(retrievedParameters);
}

}  // namespace mongo
