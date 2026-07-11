// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/s/resharding/coordinator_document_gen.h"
#include "mongo/db/s/resharding/resharding_coordinator.h"
#include "mongo/db/s/resharding/resharding_coordinator_service.h"
#include "mongo/db/s/resharding/resharding_coordinator_service_util.h"
#include "mongo/db/s/resharding/resharding_donor_recipient_common.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/request_types/abort_reshard_collection_gen.h"
#include "mongo/s/resharding/resharding_feature_flag_gen.h"
#include "mongo/s/resharding/type_collection_fields_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

void assertExistsReshardingDocument(OperationContext* opCtx, UUID reshardingUUID) {
    PersistentTaskStore<ReshardingCoordinatorDocument> store(
        NamespaceString::kConfigReshardingOperationsNamespace);

    boost::optional<ReshardingCoordinatorDocument> docOptional;
    store.forEach(opCtx,
                  BSON(ReshardingCoordinatorDocument::kReshardingUUIDFieldName << reshardingUUID),
                  [&](const ReshardingCoordinatorDocument& doc) {
                      docOptional.emplace(doc);
                      return false;
                  });
    uassert(ErrorCodes::NoSuchReshardCollection,
            "Could not find resharding document to abort resharding operation",
            !!docOptional);
}

auto assertGetReshardingMachine(OperationContext* opCtx,
                                UUID reshardingUUID,
                                boost::optional<mongo::ReshardingProvenanceEnum> provenance) {
    auto machine = resharding::tryGetReshardingStateMachineAndThrowIfShuttingDown<
        ReshardingCoordinatorService,
        ReshardingCoordinator,
        ReshardingCoordinatorDocument>(opCtx, reshardingUUID);

    uassert(ErrorCodes::NoSuchReshardCollection,
            "Could not find in-progress resharding operation to abort",
            machine);

    if (resharding::gFeatureFlagMoveCollection.isEnabled(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot()) ||
        resharding::gFeatureFlagUnshardCollection.isEnabled(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        uassert(ErrorCodes::IllegalOperation,
                "Could not find in-progress resharding operation with matching provenance",
                provenance && (*machine)->getMetadata().getProvenance() == provenance.get());
    }

    return *machine;
}

class ConfigsvrAbortReshardCollectionCommand final
    : public TypedCommand<ConfigsvrAbortReshardCollectionCommand> {
public:
    using Request = ConfigsvrAbortReshardCollection;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            uassert(ErrorCodes::IllegalOperation,
                    "_configsvrAbortReshardCollection can only be run on config servers",
                    serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));
            CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                          opCtx->getWriteConcern());

            const auto sourceNss = resharding::resolveReshardingSourceNss(opCtx, ns());
            const auto reshardingUUID = resharding::retrieveReshardingUUID(opCtx, sourceNss);

            LOGV2(5403501,
                  "Aborting resharding operation",
                  logAttrs(sourceNss),
                  "reshardingUUID"_attr = reshardingUUID);

            assertExistsReshardingDocument(opCtx, reshardingUUID);

            auto machine =
                assertGetReshardingMachine(opCtx, reshardingUUID, request().getProvenance());
            auto future = machine->getCompletionFuture();
            machine->abort(
                {resharding::kUserAbortReason, resharding::AbortType::kAbortWithQuiesce});

            auto completionStatus = future.getNoThrow(opCtx);

            // Receiving this error from the state machine indicates that resharding was
            // successfully aborted, and that the abort command should return OK.
            if (completionStatus == ErrorCodes::ReshardCollectionAborted) {
                return;
            }

            // Receiving an OK status from the machine after attempting to abort the resharding
            // operation indicates that the resharding operation ignored the abort attempt. The
            // resharding operation only ignores the abort attempt if the decision was already
            // persisted, implying that the resharding operation was in an unabortable state.
            uassert(ErrorCodes::ReshardCollectionCommitted,
                    "Can't abort resharding operation after the decision has been persisted",
                    completionStatus != Status::OK());

            // Return any other status to the client.
            uassertStatusOK(completionStatus);
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
        return "Internal command, which is exported by the sharding config server. Do not call "
               "directly. Aborts any in-progress resharding operations for this collection.";
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }
};
MONGO_REGISTER_COMMAND(ConfigsvrAbortReshardCollectionCommand).forShard();

}  // namespace
}  // namespace mongo
