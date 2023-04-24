/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/catalog_raii.h"
#include "mongo/db/repl/tenant_migration_access_blocker_util.h"
#include "mongo/db/serverless/serverless_operation_lock_registry.h"
#include "mongo/db/serverless/shard_split_donor_op_observer.h"
#include "mongo/db/serverless/shard_split_state_machine_gen.h"
#include "mongo/db/serverless/shard_split_utils.h"

namespace mongo {
namespace {

bool isSecondary(const OperationContext* opCtx) {
    return !opCtx->writesAreReplicated();
}

struct SplitCleanupDetails {
    UUID migrationId;
    bool shouldReleaseLock;
};
const auto splitCleanupDetails =
    OperationContext::declareDecoration<boost::optional<SplitCleanupDetails>>();

ShardSplitDonorDocument parseAndValidateDonorDocument(const BSONObj& doc) {
    auto donorStateDoc = ShardSplitDonorDocument::parse(IDLParserContext("donorStateDoc"), doc);
    const std::string errmsg = "Invalid donor state doc, {}: {}";

    if (donorStateDoc.getExpireAt()) {
        uassert(ErrorCodes::BadValue,
                "Contains 'expireAt' but the split has not committed or aborted",
                donorStateDoc.getState() == ShardSplitDonorStateEnum::kCommitted ||
                    donorStateDoc.getState() == ShardSplitDonorStateEnum::kAborted);
    }

    switch (donorStateDoc.getState()) {
        case ShardSplitDonorStateEnum::kUninitialized:
            uassert(ErrorCodes::BadValue,
                    fmt::format(
                        errmsg, "blockOpTime should not be set in data sync state", doc.toString()),
                    !donorStateDoc.getBlockOpTime());
            uassert(ErrorCodes::BadValue,
                    fmt::format(errmsg,
                                "CommitOrAbortOpTime should not be set in data sync state",
                                doc.toString()),
                    !donorStateDoc.getCommitOrAbortOpTime());
            uassert(ErrorCodes::BadValue,
                    fmt::format(errmsg,
                                "Cannot have abortReason while being in data sync state",
                                doc.toString()),
                    !donorStateDoc.getAbortReason());
            break;
        case ShardSplitDonorStateEnum::kAbortingIndexBuilds:
            uassert(ErrorCodes::BadValue,
                    errmsg,
                    !donorStateDoc.getBlockOpTime() && !donorStateDoc.getCommitOrAbortOpTime() &&
                        !donorStateDoc.getAbortReason());
            break;
        case ShardSplitDonorStateEnum::kBlocking:
            uassert(ErrorCodes::BadValue,
                    fmt::format(errmsg,
                                "Missing blockOpTime while being in blocking state",
                                doc.toString()),
                    donorStateDoc.getBlockOpTime());
            uassert(
                ErrorCodes::BadValue,
                fmt::format(errmsg,
                            "CommitOrAbortOpTime shouldn't be set while being in blocking state",
                            doc.toString()),
                !donorStateDoc.getCommitOrAbortOpTime());
            uassert(ErrorCodes::BadValue,
                    fmt::format(errmsg,
                                "Cannot have an abortReason while being in blocking state",
                                doc.toString()),
                    !donorStateDoc.getAbortReason());
            break;
        case ShardSplitDonorStateEnum::kCommitted:
            uassert(ErrorCodes::BadValue,
                    fmt::format(errmsg,
                                "Missing blockOpTime while being in committed state",
                                doc.toString()),
                    donorStateDoc.getBlockOpTime());
            uassert(ErrorCodes::BadValue,
                    fmt::format(errmsg,
                                "Missing CommitOrAbortOpTime while being in committed state",
                                doc.toString()),
                    donorStateDoc.getCommitOrAbortOpTime());
            uassert(ErrorCodes::BadValue,
                    fmt::format(errmsg,
                                "Cannot have abortReason while being in committed state",
                                doc.toString()),
                    !donorStateDoc.getAbortReason());
            break;
        case ShardSplitDonorStateEnum::kAborted:
            uassert(ErrorCodes::BadValue,
                    fmt::format(
                        errmsg, "Missing abortReason while being in aborted state", doc.toString()),
                    donorStateDoc.getAbortReason());
            uassert(ErrorCodes::BadValue,
                    fmt::format(errmsg,
                                "Missing CommitOrAbortOpTime while being in aborted state",
                                doc.toString()),
                    donorStateDoc.getCommitOrAbortOpTime());
            break;
        default:
            MONGO_UNREACHABLE;
    }

    return donorStateDoc;
}

/**
 * Initializes the TenantMigrationDonorAccessBlocker for the tenant migration denoted by the given
 * state doc.
 */
void onTransitionToAbortingIndexBuilds(OperationContext* opCtx,
                                       const ShardSplitDonorDocument& donorStateDoc) {
    invariant(donorStateDoc.getState() == ShardSplitDonorStateEnum::kAbortingIndexBuilds);
    invariant(donorStateDoc.getTenantIds());
    invariant(donorStateDoc.getRecipientConnectionString());

    ServerlessOperationLockRegistry::get(opCtx->getServiceContext())
        .acquireLock(ServerlessOperationLockRegistry::LockType::kShardSplit, donorStateDoc.getId());
    opCtx->recoveryUnit()->onRollback(
        [migrationId = donorStateDoc.getId()](OperationContext* opCtx) {
            ServerlessOperationLockRegistry::get(opCtx->getServiceContext())
                .releaseLock(ServerlessOperationLockRegistry::LockType::kShardSplit, migrationId);
        });

    auto tenantIds = *donorStateDoc.getTenantIds();
    auto mtab = std::make_shared<TenantMigrationDonorAccessBlocker>(opCtx->getServiceContext(),
                                                                    donorStateDoc.getId());
    TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext()).add(tenantIds, mtab);

