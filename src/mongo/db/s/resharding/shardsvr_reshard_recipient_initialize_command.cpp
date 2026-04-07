/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
#include "mongo/db/s/resharding/shardsvr_resharding_commands_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/storage/duplicate_key_error_info.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/version_context.h"
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

            uassert(ErrorCodes::IllegalOperation,
                    "_shardsvrReshardRecipientInitialize can only be run on shard servers",
                    serverGlobalParams.clusterRole.has(ClusterRole::ShardServer));

            if (resharding::tryGetReshardingStateMachine<
                    ReshardingRecipientService,
                    ReshardingRecipientService::RecipientStateMachine,
                    ReshardingRecipientDocument>(opCtx, uuid())) {
                LOGV2(12092602,
                      "Recipient state machine already exists for resharding operation",
                      "reshardingUUID"_attr = uuid());
                return;
            }

            const auto& req = request();

            RecipientShardContext recipientCtx;
            recipientCtx.setState(RecipientStateEnum::kAwaitingFetchTimestamp);

            ReshardingRecipientDocument recipientDoc{std::move(recipientCtx)};
            recipientDoc.setCommonReshardingMetadata(req.getCommonReshardingMetadata());
            recipientDoc.setReshardingRecipientOptions(req.getRecipientOptions());

            // We clear the routing information for the temporary resharding namespace to ensure
            // this recipient shard primary will refresh from the config server and see the chunk
            // distribution for the new resharding operation.
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

            const auto& vCtx = VersionContext::getDecoration(opCtx);
            auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
            if (noChunksOnThisShard) {
                if (resharding::gFeatureFlagReshardingSkipCloningAndApplyingIfApplicable.isEnabled(
                        vCtx, fcvSnapshot)) {
                    recipientDoc.setSkipCloningAndApplying(true);
                }
                if (resharding::gFeatureFlagReshardingSkipCloningIfApplicable.isEnabled(
                        vCtx, fcvSnapshot)) {
                    recipientDoc.setSkipCloning(true);
                }
                if (resharding::gFeatureFlagReshardingSkipBuildingIndexesIfApplicable.isEnabled(
                        vCtx, fcvSnapshot)) {
                    recipientDoc.setSkipBuildingIndexes(true);
                }
            }
            if (resharding::gFeatureFlagReshardingStoreOplogFetcherProgress.isEnabled(
                    vCtx, fcvSnapshot)) {
                recipientDoc.setStoreOplogFetcherProgress(true);
            }

            resharding::createReshardingStateMachine<
                ReshardingRecipientService,
                ReshardingRecipientService::RecipientStateMachine,
                ReshardingRecipientDocument>(opCtx, recipientDoc, true);

            LOGV2(12092603,
                  "Initialized resharding recipient state machine via "
                  "_shardsvrReshardRecipientInitialize command",
                  "reshardingUUID"_attr = uuid());
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
