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

#include <memory>
#include <string>

#include "mongo/base/error_codes.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/catalog_shard_feature_flag_gen.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/transition_from_dedicated_config_server_gen.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {

const ReadPreferenceSetting kPrimaryOnlyReadPreference{ReadPreference::PrimaryOnly};

class TransitionFromDedicatedConfigServerCommand
    : public TypedCommand<TransitionFromDedicatedConfigServerCommand> {
public:
    using Request = TransitionFromDedicatedConfigServer;

    std::string help() const override {
        return "transition from dedicated config server to config shard";
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
            // (Ignore FCV check): TODO(SERVER-75389): add why FCV is ignored here.
            uassert(8454804,
                    "The transition to config shard feature is disabled",
                    gFeatureFlagTransitionToCatalogShard.isEnabledAndIgnoreFCVUnsafe());

            ConfigsvrTransitionFromDedicatedConfigServer cmdToSend;
            cmdToSend.setDbName(DatabaseName::kAdmin);
            generic_argument_util::setMajorityWriteConcern(cmdToSend, &opCtx->getWriteConcern());

            auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

            // Force a reload of this node's shard list cache at the end of this command.
            auto cmdResponseWithStatus =
                configShard->runCommandWithFixedRetryAttempts(opCtx,
                                                              kPrimaryOnlyReadPreference,
                                                              DatabaseName::kAdmin,
                                                              cmdToSend.toBSON(),
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
                        ->isAuthorizedForActionsOnResource(
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::transitionFromDedicatedConfigServer));
        }
    };
};
MONGO_REGISTER_COMMAND(TransitionFromDedicatedConfigServerCommand).forRouter();

}  // namespace
}  // namespace mongo
