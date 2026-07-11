// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/s/migration_blocking_operation/multi_update_coordinator.h"
#include "mongo/db/s/migration_blocking_operation/multi_update_coordinator_gen.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/s/request_types/coordinate_multi_update_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {

class ShardsvrCoordinateMultiUpdateCommand final
    : public TypedCommand<ShardsvrCoordinateMultiUpdateCommand> {
public:
    using Request = ShardsvrCoordinateMultiUpdate;
    using Response = ShardsvrCoordinateMultiUpdateResponse;

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command. Do not call directly. Coordinates an updateMany or deleteMany to "
               "pause migrations, perform the updates, then resume migrations."
               "Will only be called when cluster parameter pauseMigrationsDuringMultiUpdates is "
               "true.";
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Response typedRun(OperationContext* opCtx) {
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();
            ShardingState::get(opCtx)->assertCanAcceptShardedCommands();

            MultiUpdateCoordinatorMetadata metadata;
            metadata.setId(request().getUuid());
            metadata.setDatabaseVersion(request().getDatabaseVersion());
            metadata.setUpdateCommand(request().getCommand());
            metadata.setNss(ns());

            // Determine if the command is an upsert. Each coordinated multi write is sent
            // individually either as an update or a bulk write.
            if (metadata.getUpdateCommand().hasField("updates")) {
                auto updates = request().getCommand().getField("updates").Array();
                tassert(11057400,
                        "Expected a single operation when coordinating multi update",
                        updates.size() == 1);
                metadata.setIsUpsert(updates.front().Obj().getBoolField("upsert"));
            } else if (metadata.getUpdateCommand().hasField("bulkWrite")) {
                auto ops = request().getCommand().getField("ops").Array();
                tassert(11057401,
                        "Expected a single operation when coordinating multi update",
                        ops.size() == 1);
                metadata.setIsUpsert(ops.front().Obj().getBoolField("upsert"));
            }

            MultiUpdateCoordinatorDocument coordinatorDoc;
            coordinatorDoc.setMetadata(metadata);

            auto registry = repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext());
            auto service =
                registry->lookupServiceByName(MultiUpdateCoordinatorService::kServiceName);
            auto instance = MultiUpdateCoordinatorInstance::getOrCreate(
                opCtx, service, coordinatorDoc.toBSON());

            auto response = Response();
            response.setResult(instance->getCompletionFuture().get(opCtx));
            return response;
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
};
MONGO_REGISTER_COMMAND(ShardsvrCoordinateMultiUpdateCommand).forShard();

}  // namespace
}  // namespace mongo
