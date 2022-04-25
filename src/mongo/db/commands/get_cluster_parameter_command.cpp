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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/cluster_server_parameter_cmds_gen.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/idl/cluster_server_parameter_gen.h"
#include "mongo/logv2/log.h"

namespace mongo {

namespace {

class GetClusterParameterCommand final : public TypedCommand<GetClusterParameterCommand> {
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
        return "Get in-memory cluster parameter value(s) from this node";
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Reply typedRun(OperationContext* opCtx) {
            uassert(
                ErrorCodes::IllegalOperation,
                "featureFlagClusterWideConfig not enabled",
                gFeatureFlagClusterWideConfig.isEnabled(serverGlobalParams.featureCompatibility));

            // TODO SERVER-65249: This will eventually be made specific to the parameter being set
            // so that some parameters will be able to use getClusterParameter even on standalones.
            uassert(ErrorCodes::IllegalOperation,
                    str::stream() << Request::kCommandName << " cannot be run on standalones",
                    repl::ReplicationCoordinator::get(opCtx)->getReplicationMode() !=
                        repl::ReplicationCoordinator::modeNone);

            const stdx::variant<std::string, std::vector<std::string>>& cmdBody =
                request().getCommandParameter();
            ServerParameterSet* clusterParameters = ServerParameterSet::getClusterParameterSet();
            std::vector<BSONObj> parameterValues;
            std::vector<std::string> parameterNames;

            // For each parameter, generate a BSON representation of it and retrieve its name.
            auto makeBSON = [&](ServerParameter* requestedParameter) {
                // Skip any disabled cluster parameters.
                if (requestedParameter->isEnabled()) {
                    BSONObjBuilder bob;
                    requestedParameter->append(opCtx, bob, requestedParameter->name());
                    parameterValues.push_back(bob.obj());
                    parameterNames.push_back(requestedParameter->name());
                }
            };

            stdx::visit(
                visit_helper::Overloaded{
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
                            // Any other string must correspond to a single parameter name. Return
                            // an error if a disabled cluster parameter is explicitly requested.
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
                            ServerParameter* sp = clusterParameters->get(requestedParameterName);
                            uassert(ErrorCodes::BadValue,
                                    str::stream() << "Server parameter: '" << requestedParameterName
                                                  << "' is currently disabled'",
                                    sp->isEnabled());
                            makeBSON(sp);
                        }
                    }},
                cmdBody);

            LOGV2_DEBUG(6226100,
                        2,
                        "Retrieved parameter values for cluster server parameters",
                        "parameterNames"_attr = parameterNames);

            return Reply(parameterValues);
        }

    private:
        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            auto* authzSession = AuthorizationSession::get(opCtx->getClient());
            uassert(ErrorCodes::Unauthorized,
                    "Not authorized to retrieve cluster server parameters",
                    authzSession->isAuthorizedForPrivilege(Privilege{
                        ResourcePattern::forClusterResource(), ActionType::getClusterParameter}));
        }

        NamespaceString ns() const override {
            return NamespaceString(request().getDbName(), "");
        }
    };
} getClusterParameterCommand;

}  // namespace
}  // namespace mongo
