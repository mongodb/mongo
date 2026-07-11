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
#include "mongo/db/s/resharding/resharding_recipient_service.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/s/resharding/shardsvr_resharding_commands_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/storage/duplicate_key_error_info.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/logv2/log.h"
#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/s/resharding/resharding_feature_flag_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding

namespace mongo {
namespace {

class ShardsvrReshardRecipientInitializeCommand final
    : public TypedCommand<ShardsvrReshardRecipientInitializeCommand> {
public:
    using Request = ShardsvrReshardRecipientInitialize;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            LOGV2(12992404,
                  "Received _shardsvrReshardRecipientInitialize command",
                  "reshardingUUID"_attr = uuid(),
                  "lsid"_attr = opCtx->getLogicalSessionId(),
                  "txnNum"_attr = opCtx->getTxnNumber());

            uassert(ErrorCodes::IllegalOperation,
                    "_shardsvrReshardRecipientInitialize can only be run on shard servers",
                    serverGlobalParams.clusterRole.has(ClusterRole::ShardServer));

            if (!resharding::tryGetReshardingStateMachine<
                    ReshardingRecipientService,
                    ReshardingRecipientService::RecipientStateMachine,
                    ReshardingRecipientDocument>(opCtx, uuid())) {
                const auto& req = request();

                RecipientShardContext recipientCtx;
                recipientCtx.setState(RecipientStateEnum::kAwaitingFetchTimestamp);

                ReshardingRecipientDocument recipientDoc{std::move(recipientCtx)};
                recipientDoc.setCommonReshardingMetadata(req.getCommonReshardingMetadata());
                recipientDoc.setReshardingRecipientOptions(req.getRecipientOptions());

                // We clear the routing information for the temporary resharding namespace to ensure
                // this recipient shard primary will refresh from the config server and see the
                // chunk distribution for the new resharding operation.
                auto tempNss = req.getCommonReshardingMetadata().getTempReshardingNss();
                auto* catalogCache = Grid::get(opCtx)->catalogCache();
                catalogCache->invalidateCollectionEntry_LINEARIZABLE(tempNss);

                // Refresh routing info for the temp namespace and check whether this shard owns
                // any chunks. This determines whether we can skip cloning/applying phases.
                auto cri = uassertStatusOK(catalogCache->getCollectionRoutingInfo(opCtx, tempNss));
                bool noChunksOnThisShard = false;
                if (cri.hasRoutingTable()) {
                    std::set<ShardId> shards;
                    cri.getCurrentChunkManager().getAllShardIds(&shards);
                    noChunksOnThisShard =
                        shards.find(ShardingState::get(opCtx)->shardId()) == shards.end();
                }

                const auto fom = req.getCommonReshardingMetadata().getForwardableOpMetadata();
                if (noChunksOnThisShard) {
                    if (resharding::isEnabledWithPinnedVersion(
                            fom,
                            resharding::gFeatureFlagReshardingSkipCloningAndApplyingIfApplicable)) {
                        recipientDoc.setSkipCloningAndApplying(true);
                    }
                    if (resharding::isEnabledWithPinnedVersion(
                            fom, resharding::gFeatureFlagReshardingSkipCloningIfApplicable)) {
                        recipientDoc.setSkipCloning(true);
                    }
                    if (resharding::isEnabledWithPinnedVersion(
                            fom,
                            resharding::gFeatureFlagReshardingSkipBuildingIndexesIfApplicable)) {
                        recipientDoc.setSkipBuildingIndexes(true);
                    }
                }
                if (resharding::isEnabledWithPinnedVersion(
                        fom, resharding::gFeatureFlagReshardingStoreOplogFetcherProgress)) {
                    recipientDoc.setStoreOplogFetcherProgress(true);
                }

                resharding::createReshardingStateMachine<
                    ReshardingRecipientService,
                    ReshardingRecipientService::RecipientStateMachine,
                    ReshardingRecipientDocument>(opCtx, recipientDoc, true);

                LOGV2(12092603,
                      "Initialized resharding recipient state machine via "
                      "_shardsvrReshardRecipientInitialize command",
                      "reshardingUUID"_attr = uuid(),
                      "lsid"_attr = opCtx->getLogicalSessionId(),
                      "txnNum"_attr = opCtx->getTxnNumber());
            } else {
                LOGV2(12092602,
                      "Recipient state machine already exists for resharding operation",
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
        return "Internal command that initializes the resharding recipient. Do not call directly.";
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
MONGO_REGISTER_COMMAND(ShardsvrReshardRecipientInitializeCommand).forShard();

}  // namespace
}  // namespace mongo
