// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/audit.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

class ConfigsvrCommitRefineCollectionShardKeyCommand final
    : public TypedCommand<ConfigsvrCommitRefineCollectionShardKeyCommand> {
public:
    using Request = ConfigsvrCommitRefineCollectionShardKey;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            uassert(ErrorCodes::IllegalOperation,
                    "_configsvrCommitRefineCollectionShardKey can only be run on config servers",
                    serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));
            CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                          opCtx->getWriteConcern());

            // Set the operation context read concern level to local for reads into the config
            // database.
            repl::ReadConcernArgs::get(opCtx) =
                repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);
            // Set the write concern on the opCtx to majority so by the time the transaction
            // finishes the writes will be majority written.
            opCtx->setWriteConcern(opCtx->getWriteConcern());

            audit::logRefineCollectionShardKey(
                opCtx->getClient(), ns(), request().getKey().toBSON());

            ShardingCatalogManager::get(opCtx)->commitRefineCollectionShardKey(
                opCtx,
                ns(),
                ShardKeyPattern(request().getKey()),
                request().getNewTimestamp(),
                request().getNewEpoch(),
                request().getOldTimestamp());
        }

    private:
        NamespaceString ns() const override {
            return request().getCommandParameter();
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
                            ActionType::internal));
        }
    };

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command, which is exported by the sharding config server. Do not call "
               "directly. Commits the new shard key on the specified collection.";
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }
};
MONGO_REGISTER_COMMAND(ConfigsvrCommitRefineCollectionShardKeyCommand).forShard();
}  // namespace
}  // namespace mongo