    opCtx->recoveryUnit()->onRollback([migrationId =
                                           donorStateDoc.getId()](OperationContext* opCtx) {
        TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext())
            .removeAccessBlockersForMigration(migrationId,
                                              TenantMigrationAccessBlocker::BlockerType::kDonor);
    });
}

/**
 * Transitions the TenantMigrationDonorAccessBlocker to the blocking state.
 */
void onTransitionToBlocking(OperationContext* opCtx, const ShardSplitDonorDocument& donorStateDoc) {
    invariant(donorStateDoc.getState() == ShardSplitDonorStateEnum::kBlocking);
    invariant(donorStateDoc.getBlockOpTime());

    auto mtab = tenant_migration_access_blocker::getDonorAccessBlockerForMigration(
        opCtx->getServiceContext(), donorStateDoc.getId());
    invariant(mtab);

    if (isSecondary(opCtx)) {
        // A primary calls startBlockingWrites on the TenantMigrationDonorAccessBlocker before
        // reserving the OpTime for the "start blocking" write, so only secondaries call
        // startBlockingWrites on the TenantMigrationDonorAccessBlocker in the op observer.
        mtab->startBlockingWrites();
    }

    // Both primaries and secondaries call startBlockingReadsAfter in the op observer, since
    // startBlockingReadsAfter just needs to be called before the "start blocking" write's oplog
    // hole is filled.
    mtab->startBlockingReadsAfter(donorStateDoc.getBlockOpTime()->getTimestamp());
}

/**
 * Transitions the TenantMigrationDonorAccessBlocker to the committed state.
 */
void onTransitionToCommitted(OperationContext* opCtx,
                             const ShardSplitDonorDocument& donorStateDoc) {
    invariant(donorStateDoc.getState() == ShardSplitDonorStateEnum::kCommitted);
    invariant(donorStateDoc.getCommitOrAbortOpTime());

    auto mtab = tenant_migration_access_blocker::getDonorAccessBlockerForMigration(
        opCtx->getServiceContext(), donorStateDoc.getId());
    invariant(mtab);

    mtab->setCommitOpTime(opCtx, donorStateDoc.getCommitOrAbortOpTime().value());
}

/**
 * Transitions the TenantMigrationDonorAccessBlocker to the aborted state.
 */
void onTransitionToAborted(OperationContext* opCtx, const ShardSplitDonorDocument& donorStateDoc) {
    invariant(donorStateDoc.getState() == ShardSplitDonorStateEnum::kAborted);
    invariant(donorStateDoc.getCommitOrAbortOpTime());

    auto mtab = tenant_migration_access_blocker::getDonorAccessBlockerForMigration(
        opCtx->getServiceContext(), donorStateDoc.getId());
    if (!mtab) {
        // The only case where there can be no tenants is when the instance is created by the
        // abort command. In that case, no tenant migration blockers are created and the state
        // will go straight to abort.
        invariant(donorStateDoc.getState() == ShardSplitDonorStateEnum::kUninitialized);
        return;
    }

    mtab->setAbortOpTime(opCtx, donorStateDoc.getCommitOrAbortOpTime().value());
}

/**
 * Used to update the TenantMigrationDonorAccessBlocker for the migration denoted by the donor's
 * state doc once the write for updating the doc is committed.
 */
