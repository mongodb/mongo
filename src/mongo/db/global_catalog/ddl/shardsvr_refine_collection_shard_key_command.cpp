// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/checked_cast.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/global_catalog/ddl/refine_collection_shard_key_coordinator.h"
#include "mongo/db/global_catalog/ddl/refine_collection_shard_key_coordinator_document_gen.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_coordinator_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_coordinator_service.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future.h"

#include <memory>
#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

class ShardsvrRefineCollectionShardKeyCommand final
    : public TypedCommand<ShardsvrRefineCollectionShardKeyCommand> {
public:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return Command::AllowedOnSecondary::kNever;
    }

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command, which is exported by the primary sharding server. Do not call "
               "directly. Refines Collection shard key.";
    }

    using Request = ShardsvrRefineCollectionShardKey;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            ShardingState::get(opCtx)->assertCanAcceptShardedCommands();
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            auto refineCoordinator = [&] {
                auto coordinatorDoc = RefineCollectionShardKeyCoordinatorDocument();
                coordinatorDoc.setShardingCoordinatorMetadata(
                    {{ns(), CoordinatorTypeEnum::kRefineCollectionShardKey}});
                coordinatorDoc.setRefineCollectionShardKeyRequest(
                    request().getRefineCollectionShardKeyRequest());

                auto service = ShardingCoordinatorService::getService(opCtx);
                return checked_pointer_cast<RefineCollectionShardKeyCoordinator>(
                    service->getOrCreateInstance(
                        opCtx, coordinatorDoc.toBSON(), FixedFCVRegion{opCtx}));
            }();

            refineCoordinator->getCompletionFuture().get(opCtx);
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

        /**
         * The ns() for when Request's IDL specifies "namespace: concatenate_with_db".
         */
        NamespaceString ns() const override {
            return request().getNamespace();
        }
    };
};
MONGO_REGISTER_COMMAND(ShardsvrRefineCollectionShardKeyCommand).forShard();

}  // namespace
}  // namespace mongo
