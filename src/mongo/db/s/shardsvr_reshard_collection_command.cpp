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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/s/dist_lock_manager.h"
#include "mongo/db/s/reshard_collection_coordinator.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"

namespace mongo {
namespace {

class ShardsvrReshardCollectionCommand final
    : public TypedCommand<ShardsvrReshardCollectionCommand> {
public:
    using Request = ShardsvrReshardCollection;

    std::string help() const override {
        return "Internal command. Do not call directly. Reshards a collection.";
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

        void typedRun(OperationContext* opCtx) {
            uassertStatusOK(ShardingState::get(opCtx)->canAcceptShardedCommands());

            uassert(ErrorCodes::InvalidOptions,
                    str::stream() << Request::kCommandName
                                  << " must be called with majority writeConcern, got "
                                  << opCtx->getWriteConcern().wMode,
                    opCtx->getWriteConcern().wMode == WriteConcernOptions::kMajority);
            // (Generic FCV reference): To run this command and ensure the consistency of the
            // metadata we need to make sure we are on a stable state.
            uassert(
                ErrorCodes::CommandNotSupported,
                "Resharding is not supported for this version, please update the FCV to latest.",
                !serverGlobalParams.featureCompatibility.isUpgradingOrDowngrading());

            DistLockManager::ScopedDistLock dbDistLock(
                uassertStatusOK(DistLockManager::get(opCtx)->lock(
                    opCtx, ns().db(), "reshardCollection", DistLockManager::kDefaultLockTimeout)));
            DistLockManager::ScopedDistLock collDistLock(
                uassertStatusOK(DistLockManager::get(opCtx)->lock(
                    opCtx, ns().ns(), "reshardCollection", DistLockManager::kDefaultLockTimeout)));

            auto reshardCollectionCoordinator =
                std::make_shared<ReshardCollectionCoordinator>(opCtx, request());
            reshardCollectionCoordinator->run(opCtx).get(opCtx);
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
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                           ActionType::internal));
        }
    };

} shardsvrReshardCollectionCommand;

}  // namespace
}  // namespace mongo
