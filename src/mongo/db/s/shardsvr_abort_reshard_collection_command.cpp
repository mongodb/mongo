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
#include "mongo/s/request_types/abort_reshard_collection_gen.h"
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

class ShardsvrAbortReshardCollectionCommand final
    : public TypedCommand<ShardsvrAbortReshardCollectionCommand> {
public:
    using Request = ShardsvrAbortReshardCollection;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            uassert(ErrorCodes::IllegalOperation,
                    "_shardsvrAbortReshardCollection can only be run on shard servers",
                    serverGlobalParams.clusterRole.has(ClusterRole::ShardServer));
            CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                          opCtx->getWriteConcern());

            // Persist the config time to ensure that in case of stepdown next filtering metadata
            // refresh on the new primary will always fetch the latest information.
            VectorClockMutable::get(opCtx)->waitForDurableConfigTime().get(opCtx);

            std::vector<SharedSemiFuture<void>> futuresToWait;

            if (auto machine = resharding::tryGetReshardingStateMachineAndThrowIfShuttingDown<
                    ReshardingRecipientService,
                    ReshardingRecipientService::RecipientStateMachine,
                    ReshardingRecipientDocument>(opCtx, uuid())) {
                futuresToWait.push_back((*machine)->getCompletionFuture());

                LOGV2(5663800,
                      "Aborting resharding recipient participant",
                      "reshardingUUID"_attr = uuid());
                (*machine)->abort(isUserCanceled());
            }

            if (auto machine = resharding::tryGetReshardingStateMachineAndThrowIfShuttingDown<
                    ReshardingDonorService,
                    ReshardingDonorService::DonorStateMachine,
                    ReshardingDonorDocument>(opCtx, uuid())) {
                futuresToWait.push_back((*machine)->getCompletionFuture());

                LOGV2(5663801,
                      "Aborting resharding donor participant",
                      "reshardingUUID"_attr = uuid());
                (*machine)->abort(isUserCanceled());
            }

            for (const auto& doneFuture : futuresToWait) {
                doneFuture.get(opCtx);
            }

            // If abort actually went through, the resharding documents should be cleaned up.
            // If they still exists, it could be because that it was interrupted or it is no
            // longer primary.
            resharding::doNoopWrite(opCtx, "_shardsvrAbortReshardCollection no-op", ns());
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
               "Aborts any in-progress resharding operations.";
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }
};
MONGO_REGISTER_COMMAND(ShardsvrAbortReshardCollectionCommand).forShard();

}  // namespace
}  // namespace mongo
