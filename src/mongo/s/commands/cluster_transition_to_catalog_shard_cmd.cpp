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

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog_shard_feature_flag_gen.h"
#include "mongo/db/commands.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/transition_to_catalog_shard_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {

const ReadPreferenceSetting kPrimaryOnlyReadPreference{ReadPreference::PrimaryOnly};

class TransitionToCatalogShardCommand : public TypedCommand<TransitionToCatalogShardCommand> {
public:
    using Request = TransitionToCatalogShard;

    std::string help() const override {
        return "transition from dedicated config server to catalog shard";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            uassert(7467201,
                    "The catalog shard feature is disabled",
                    gFeatureFlagCatalogShard.isEnabled(serverGlobalParams.featureCompatibility));

            ConfigsvrTransitionToCatalogShard cmdToSend;
            cmdToSend.setDbName({"admin"});

            auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

            // Force a reload of this node's shard list cache at the end of this command.
            auto cmdResponseWithStatus = configShard->runCommandWithFixedRetryAttempts(
                opCtx,
                kPrimaryOnlyReadPreference,
                "admin",
                CommandHelpers::appendMajorityWriteConcern(cmdToSend.toBSON({}),
                                                           opCtx->getWriteConcern()),
                Shard::RetryPolicy::kIdempotent);

            Grid::get(opCtx)->shardRegistry()->reload(opCtx);

            uassertStatusOK(cmdResponseWithStatus);
            uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(cmdResponseWithStatus));
        }

    private:
        NamespaceString ns() const override {
            return {};
        }

        bool supportsWriteConcern() const override {
            return true;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                           ActionType::transitionToCatalogShard));
        }
    };
};

MONGO_REGISTER_FEATURE_FLAGGED_COMMAND(TransitionToCatalogShardCommand,
                                       gFeatureFlagTransitionToCatalogShard);

}  // namespace
}  // namespace mongo
