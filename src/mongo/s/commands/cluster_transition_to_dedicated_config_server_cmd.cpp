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

#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog_shard_feature_flag_gen.h"
#include "mongo/db/commands.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/transition_to_dedicated_config_server_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {

const ReadPreferenceSetting kPrimaryOnlyReadPreference{ReadPreference::PrimaryOnly};

class TransitionToDedicatedConfigServerCmd : public BasicCommand {
public:
    TransitionToDedicatedConfigServerCmd() : BasicCommand("transitionToDedicatedConfigServer") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    std::string help() const override {
        return "transition to dedicated config server";
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName&,
                                 const BSONObj&) const override {
        auto* as = AuthorizationSession::get(opCtx->getClient());
        if (!as->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                  ActionType::transitionToDedicatedConfigServer)) {
            return {ErrorCodes::Unauthorized, "unauthorized"};
        }

        return Status::OK();
    }

    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        // (Ignore FCV check): TODO(SERVER-75389): add why FCV is ignored here.
        uassert(7368401,
                "The transition to catalog shard feature is disabled",
                gFeatureFlagTransitionToCatalogShard.isEnabledAndIgnoreFCVUnsafe());
        uassert(7467200,
                "The catalog shard feature is disabled",
                gFeatureFlagCatalogShard.isEnabled(serverGlobalParams.featureCompatibility));

        auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

        ConfigsvrTransitionToDedicatedConfig transitionToDedicatedConfigServer;
        transitionToDedicatedConfigServer.setDbName({"admin"});

        // Force a reload of this node's shard list cache at the end of this command.
        auto cmdResponseWithStatus = configShard->runCommandWithFixedRetryAttempts(
            opCtx,
            kPrimaryOnlyReadPreference,
            "admin",
            CommandHelpers::appendMajorityWriteConcern(
                CommandHelpers::appendGenericCommandArgs(
                    cmdObj, transitionToDedicatedConfigServer.toBSON({})),
                opCtx->getWriteConcern()),
            Shard::RetryPolicy::kIdempotent);

        Grid::get(opCtx)->shardRegistry()->reload(opCtx);

        uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(cmdResponseWithStatus));
        CommandHelpers::filterCommandReplyForPassthrough(cmdResponseWithStatus.getValue().response,
                                                         &result);

        return true;
    }

} transitionToDedicatedConfigServerCmd;

}  // namespace
}  // namespace mongo
