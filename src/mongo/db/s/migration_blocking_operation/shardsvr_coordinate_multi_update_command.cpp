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

            if (metadata.getUpdateCommand().hasField("updates")) {
                auto updates = request().getCommand().getField("updates").Array();
                // Each coordinated multi write in a bulk write is sent individually.
                invariant(updates.size() == 1);
                metadata.setIsUpsert(updates.front().Obj().getBoolField("upsert"));
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
