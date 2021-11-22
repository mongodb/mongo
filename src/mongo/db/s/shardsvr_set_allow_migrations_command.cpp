/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/commands.h"
#include "mongo/db/s/set_allow_migrations_coordinator.h"
#include "mongo/db/s/sharding_ddl_coordinator_service.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/logv2/log.h"

namespace mongo {
namespace {

class ShardsvrSetAllowMigrationsCommand final
    : public TypedCommand<ShardsvrSetAllowMigrationsCommand> {
public:
    using Request = ShardsvrSetAllowMigrations;

    std::string help() const override {
        return "Internal command. Do not call directly. Enable/disable migrations in a collection.";
    }

    bool adminOnly() const override {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            uassertStatusOK(ShardingState::get(opCtx)->canAcceptShardedCommands());

            opCtx->setAlwaysInterruptAtStepDownOrUp();

            uassert(ErrorCodes::InvalidOptions,
                    str::stream() << Request::kCommandName
                                  << " must be called with majority writeConcern, got "
                                  << request().toBSON(BSONObj()),
                    opCtx->getWriteConcern().wMode == WriteConcernOptions::kMajority);

            SetAllowMigrationsRequest setAllowMigationsCmdRequest =
                request().getSetAllowMigrationsRequest();
            auto nss = ns();
            auto coordinatorDoc = SetAllowMigrationsCoordinatorDocument();
            coordinatorDoc.setShardingDDLCoordinatorMetadata(
                {{std::move(nss), DDLCoordinatorTypeEnum::kSetAllowMigrations}});
            coordinatorDoc.setSetAllowMigrationsRequest(std::move(setAllowMigationsCmdRequest));

            auto service = ShardingDDLCoordinatorService::getService(opCtx);
            auto setAllowMigrationsCoordinator =
                checked_pointer_cast<SetAllowMigrationsCoordinator>(
                    service->getOrCreateInstance(opCtx, coordinatorDoc.toBSON()));
            setAllowMigrationsCoordinator->getCompletionFuture().get(opCtx);
        }

    private:
        NamespaceString ns() const override {
            return request().getNamespace();
        }

        bool supportsWriteConcern() const override {
            return true;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                           ActionType::internal));
        }
    };

} shardsvrSetAllowMigrationsCommand;

}  // namespace
}  // namespace mongo
