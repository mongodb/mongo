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
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/resharding/resharding_coordinator_service.h"
#include "mongo/db/s/resharding/resharding_donor_recipient_common.h"
#include "mongo/db/s/resharding_util.h"
#include "mongo/logv2/log.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/abort_reshard_collection_gen.h"
#include "mongo/s/resharding/resharding_feature_flag_gen.h"

namespace mongo {
namespace {

class ShardsvrAbortReshardCollectionCommand final
    : public TypedCommand<ShardsvrAbortReshardCollectionCommand> {
public:
    using Request = ShardsvrAbortReshardCollection;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            opCtx->setAlwaysInterruptAtStepDownOrUp();

            uassert(ErrorCodes::IllegalOperation,
                    "_shardsvrAbortReshardCollection can only be run on shard servers",
                    serverGlobalParams.clusterRole == ClusterRole::ShardServer);
            uassert(ErrorCodes::InvalidOptions,
                    "_shardsvrAbortReshardCollection must be called with majority writeConcern",
                    opCtx->getWriteConcern().wMode == WriteConcernOptions::kMajority);

            std::vector<SharedSemiFuture<void>> futuresToWait;

            if (auto machine = resharding::tryGetReshardingStateMachine<
                    ReshardingRecipientService,
                    ReshardingRecipientService::RecipientStateMachine,
                    ReshardingRecipientDocument>(opCtx, uuid())) {
                futuresToWait.push_back((*machine)->getCompletionFuture());

                LOGV2(5663800,
                      "Aborting resharding recipient participant",
                      "reshardingUUID"_attr = uuid());
                (*machine)->abort(isUserCanceled());
            }

            if (auto machine = resharding::tryGetReshardingStateMachine<
                    ReshardingDonorService,
                    ReshardingDonorService::DonorStateMachine,
                    ReshardingDonorDocument>(opCtx, uuid())) {
                futuresToWait.push_back((*machine)->getCompletionFuture());

                LOGV2(5663801,
                      "Aborting resharding donor participant",
                      "reshardingUUID"_attr = uuid());
                (*machine)->abort(isUserCanceled());
            }

            for (auto doneFuture : futuresToWait) {
                doneFuture.wait(opCtx);
            }

            // If abort actually went through, the resharding documents should be cleaned up.
            // If they still exists, it could be because that it was interrupted or it is no
            // longer primary.
            doNoopWrite(opCtx, "_shardsvrAbortReshardCollection no-op", ns());
            PersistentTaskStore<CommonReshardingMetadata> donorReshardingOpStore(
                NamespaceString::kDonorReshardingOperationsNamespace);
            uassert(5563802,
                    "Donor state document still exists after attempted abort",
                    donorReshardingOpStore.count(
                        opCtx, BSON(ReshardingDonorDocument::kReshardingUUIDFieldName << uuid())) ==
                        0);

            PersistentTaskStore<CommonReshardingMetadata> recipientReshardingOpStore(
                NamespaceString::kRecipientReshardingOperationsNamespace);
            uassert(
                5563803,
                "Recipient state document still exists after attempted abort",
                recipientReshardingOpStore.count(
                    opCtx, BSON(ReshardingRecipientDocument::kReshardingUUIDFieldName << uuid())) ==
                    0);
        }

    private:
        bool isUserCanceled() const {
            return request().getUserCanceled();
        }

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
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                           ActionType::internal));
        }
    };

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command, which is exported by the shard server. Do not call directly. "
               "Aborts any in-progress resharding operations.";
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }
} shardsvrAbortReshardCollectionCmd;

}  // namespace
}  // namespace mongo