class TenantMigrationDonorCommitOrAbortHandler final : public RecoveryUnit::Change {
public:
    TenantMigrationDonorCommitOrAbortHandler(ShardSplitDonorDocument donorStateDoc)
        : _donorStateDoc(std::move(donorStateDoc)) {}

    void commit(OperationContext* opCtx, boost::optional<Timestamp>) override {
        if (_donorStateDoc.getExpireAt()) {
            ServerlessOperationLockRegistry::get(opCtx->getServiceContext())
                .releaseLock(ServerlessOperationLockRegistry::LockType::kShardSplit,
                             _donorStateDoc.getId());
            auto mtab = tenant_migration_access_blocker::getDonorAccessBlockerForMigration(
                opCtx->getServiceContext(), _donorStateDoc.getId());

            if (!mtab) {
                // The state doc and TenantMigrationDonorAccessBlocker for this
                // migration were removed immediately after expireAt was set. This is
                // unlikely to occur in production where the garbage collection delay
                // should be sufficiently large.
                return;
            }

            if (isSecondary(opCtx)) {
                // Setting expireAt implies that the TenantMigrationDonorAccessBlocker
                // for this migration will be removed shortly after this. However, a
                // lagged secondary might not manage to advance its majority commit
                // point past the migration commit or abort opTime and consequently
                // transition out of the blocking state before the
                // TenantMigrationDonorAccessBlocker is removed. When this occurs,
                // blocked reads or writes will be left waiting for the migration
                // decision indefinitely. To avoid that, notify the
                // TenantMigrationDonorAccessBlocker here that the commit or abort
                // opTime has been majority committed (guaranteed to be true since by
                // design the donor never marks its state doc as garbage collectable
                // before the migration decision is majority committed).
                mtab->onMajorityCommitPointUpdate(_donorStateDoc.getCommitOrAbortOpTime().value());
            }

            if (_donorStateDoc.getState() == ShardSplitDonorStateEnum::kAborted) {
                invariant(mtab->inStateAborted());
                // The migration durably aborted and is now marked as garbage
                // collectable, remove its TenantMigrationDonorAccessBlocker right away
                // to allow back-to-back migration retries.
                TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext())
                    .removeAccessBlockersForMigration(
                        _donorStateDoc.getId(), TenantMigrationAccessBlocker::BlockerType::kDonor);
            }

            return;
        }

        switch (_donorStateDoc.getState()) {
            case ShardSplitDonorStateEnum::kCommitted:
                onTransitionToCommitted(opCtx, _donorStateDoc);
                break;
            case ShardSplitDonorStateEnum::kAborted:
                onTransitionToAborted(opCtx, _donorStateDoc);
                break;
            default:
                MONGO_UNREACHABLE;
        }
    }

    void rollback(OperationContext* opCtx) override {}

private:
    const ShardSplitDonorDocument _donorStateDoc;
};

}  // namespace

void ShardSplitDonorOpObserver::onInserts(OperationContext* opCtx,
                                          const CollectionPtr& coll,
                                          std::vector<InsertStatement>::const_iterator first,
                                          std::vector<InsertStatement>::const_iterator last,
                                          std::vector<bool> fromMigrate,
                                          bool defaultFromMigrate) {
    if (coll->ns() != NamespaceString::kShardSplitDonorsNamespace ||
        tenant_migration_access_blocker::inRecoveryMode(opCtx)) {
        return;
    }

    for (auto it = first; it != last; it++) {
        auto donorStateDoc = parseAndValidateDonorDocument(it->doc);
        switch (donorStateDoc.getState()) {
            case ShardSplitDonorStateEnum::kAbortingIndexBuilds:
                onTransitionToAbortingIndexBuilds(opCtx, donorStateDoc);
                break;
            case ShardSplitDonorStateEnum::kAborted:
                // If the operation starts aborted, do not do anything.
                break;
            default:
                uasserted(ErrorCodes::IllegalOperation,
                          "Cannot insert donor's state document with state other than 'aborted' or "
                          "'aborting index builds'.");
        }
    }
}

void ShardSplitDonorOpObserver::onUpdate(OperationContext* opCtx,
                                         const OplogUpdateEntryArgs& args,
                                         OpStateAccumulator* opAccumulator) {
    if (args.coll->ns() != NamespaceString::kShardSplitDonorsNamespace ||
        tenant_migration_access_blocker::inRecoveryMode(opCtx)) {
        return;
    }

    auto donorStateDoc = parseAndValidateDonorDocument(args.updateArgs->updatedDoc);
    switch (donorStateDoc.getState()) {
        case ShardSplitDonorStateEnum::kBlocking:
            onTransitionToBlocking(opCtx, donorStateDoc);
            break;
        case ShardSplitDonorStateEnum::kCommitted:
        case ShardSplitDonorStateEnum::kAborted:
            opCtx->recoveryUnit()->registerChange(
                std::make_unique<TenantMigrationDonorCommitOrAbortHandler>(donorStateDoc));
            break;
        default:
            uasserted(ErrorCodes::IllegalOperation,
                      "Cannot update donor's state document with state other than 'aborted', "
                      "'committed', or 'aborted'");
    }
}

