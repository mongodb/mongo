/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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
#include "mongo/db/catalog/capped_utils.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/s/convert_to_capped_coordinator.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"
#include "mongo/s/sharding_feature_flags_gen.h"
#include "mongo/s/sharding_state.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

class ShardsvrConvertToCappedCommand final : public TypedCommand<ShardsvrConvertToCappedCommand> {
public:
    using Request = ShardsvrConvertToCapped;

    std::string help() const override {
        return "Internal command, do not invoke directly. Converts a collection to capped.";
    }

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return Command::AllowedOnSecondary::kNever;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            ShardingState::get(opCtx)->assertCanAcceptShardedCommands();
            CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                          opCtx->getWriteConcern());
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            // Since this operation is not directly writing locally we need to force its db
            // profile level increase in order to be logged in "<db>.system.profile"
            CurOp::get(opCtx)->raiseDbProfileLevel(
                CollectionCatalog::get(opCtx)->getDatabaseProfileLevel(ns().dbName()));

            const auto& nss = ns();

            {
                // TODO SERVER-87119 remove this scope once v8.0 branches out
                // Unsafe best effort check needed to prevent calling convertToCapped on sharded
                // collections when mustUseCoordinator=false
                const auto cri = uassertStatusOK(
                    Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfoWithRefresh(opCtx,
                                                                                          nss));
                uassert(ErrorCodes::IllegalOperation,
                        "You can't convertToCapped a sharded collection",
                        !cri.cm.isSharded());
            }

            boost::optional<SharedSemiFuture<void>> coordinatorCompletionFuture;
            {
                FixedFCVRegion fixedFcvRegion{opCtx};
                bool mustUseCoordinator = feature_flags::gConvertToCappedCoordinator.isEnabled(
                    (*fixedFcvRegion).acquireFCVSnapshot());

                if (!mustUseCoordinator) {
                    convertToCapped(opCtx, nss, request().getSize());
                    return;
                }

                auto coordinatorDoc = ConvertToCappedCoordinatorDocument();
                coordinatorDoc.setShardsvrConvertToCappedRequest(
                    request().getShardsvrConvertToCappedRequest());
                coordinatorDoc.setShardingDDLCoordinatorMetadata(
                    {{nss, DDLCoordinatorTypeEnum::kConvertToCapped}});

                auto service = ShardingDDLCoordinatorService::getService(opCtx);
                auto coordinator = checked_pointer_cast<ConvertToCappedCoordinator>(
                    service->getOrCreateInstance(opCtx, coordinatorDoc.toBSON()));
                coordinatorCompletionFuture.emplace(coordinator->getCompletionFuture());
            }

            coordinatorCompletionFuture->get(opCtx);
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
                        ->isAuthorizedForActionsOnResource(
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::internal));
        }
    };
};
MONGO_REGISTER_COMMAND(ShardsvrConvertToCappedCommand).forShard();

}  // namespace
}  // namespace mongo
