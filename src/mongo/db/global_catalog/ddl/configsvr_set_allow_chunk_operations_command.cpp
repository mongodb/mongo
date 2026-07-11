// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/error_codes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_util.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <string>

#include <boost/move/utility_core.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

class ConfigsvrSetAllowChunkOperationsCommand final
    : public TypedCommand<ConfigsvrSetAllowChunkOperationsCommand> {
public:
    using Request = ConfigsvrSetAllowChunkOperations;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            const NamespaceString& nss = ns();

            uassert(ErrorCodes::IllegalOperation,
                    "_configsvrSetAllowChunkOperations can only be run on config servers",
                    serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));
            CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                          opCtx->getWriteConcern());

            uassert(12120911,
                    "_configsvrSetAllowChunkOperations should only run with AuthoritativeShardsDDL "
                    "enabled",
                    sharding_ddl_util::getGrantedAuthoritativeMetadataAccessLevel(
                        VersionContext::getDecoration(opCtx),
                        serverGlobalParams.featureCompatibility.acquireFCVSnapshot()) !=
                        AuthoritativeMetadataAccessLevelEnum::kNone);

            {
                // Use ACR to have a thread holding the session while we do the metadata updates so
                // we can serialize concurrent requests to setAllowChunkOperations (i.e. a stepdown
                // happens and the new primary sends a setAllowChunkOperations with the same
                // sessionId). We could think about weakening the serialization guarantee in the
                // future because the replay protection comes from the oplog write with a specific
                // txnNumber. Using ACR also prevents having deadlocks with the shutdown thread
                // because the cancellation of the new operation context is linked to the parent
                // one.
                auto newClient =
                    opCtx->getServiceContext()->getService()->makeClient("SetAllowChunkOperations");
                AlternativeClientRegion acr(newClient);
                auto executor =
                    Grid::get(opCtx->getServiceContext())->getExecutorPool()->getFixedExecutor();
                auto newOpCtxPtr = CancelableOperationContext(
                    cc().makeOperationContext(), opCtx->getCancellationToken(), executor);

                AuthorizationSession::get(newOpCtxPtr.get()->getClient())
                    ->grantInternalAuthorization();
                newOpCtxPtr->setWriteConcern(opCtx->getWriteConcern());

                // Set the operation context read concern level to local for reads into the config
                // database.
                repl::ReadConcernArgs::get(newOpCtxPtr.get()) =
                    repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);

                const auto allowChunkOperations = request().getAllowChunkOperations();
                const auto& collectionUUID = request().getCollectionUUID();

                ShardingCatalogManager::get(newOpCtxPtr.get())
                    ->setAllowChunkOperations(
                        newOpCtxPtr.get(), nss, collectionUUID, allowChunkOperations);
            }

            // Since no write happened on this txnNumber, we need to make a dummy write to
            // protect against older requests with old txnNumbers.
            DBDirectClient client(opCtx);
            client.update(NamespaceString::kServerConfigurationNamespace,
                          BSON("_id" << "setAllowChunkOperationsStats"),
                          BSON("$inc" << BSON("count" << 1)),
                          true /* upsert */,
                          false /* multi */);
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
               "directly. Sets the allowChunkOperations flag on the specified collection.";
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool supportsRetryableWrite() const final {
        return true;
    }
};
MONGO_REGISTER_COMMAND(ConfigsvrSetAllowChunkOperationsCommand).forShard();

}  // namespace
}  // namespace mongo
