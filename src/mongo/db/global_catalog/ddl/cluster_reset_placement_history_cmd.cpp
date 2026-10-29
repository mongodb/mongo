// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/ddl/placement_history_commands_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <string>

#include <boost/move/utility_core.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

class ClusterResetPlacementHistoryCommand final
    : public TypedCommand<ClusterResetPlacementHistoryCommand> {
public:
    using Request = ClusterResetPlacementHistory;

    std::string help() const override {
        return "Invoke adminCommand({resetPlacementHistory: 1}) to reset the log of namespace "
               "placement changes. Meant to be used only under the guidance of tech support.";
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
            ConfigsvrResetPlacementHistory configsvrRequest;
            configsvrRequest.setDbName(DatabaseName::kAdmin);
            configsvrRequest.setWriteConcern(defaultMajorityWriteConcernDoNotUse());

            auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
            const auto commandResponse =
                uassertStatusOK(configShard->runCommandWithIndefiniteRetries(
                    opCtx,
                    ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                    DatabaseName::kAdmin,
                    configsvrRequest.toBSON(),
                    Shard::RetryPolicy::kIdempotent));
            uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(commandResponse));
        }

    private:
        NamespaceString ns() const override {
            return NamespaceString(request().getDbName());
        }

        bool supportsWriteConcern() const override {
            return true;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forExactNamespace(ns()),
                                                           ActionType::moveChunk));
        }
    };
};
MONGO_REGISTER_COMMAND(ClusterResetPlacementHistoryCommand).forRouter();

}  // namespace
}  // namespace mongo
