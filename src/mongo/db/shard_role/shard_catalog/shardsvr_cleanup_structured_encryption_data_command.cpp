// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/checked_cast.h"
#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/cleanup_structured_encryption_data_coordinator.h"
#include "mongo/db/cleanup_structured_encryption_data_coordinator_gen.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/commands/fle2_cleanup_gen.h"
#include "mongo/db/commands/fle2_compact.h"
#include "mongo/db/curop.h"
#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/ddl/sharding_coordinator_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_coordinator_service.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/lock_manager/exception_util.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/operation_sharding_state.h"
#include "mongo/db/topology/user_write_block/replica_set_write_block_bypass.h"
#include "mongo/db/topology/user_write_block/replica_set_write_block_state.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <set>
#include <string>
#include <string_view>

#include <boost/move/utility_core.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

class _shardsvrCleanupStructuredEncryptionDataCommand final
    : public TypedCommand<_shardsvrCleanupStructuredEncryptionDataCommand> {
public:
    using Request = CleanupStructuredEncryptionData;
    using Reply = typename Request::Reply;

    _shardsvrCleanupStructuredEncryptionDataCommand()
        : TypedCommand("_shardsvrCleanupStructuredEncryptionData"sv) {}

    bool skipApiVersionCheck() const final {
        // Internal command (server to server).
        return true;
    }

    std::string help() const final {
        return "Internal command. Do not call directly. Cleans up an ECOC collection.";
    }

    bool adminOnly() const final {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kNever;
    }

    std::set<std::string_view> sensitiveFieldNames() const final {
        return {CleanupStructuredEncryptionData::kCleanupTokensFieldName};
    }

    bool includeInCommandStats() const final {
        return false;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Reply typedRun(OperationContext* opCtx) {
            {
                std::lock_guard<Client> lk(*opCtx->getClient());
                CurOp::get(opCtx)->setShouldOmitDiagnosticInformation(lk, true);
            }

            // Like storage-level compact, QE cleanup reclaims disk space and is gated behind
            // allowDeletions. If the block is active with allowDeletions:true, bypass the general
            // write block so that internal ESC inserts/updates are not rejected by the op observer.
            auto* writeBlockState = ReplicaSetWriteBlockState::get(opCtx);
            uassertStatusOK(writeBlockState->checkIfCompactAllowedToStart(opCtx));
            if (writeBlockState->isReplicaSetWriteBlockingEnabled()) {
                ReplicaSetWriteBlockBypass::get(opCtx).set(true);
            }

            auto cleanupCoordinator =
                [&]() -> std::shared_ptr<ShardingCoordinatorService::Instance> {
                FixedFCVRegion fixedFcvRegion(opCtx);
                auto cleanup = writeConflictRetry(opCtx,
                                                  Request::kCommandName,
                                                  request().getNamespace(),
                                                  [&]() { return makeRequest(opCtx); });
                return ShardingCoordinatorService::getService(opCtx)->getOrCreateInstance(
                    opCtx, cleanup.toBSON(), fixedFcvRegion);
            }();

            return checked_pointer_cast<CleanupStructuredEncryptionDataCoordinator>(
                       cleanupCoordinator)
                ->getResponse(opCtx);
        }

    private:
        CleanupStructuredEncryptionDataState makeRequest(OperationContext* opCtx) {
            const auto& req = request();
            const auto& nss = req.getNamespace();
            // Routers route DDLS to the db-primary shard, with a 'databaseVersion' attached to
            // the command but no 'shardVersion'. This is okay, because the db-primary shard
            // will coordinate the operation. However, we need to attach an IGNORED Shard Role
            // so that we are able to access the collection metadata. This is ok as long as we
            // don't access user data.
            boost::optional<ScopedSetShardRole> optShardRoleIgnore;
            if (!OperationShardingState::get(opCtx).getShardVersion(nss)) {
                ShardVersion shardVersionIgnored;
                shardVersionIgnored.setPlacementVersionIgnored();
                optShardRoleIgnore.emplace(opCtx, nss, shardVersionIgnored, boost::none);
            }
            auto baseColl = acquireCollection(opCtx,
                                              CollectionAcquisitionRequest::fromOpCtx(
                                                  opCtx, nss, AcquisitionPrerequisites::kWrite),
                                              MODE_IX);
            uassert(ErrorCodes::NamespaceNotFound,
                    str::stream() << "Unknown collection: " << nss.toStringForErrorMsg(),
                    baseColl.exists());

            validateCleanupRequest(req, *(baseColl.getCollectionPtr().get()));

            auto namespaces =
                uassertStatusOK(EncryptedStateCollectionsNamespaces::createFromDataCollection(
                    *(baseColl.getCollectionPtr().get())));

            CleanupStructuredEncryptionDataState cleanup;

            // To avoid deadlock, IX locks for ecocRenameNss and ecocNss must be acquired in the
            // same order they'd be acquired during renameCollection (ascending ResourceId order).
            // The 2 collections are unrouted so we need to specify a version. By design, these
            // collections are always unsharded (untracked) and therefore on the primary shard.
            {
                auto dbVersion = OperationShardingState::get(opCtx).getDbVersion(nss.dbName());
                auto pc = PlacementConcern(dbVersion, ShardVersion::UNTRACKED());
                CollectionAcquisitionRequests requests = {
                    CollectionAcquisitionRequest(namespaces.ecocNss,
                                                 pc,
                                                 repl::ReadConcernArgs::get(opCtx),
                                                 AcquisitionPrerequisites::kWrite),
                    CollectionAcquisitionRequest(namespaces.ecocRenameNss,
                                                 pc,
                                                 repl::ReadConcernArgs::get(opCtx),
                                                 AcquisitionPrerequisites::kWrite),
                };

                // Acquire all the nss at the same snapshot.
                auto allAcquisitions =
                    makeAcquisitionMap(acquireCollections(opCtx, requests, MODE_IX));
                auto ecocColl = allAcquisitions.extract(namespaces.ecocNss).mapped();

                if (ecocColl.exists()) {
                    cleanup.setEcocUuid(ecocColl.uuid());
                }
                auto ecocTempColl = allAcquisitions.extract(namespaces.ecocRenameNss).mapped();
                if (ecocTempColl.exists()) {
                    cleanup.setEcocRenameUuid(ecocTempColl.uuid());
                }
            }

            cleanup.setShardingCoordinatorMetadata(
                {{nss, CoordinatorTypeEnum::kCleanupStructuredEncryptionData}});
            cleanup.setEscNss(namespaces.escNss);
            cleanup.setEcocNss(namespaces.ecocNss);
            cleanup.setEcocRenameNss(namespaces.ecocRenameNss);
            cleanup.setCleanupTokens(req.getCleanupTokens().getOwned());

            return cleanup;
        }

        NamespaceString ns() const override {
            return request().getNamespace();
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
};  // namespace
MONGO_REGISTER_COMMAND(_shardsvrCleanupStructuredEncryptionDataCommand).forShard();

}  // namespace
}  // namespace mongo
