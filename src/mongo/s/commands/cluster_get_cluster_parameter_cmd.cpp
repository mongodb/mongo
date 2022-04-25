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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/cluster_server_parameter_cmds_gen.h"
#include "mongo/idl/cluster_server_parameter_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"

namespace mongo {
namespace {

class GetClusterParameterCmd final : public TypedCommand<GetClusterParameterCmd> {
public:
    using Request = GetClusterParameter;
    using Reply = GetClusterParameter::Reply;
    using Map = ServerParameterSet::Map;

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return true;
    }

    std::string help() const override {
        return "Get majority-written cluster parameter value(s) from the config servers";
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Reply typedRun(OperationContext* opCtx) {
            uassert(
                ErrorCodes::IllegalOperation,
                "featureFlagClusterWideConfig not enabled",
                gFeatureFlagClusterWideConfig.isEnabled(serverGlobalParams.featureCompatibility));

            // For now, the mongos implementation retrieves the names of the requested cluster
            // server parameters and queries them from the config.clusterParameters namespace on
            // the config servers. This may change after SERVER-62264, when cluster server
            // parameters will be cached on mongoses as well.
            auto configServers = Grid::get(opCtx)->shardRegistry()->getConfigShard();

            // Create the query document such that all documents in config.clusterParmeters with _id
            // in the requested list of ServerParameters are returned.
            const stdx::variant<std::string, std::vector<std::string>>& cmdBody =
                request().getCommandParameter();
            ServerParameterSet* clusterParameters = ServerParameterSet::getClusterParameterSet();

            std::vector<std::string> requestedParameterNames;
            BSONObjBuilder queryDocBuilder;
            BSONObjBuilder inObjBuilder = queryDocBuilder.subobjStart("_id"_sd);
            BSONArrayBuilder parameterNameBuilder = inObjBuilder.subarrayStart("$in"_sd);

            stdx::visit(
                visit_helper::Overloaded{
                    [&](const std::string& strParameterName) {
                        if (strParameterName == "*"_sd) {
                            // Append all cluster parameter names.
                            Map clusterParameterMap = clusterParameters->getMap();
                            requestedParameterNames.reserve(clusterParameterMap.size());
                            for (const auto& param : clusterParameterMap) {
                                // Skip any disabled test parameters.
                                if (param.second->isEnabled()) {
                                    parameterNameBuilder.append(param.first);
                                    requestedParameterNames.push_back(param.first);
                                }
                            }
                        } else {
                            // Return an error if a disabled cluster parameter is explicitly
                            // requested.
                            uassert(ErrorCodes::BadValue,
                                    str::stream() << "Server parameter: '" << strParameterName
                                                  << "' is currently disabled'",
                                    clusterParameters->get(strParameterName)->isEnabled());
                            parameterNameBuilder.append(strParameterName);
                            requestedParameterNames.push_back(strParameterName);
                        }
                    },
                    [&](const std::vector<std::string>& listParameterNames) {
                        uassert(ErrorCodes::BadValue,
                                "Must supply at least one cluster server parameter name to "
                                "getClusterParameter",
                                listParameterNames.size() > 0);
                        requestedParameterNames.reserve(listParameterNames.size());
                        for (const auto& requestedParameterName : listParameterNames) {
                            // Return an error if a disabled cluster parameter is explicitly
                            // requested.
                            uassert(ErrorCodes::BadValue,
                                    str::stream() << "Server parameter: '" << requestedParameterName
                                                  << "' is currently disabled'",
                                    clusterParameters->get(requestedParameterName)->isEnabled());
                            parameterNameBuilder.append(requestedParameterName);
                            requestedParameterNames.push_back(requestedParameterName);
                        }
                    }},
                cmdBody);

            parameterNameBuilder.doneFast();
            inObjBuilder.doneFast();

            // Perform the majority read on the config server primary.
            BSONObj query = queryDocBuilder.obj();
            LOGV2(6226101, "Querying config servers for cluster parameters", "query"_attr = query);
            auto findResponse = uassertStatusOK(configServers->exhaustiveFindOnConfig(
                opCtx,
                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                repl::ReadConcernLevel::kMajorityReadConcern,
                NamespaceString::kClusterParametersNamespace,
                query,
                BSONObj(),
                boost::none));

            // Any parameters that are not included in the response don't have a cluster parameter
            // document yet, which means they still are using the default value.
            std::vector<BSONObj> retrievedParameters = std::move(findResponse.docs);
            if (retrievedParameters.size() < requestedParameterNames.size()) {
                std::vector<std::string> onDiskParameterNames;
                onDiskParameterNames.reserve(retrievedParameters.size());
                std::transform(retrievedParameters.begin(),
                               retrievedParameters.end(),
                               std::back_inserter(onDiskParameterNames),
                               [&](const auto& onDiskParameter) {
                                   return onDiskParameter["_id"_sd].String();
                               });

                // Sort and find the set difference of the requested parameters and the parameters
                // returned.
                std::vector<std::string> defaultParameterNames;

                defaultParameterNames.reserve(requestedParameterNames.size() -
                                              onDiskParameterNames.size());

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
                    defaultParameter->append(opCtx, bob, defaultParameterName);
                    retrievedParameters.push_back(bob.obj());
                }
            }

            return Reply(retrievedParameters);
        }

    private:
        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const final {
            auto* authzSession = AuthorizationSession::get(opCtx->getClient());
            uassert(ErrorCodes::Unauthorized,
                    "Not authorized to retrieve cluster parameters",
                    authzSession->isAuthorizedForPrivilege(Privilege{
                        ResourcePattern::forClusterResource(), ActionType::getClusterParameter}));
        }

        NamespaceString ns() const override {
            return NamespaceString(request().getDbName(), "");
        }
    };
} getClusterParameterCmd;

}  // namespace
}  // namespace mongo
