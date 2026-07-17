// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/compact_gen.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/shard_catalog/collection_catalog.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/topology/user_write_block/replica_set_write_block_state.h"
#include "mongo/logv2/log.h"

#include <string_view>

#include <boost/cstdint.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {

Status autoCompact(OperationContext* opCtx,
                   bool enable,
                   bool runOnce,
                   boost::optional<int64_t> freeSpaceTargetMB) {
    if (!opCtx->getServiceContext()->userWritesAllowed()) {
        return Status(ErrorCodes::IllegalOperation,
                      "autoCompact can only be executed when writes are allowed");
    }

    const auto& provider = rss::ReplicatedStorageService::get(opCtx).getPersistenceProvider();
    if (!provider.supportsCompaction()) {
        return Status(ErrorCodes::CommandNotSupported,
                      str::stream() << "autoCompact is not supported in this storage mode: "
                                    << provider.name());
    }

    auto* storageEngine = opCtx->getServiceContext()->getStorageEngine();

    // Holding the global lock to prevent racing with storage shutdown. However, no need to hold the
    // RSTL nor acquire a flow control ticket. This doesn't care about the replica state of the node
    // and the operation is not replicated.
    Lock::GlobalLock lk{
        opCtx,
        MODE_IS,
        Date_t::max(),
        Lock::InterruptBehavior::kThrow,
        Lock::GlobalLockOptions{.skipFlowControlTicket = true, .skipRSTLLock = true}};

    // Reject enabling auto compaction while replica set deletions are blocked. Disabling
    // autoCompact is always permitted.
    if (enable) {
        if (auto status =
                ReplicaSetWriteBlockState::get(opCtx)->checkIfCompactAllowedToStart(opCtx);
            !status.isOK()) {
            return status;
        }
    }

    std::shared_ptr<const CollectionCatalog> catalog = CollectionCatalog::get(opCtx);
    std::vector<std::string_view> excludedIdents;

    if (enable) {
        // The oplog is always excluded when enabling auto compaction. If this is a replica set,
        // ensure it exists, otherwise it may not and proceed.
        const auto replCoord = repl::ReplicationCoordinator::get(opCtx);
        const bool isReplSet = replCoord->getSettings().isReplSet();
        if (isReplSet) {
            auto state = replCoord->getMemberState();
            uassert(ErrorCodes::NotPrimaryOrSecondary,
                    "Can only run 'autoCompact' on a primary or secondary in steady-state",
                    state.primary() || state.secondary());
        }

        auto collection =
            catalog->lookupCollectionByNamespace(opCtx, NamespaceString::kRsOplogNamespace);

        tassert(8354800, "the oplog must exist in a replica set", collection || !isReplSet);

        if (collection)
            excludedIdents.push_back(collection->getSharedIdent()->getIdent());
    }

    AutoCompactOptions options{enable, runOnce, freeSpaceTargetMB, std::move(excludedIdents)};

    Status status =
        storageEngine->autoCompact(*shard_role_details::getRecoveryUnit(opCtx), options);
    if (!status.isOK())
        return status;

    LOGV2(8012100, "AutoCompact", "enabled"_attr = enable);
    return status;
}

class AutoCompactCmd final : public TypedCommand<AutoCompactCmd> {
public:
    using Request = AutoCompact;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            uassertStatusOK(autoCompact(opCtx,
                                        request().getCommandParameter(),
                                        request().getRunOnce(),
                                        request().getFreeSpaceTargetMB()));
        }

    private:
        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            auto* as = AuthorizationSession::get(opCtx->getClient());
            uassert(ErrorCodes::Unauthorized,
                    "Not authorized",
                    as->isAuthorizedForActionsOnResource(
                        ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                        ActionType::compact));
        }

        NamespaceString ns() const override {
            return NamespaceString::kEmpty;
        }
    };

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    std::string help() const override {
        return "enable auto compaction for a database\n"
               "warning: compact operation has blocking behaviour and is slow, enabling auto "
               "compact will allow compact to run on any collection at any time. You can cancel "
               "by disabling auto compact.\n"
               "{ autoCompact : <bool>, [freeSpaceTargetMB:<int64_t>], [runOnce:<bool>] }\n"
               "  freeSpaceTargetMB - minimum amount of space recoverable for compaction to "
               "proceed\n"
               "  runOnce - executes compaction on the database only once\n";
    }
};

MONGO_REGISTER_COMMAND(AutoCompactCmd).forShard();
}  // namespace mongo
