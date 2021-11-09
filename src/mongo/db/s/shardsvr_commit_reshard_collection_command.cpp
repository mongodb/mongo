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
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/resharding/resharding_donor_recipient_common.h"
#include "mongo/db/s/resharding_util.h"
#include "mongo/logv2/log.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/commit_reshard_collection_gen.h"
#include "mongo/s/resharding/resharding_feature_flag_gen.h"

namespace mongo {
namespace {

class ShardsvrCommitReshardCollectionCommand final
    : public TypedCommand<ShardsvrCommitReshardCollectionCommand> {
public:
    using Request = ShardsvrCommitReshardCollection;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            opCtx->setAlwaysInterruptAtStepDownOrUp();

            uassert(ErrorCodes::IllegalOperation,
                    "_shardsvrCommitReshardCollection can only be run on shard servers",
                    serverGlobalParams.clusterRole == ClusterRole::ShardServer);
            uassert(ErrorCodes::InvalidOptions,
                    "_shardsvrCommitReshardCollection must be called with majority writeConcern",
                    opCtx->getWriteConcern().wMode == WriteConcernOptions::kMajority);

            std::vector<SharedSemiFuture<void>> futuresToWait;

            {
                auto recipientMachine = resharding::tryGetReshardingStateMachine<
                    ReshardingRecipientService,
                    ReshardingRecipientService::RecipientStateMachine,
                    ReshardingRecipientDocument>(opCtx, uuid());

                auto donorMachine = resharding::tryGetReshardingStateMachine<
                    ReshardingDonorService,
                    ReshardingDonorService::DonorStateMachine,
                    ReshardingDonorDocument>(opCtx, uuid());

                if (recipientMachine) {
                    futuresToWait.push_back((*recipientMachine)->getCompletionFuture());

                    LOGV2(5795300,
                          "Committing resharding recipient participant",
                          "reshardingUUID"_attr = uuid());

                    (*recipientMachine)->commit();
                }

                if (donorMachine) {
                    futuresToWait.push_back((*donorMachine)->getCompletionFuture());

                    LOGV2(5795301,
                          "Committing resharding donor participant",
                          "reshardingUUID"_attr = uuid());

                    (*donorMachine)->commit();
                }
            }

            for (auto doneFuture : futuresToWait) {
                doneFuture.wait(opCtx);
            }

            // If commit actually went through, the resharding documents will be cleaned up. If
            // documents still exist, it could be because that commit was interrupted or that the
            // underlying replica set node is no longer primary.
            doNoopWrite(opCtx, "_shardsvrCommitReshardCollection no-op", ns());
            PersistentTaskStore<CommonReshardingMetadata> donorReshardingOpStore(
                NamespaceString::kDonorReshardingOperationsNamespace);
            uassert(5795302,
                    "Donor state document still exists after attempted commit",
                    donorReshardingOpStore.count(
                        opCtx, BSON(ReshardingDonorDocument::kReshardingUUIDFieldName << uuid())) ==
                        0);

            PersistentTaskStore<CommonReshardingMetadata> recipientReshardingOpStore(
                NamespaceString::kRecipientReshardingOperationsNamespace);
            uassert(
                5795303,
                "Recipient state document still exists after attempted commit",
                recipientReshardingOpStore.count(
                    opCtx, BSON(ReshardingRecipientDocument::kReshardingUUIDFieldName << uuid())) ==
                    0);
        }

    private:
        UUID uuid() const {
            return request().getReshardingUUID();
        }

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

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command, which is exported by the shard server. Do not call directly. "
               "Commits an in-progress resharding operations";
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }
} shardsvrCommitReshardCollectionCmd;

}  // namespace
}  // namespace mongo
