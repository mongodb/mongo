// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/error_codes.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/topology/user_write_block/set_user_write_block_mode_gen.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kAccessControl


namespace mongo {
namespace {
class SetUserWriteBlockModeCommand final : public TypedCommand<SetUserWriteBlockModeCommand> {
public:
    using Request = SetUserWriteBlockMode;

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }
    bool adminOnly() const override {
        return false;
    }

    std::string help() const override {
        return "Set whether user write blocking is enabled";
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            ConfigsvrSetUserWriteBlockMode configsvrSetUserWriteBlockModeCmd;
            configsvrSetUserWriteBlockModeCmd.setDbName(DatabaseName::kAdmin);
            SetUserWriteBlockModeRequest setUserWriteBlockModeRequest(
                request().getSetUserWriteBlockModeRequest());
            configsvrSetUserWriteBlockModeCmd.setSetUserWriteBlockModeRequest(
                setUserWriteBlockModeRequest);
            generic_argument_util::setMajorityWriteConcern(configsvrSetUserWriteBlockModeCmd,
                                                           &opCtx->getWriteConcern());

            auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
            auto cmdResponse = uassertStatusOK(
                configShard->runCommand(opCtx,
                                        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                        DatabaseName::kAdmin,
                                        configsvrSetUserWriteBlockModeCmd.toBSON(),
                                        Shard::RetryPolicy::kIdempotent));
            uassertStatusOK(cmdResponse.commandStatus);
            uassertStatusOK(cmdResponse.writeConcernStatus);
        }

    private:
        bool supportsWriteConcern() const override {
            return false;
        }
        NamespaceString ns() const override {
            return NamespaceString::kEmpty;
        }
        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForPrivilege(Privilege{
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::setUserWriteBlockMode}));
        }
    };
};
MONGO_REGISTER_COMMAND(SetUserWriteBlockModeCommand).forRouter();
}  // namespace
}  // namespace mongo