void ShardSplitDonorOpObserver::aboutToDelete(OperationContext* opCtx,
                                              const CollectionPtr& coll,
                                              BSONObj const& doc) {
    if (coll->ns() != NamespaceString::kShardSplitDonorsNamespace ||
        tenant_migration_access_blocker::inRecoveryMode(opCtx)) {
        return;
    }

    auto donorStateDoc = parseAndValidateDonorDocument(doc);
    const bool shouldRemoveOnRecipient =
        serverless::shouldRemoveStateDocumentOnRecipient(opCtx, donorStateDoc);
    uassert(ErrorCodes::IllegalOperation,
            str::stream() << "cannot delete a donor's state document " << doc
                          << " since it has not been marked as garbage collectable and is not a"
                          << " recipient garbage collectable.",
            donorStateDoc.getExpireAt() || shouldRemoveOnRecipient);

    // To support back-to-back split retries, when a split is aborted, we remove its
    // TenantMigrationDonorAccessBlockers as soon as its donor state doc is marked as garbage
    // collectable. So onDelete should skip removing the TenantMigrationDonorAccessBlockers for
    // aborted splits.
    if (donorStateDoc.getState() != ShardSplitDonorStateEnum::kAborted) {
        splitCleanupDetails(opCtx) =
            boost::make_optional(SplitCleanupDetails{donorStateDoc.getId(), false});
    }

    if (shouldRemoveOnRecipient) {
        splitCleanupDetails(opCtx) =
            boost::make_optional(SplitCleanupDetails{donorStateDoc.getId(), true});
    }
}

void ShardSplitDonorOpObserver::onDelete(OperationContext* opCtx,
                                         const CollectionPtr& coll,
                                         StmtId stmtId,
                                         const OplogDeleteEntryArgs& args,
                                         OpStateAccumulator* opAccumulator) {
    if (coll->ns() != NamespaceString::kShardSplitDonorsNamespace || !splitCleanupDetails(opCtx) ||
        tenant_migration_access_blocker::inRecoveryMode(opCtx)) {
        return;
    }

    opCtx->recoveryUnit()->onCommit([](OperationContext* opCtx, boost::optional<Timestamp>) {
        // Donor access blockers are removed from donor nodes via the shard split op observer.
        // Donor access blockers are removed from recipient nodes when the node applies the
        // recipient config. When the recipient primary steps up it will delete its state
        // document, the call to remove access blockers there will be a no-op.

        const auto migrationId = splitCleanupDetails(opCtx)->migrationId;
        auto& registry = TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext());
        registry.removeAccessBlockersForMigration(
            migrationId, TenantMigrationAccessBlocker::BlockerType::kDonor);

        if (splitCleanupDetails(opCtx)->shouldReleaseLock) {
            ServerlessOperationLockRegistry::get(opCtx->getServiceContext())
                .releaseLock(ServerlessOperationLockRegistry::LockType::kShardSplit, migrationId);
        }
    });
}

repl::OpTime ShardSplitDonorOpObserver::onDropCollection(OperationContext* opCtx,
                                                         const NamespaceString& collectionName,
                                                         const UUID& uuid,
                                                         std::uint64_t numRecords,
                                                         const CollectionDropType dropType) {
    if (collectionName == NamespaceString::kShardSplitDonorsNamespace) {
        opCtx->recoveryUnit()->onCommit([](OperationContext* opCtx, boost::optional<Timestamp>) {
            TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext())
                .removeAll(TenantMigrationAccessBlocker::BlockerType::kDonor);

            ServerlessOperationLockRegistry::get(opCtx->getServiceContext())
                .onDropStateCollection(ServerlessOperationLockRegistry::LockType::kShardSplit);
        });
    }

    return {};
}

void ShardSplitDonorOpObserver::onMajorityCommitPointUpdate(ServiceContext* service,
                                                            const repl::OpTime& newCommitPoint) {
    TenantMigrationAccessBlockerRegistry::get(service).onMajorityCommitPointUpdate(newCommitPoint);
}

}  // namespace mongo
