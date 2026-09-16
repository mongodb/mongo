// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/db/admission/execution_control/execution_admission_context.h"
#include "mongo/db/admission/ticketing/admission_context.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/replicated_fast_count/repair_replicated_metadata_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/lock_manager/exception_util.h"
#include "mongo/db/shard_role/shard_catalog/collection_catalog.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/util/assert_util.h"

#include <string>

namespace mongo {
namespace {

void _writeNoopOplogEntry(OperationContext* opCtx, const UUID& uuid, const BSONObj& metadata) {
    if (metadata.isEmpty()) {
        return;
    }

    ScopedAdmissionPriority<ExecutionAdmissionContext> priority{
        opCtx, AdmissionContext::Priority::kExempt};

    writeConflictRetry(opCtx, "repairReplicatedMetadata", NamespaceString::kRsOplogNamespace, [&] {
        WriteUnitOfWork wuow(opCtx);
        opCtx->getClient()->getServiceContext()->getOpObserver()->onInternalOpMessage(
            opCtx,
            NamespaceString::kEmpty,
            uuid,
            BSON("msg" << "Repairing collection's replicated metadata with diffs"),
            BSON("type" << "repairReplicatedMetadata" << "uuid" << uuid << "m" << metadata),
            boost::none,
            boost::none,
            boost::none,
            boost::none);
        wuow.commit();
    });
}

class RepairReplicatedMetadataCommand final : public TypedCommand<RepairReplicatedMetadataCommand> {
public:
    using Request = RepairReplicatedMetadata;

    std::string help() const override {
        return "Repairs replicated metadata.";
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            auto replCoord = repl::ReplicationCoordinator::get(opCtx);
            uassert(ErrorCodes::NoReplicationEnabled,
                    "Must have replication set up to run \"repairReplicatedMetadata\"",
                    replCoord->getSettings().isReplSet());
            uassert(ErrorCodes::NotWritablePrimary,
                    "Must be primary to run \"repairReplicatedMetadata\"",
                    replCoord->canAcceptWritesForDatabase(opCtx, DatabaseName::kAdmin));

            // Resolve and acquire the collection by UUID so the repair serializes with drops and
            // renames. Renames preserve the UUID, so retry if the collection moved; if it no
            // longer exists, the command is a true no-op.
            const auto& uuid = request().getUuid();
            while (true) {
                boost::optional<NamespaceString> nss;
                {
                    Lock::GlobalLock globalLock(
                        opCtx, MODE_IS, Date_t::max(), Lock::InterruptBehavior::kThrow);
                    nss = CollectionCatalog::get(opCtx)->lookupNSSByUUID(opCtx, uuid);
                }
                if (!nss) {
                    return;
                }

                auto collection =
                    acquireCollection(opCtx,
                                      CollectionAcquisitionRequest::fromOpCtx(
                                          opCtx, *nss, AcquisitionPrerequisites::kWrite),
                                      MODE_IX);
                if (collection.exists() && collection.uuid() == uuid) {
                    _writeNoopOplogEntry(opCtx, uuid, request().getMetadata().toBSON());
                    return;
                }
            }
        }

    private:
        NamespaceString ns() const override {
            return NamespaceString::kEmpty;
        }

        bool supportsWriteConcern() const override {
            return true;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            auto* authzSession = AuthorizationSession::get(opCtx->getClient());
            uassert(ErrorCodes::Unauthorized,
                    "Not authorized to repair replicated metadata",
                    authzSession->isAuthorizedForActionsOnResource(
                        ResourcePattern::forClusterResource(authzSession->getUserTenantId()),
                        ActionType::repairReplicatedMetadata));
        }
    };
};

MONGO_REGISTER_COMMAND(RepairReplicatedMetadataCommand).forShard();

}  // namespace
}  // namespace mongo
