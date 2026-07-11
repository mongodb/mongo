// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/error_codes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/router_role/routing_cache/catalog_cache.h"
#include "mongo/db/s/resharding/resharding_donor_recipient_common.h"
#include "mongo/db/s/resharding/resharding_donor_service.h"
#include "mongo/db/s/resharding/shardsvr_resharding_commands_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/storage/duplicate_key_error_info.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/logv2/log.h"
#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding

namespace mongo {
namespace {

class ShardsvrReshardDonorInitializeCommand final
    : public TypedCommand<ShardsvrReshardDonorInitializeCommand> {
public:
    using Request = ShardsvrReshardDonorInitialize;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            LOGV2(12992402,
                  "Received _shardsvrReshardDonorInitialize command",
                  "reshardingUUID"_attr = uuid(),
                  "lsid"_attr = opCtx->getLogicalSessionId(),
                  "txnNum"_attr = opCtx->getTxnNumber());

            uassert(ErrorCodes::IllegalOperation,
                    "_shardsvrReshardDonorInitialize can only be run on shard servers",
                    serverGlobalParams.clusterRole.has(ClusterRole::ShardServer));

            if (!resharding::tryGetReshardingStateMachine<ReshardingDonorService,
                                                          ReshardingDonorService::DonorStateMachine,
                                                          ReshardingDonorDocument>(opCtx, uuid())) {
                const auto& req = request();

                DonorShardContext donorCtx;
                donorCtx.setState(DonorStateEnum::kPreparingToDonate);

                ReshardingDonorDocument donorDoc{std::move(donorCtx), req.getRecipientShards()};
                donorDoc.setCommonReshardingMetadata(req.getCommonReshardingMetadata());

                // We clear the routing information for the temporary resharding namespace to ensure
                // this donor shard primary will refresh from the config server and see the chunk
                // distribution for the new resharding operation. We also invalidate the source
                // namespace since the coordinator's metadata write bumps its placement version,
                // matching the old refresh path where the donor flush updated it.
                auto* catalogCache = Grid::get(opCtx)->catalogCache();
                catalogCache->invalidateCollectionEntry_LINEARIZABLE(
                    req.getCommonReshardingMetadata().getTempReshardingNss());
                catalogCache->invalidateCollectionEntry_LINEARIZABLE(
                    req.getCommonReshardingMetadata().getSourceNss());

                resharding::createReshardingStateMachine<ReshardingDonorService,
                                                         ReshardingDonorService::DonorStateMachine,
                                                         ReshardingDonorDocument>(
                    opCtx, donorDoc, true);

                LOGV2(12092601,
                      "Initialized resharding donor state machine via "
                      "_shardsvrReshardDonorInitialize command",
                      "reshardingUUID"_attr = uuid(),
                      "lsid"_attr = opCtx->getLogicalSessionId(),
                      "txnNum"_attr = opCtx->getTxnNumber());
            } else {
                LOGV2(12092600,
                      "Donor state machine already exists for resharding operation",
                      "reshardingUUID"_attr = uuid(),
                      "lsid"_attr = opCtx->getLogicalSessionId(),
                      "txnNum"_attr = opCtx->getTxnNumber());
            }

            resharding::waitForStateDocumentMajorityCommitted(opCtx);
        }

    private:
        UUID uuid() const {
            return request().getCommandParameter();
        }

        NamespaceString ns() const override {
            return {};
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
        return "Internal command that initializes the resharding donor. Do not call directly.";
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
MONGO_REGISTER_COMMAND(ShardsvrReshardDonorInitializeCommand).forShard();

}  // namespace
}  // namespace mongo
