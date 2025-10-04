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


#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/ddl/commit_reshard_collection_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/s/resharding/donor_document_gen.h"
#include "mongo/db/s/resharding/recipient_document_gen.h"
#include "mongo/db/s/resharding/resharding_donor_recipient_common.h"
#include "mongo/db/s/resharding/resharding_donor_service.h"
#include "mongo/db/s/resharding/resharding_recipient_service.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/vector_clock/vector_clock_mutable.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <string>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


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
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            uassert(ErrorCodes::IllegalOperation,
                    "_shardsvrCommitReshardCollection can only be run on shard servers",
                    serverGlobalParams.clusterRole.has(ClusterRole::ShardServer));
            CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                          opCtx->getWriteConcern());

            // Persist the config time to ensure that in case of stepdown next filtering metadata
            // refresh on the new primary will always fetch the latest information.
            VectorClockMutable::get(opCtx)->waitForDurableConfigTime().get(opCtx);

            std::vector<SharedSemiFuture<void>> futuresToWait;

            {
                auto recipientMachine =
                    resharding::tryGetReshardingStateMachineAndThrowIfShuttingDown<
                        ReshardingRecipientService,
                        ReshardingRecipientService::RecipientStateMachine,
                        ReshardingRecipientDocument>(opCtx, uuid());

                auto donorMachine = resharding::tryGetReshardingStateMachineAndThrowIfShuttingDown<
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

            for (const auto& doneFuture : futuresToWait) {
                doneFuture.get(opCtx);
            }

            // If commit actually went through, the resharding documents will be cleaned up. If
            // documents still exist, it could be because that commit was interrupted or that the
            // underlying replica set node is no longer primary.
            resharding::doNoopWrite(opCtx, "_shardsvrCommitReshardCollection no-op", ns());
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
        return "Internal command, which is exported by the shard server. Do not call directly. "
               "Commits an in-progress resharding operations";
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }
};
MONGO_REGISTER_COMMAND(ShardsvrCommitReshardCollectionCommand).forShard();

}  // namespace
}  // namespace mongo
