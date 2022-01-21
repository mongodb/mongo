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
#include "mongo/db/serverless/shard_split_donor_op_observer.h"
#include "mongo/db/serverless/shard_split_state_machine_gen.h"

namespace mongo {
namespace {

const auto tenantIdsToDeleteDecoration =
    OperationContext::declareDecoration<boost::optional<std::vector<std::string>>>();

ShardSplitDonorDocument parseAndValidateDonorDocument(const BSONObj& doc) {
    auto donorStateDoc =
        ShardSplitDonorDocument::parse(IDLParserErrorContext("donorStateDoc"), doc);
    const std::string errmsg = str::stream() << "invalid donor state doc " << doc;

    switch (donorStateDoc.getState()) {
        case ShardSplitDonorStateEnum::kUninitialized:
        case ShardSplitDonorStateEnum::kDataSync:
            uassert(ErrorCodes::BadValue,
                    errmsg,
                    !donorStateDoc.getBlockTimestamp() && !donorStateDoc.getCommitOrAbortOpTime() &&
                        !donorStateDoc.getAbortReason());
            break;
        case ShardSplitDonorStateEnum::kBlocking:
            uassert(ErrorCodes::BadValue,
                    errmsg,
                    donorStateDoc.getBlockTimestamp() && !donorStateDoc.getCommitOrAbortOpTime() &&
                        !donorStateDoc.getAbortReason());
            break;
        case ShardSplitDonorStateEnum::kCommitted:
            uassert(ErrorCodes::BadValue,
                    errmsg,
                    donorStateDoc.getBlockTimestamp() && donorStateDoc.getCommitOrAbortOpTime() &&
                        !donorStateDoc.getAbortReason());
            break;
        case ShardSplitDonorStateEnum::kAborted:
            uassert(ErrorCodes::BadValue,
                    errmsg,
                    donorStateDoc.getAbortReason() && donorStateDoc.getCommitOrAbortOpTime());
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
void onBlockerInitialization(OperationContext* opCtx,
                             const ShardSplitDonorDocument& donorStateDoc) {
    invariant(donorStateDoc.getState() == ShardSplitDonorStateEnum::kUninitialized);

    auto optionalTenants = donorStateDoc.getTenantIds();
    invariant(optionalTenants);

    for (const auto& tenantId : optionalTenants.get()) {
        auto mtab = std::make_shared<TenantMigrationDonorAccessBlocker>(
            opCtx->getServiceContext(),
            tenantId.toString(),
            MigrationProtocolEnum::kMultitenantMigrations,
            donorStateDoc.getRecipientConnectionString()->toString());

        TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext()).add(tenantId, mtab);

        if (opCtx->writesAreReplicated()) {
            // onRollback is not registered on secondaries since secondaries should not fail to
            // apply the write.
            opCtx->recoveryUnit()->onRollback([opCtx, donorStateDoc, tenant = tenantId.toString()] {
                TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext())
                    .remove(tenant, TenantMigrationAccessBlocker::BlockerType::kDonor);
            });
        }
    }
}

/**
 * Transitions the TenantMigrationDonorAccessBlocker to the blocking state.
 */
void onTransitionToBlocking(OperationContext* opCtx, const ShardSplitDonorDocument& donorStateDoc) {
    invariant(donorStateDoc.getState() == ShardSplitDonorStateEnum::kBlocking);
    invariant(donorStateDoc.getBlockTimestamp());
    invariant(donorStateDoc.getTenantIds());

    auto tenantIds = donorStateDoc.getTenantIds().get();
    for (auto tenantId : tenantIds) {
        auto mtab = tenant_migration_access_blocker::getTenantMigrationDonorAccessBlocker(
            opCtx->getServiceContext(), tenantId);
        invariant(mtab);

        if (!opCtx->writesAreReplicated()) {
            // A primary calls startBlockingWrites on the TenantMigrationDonorAccessBlocker before
            // reserving the OpTime for the "start blocking" write, so only secondaries call
            // startBlockingWrites on the TenantMigrationDonorAccessBlocker in the op observer.
            mtab->startBlockingWrites();
        }

        // Both primaries and secondaries call startBlockingReadsAfter in the op observer, since
        // startBlockingReadsAfter just needs to be called before the "start blocking" write's oplog
        // hole is filled.
        mtab->startBlockingReadsAfter(donorStateDoc.getBlockTimestamp().get());
    }
}

/**
 * Transitions the TenantMigrationDonorAccessBlocker to the committed state.
 */
void onTransitionToCommitted(OperationContext* opCtx,
                             const ShardSplitDonorDocument& donorStateDoc) {
    invariant(donorStateDoc.getState() == ShardSplitDonorStateEnum::kCommitted);
    invariant(donorStateDoc.getCommitOrAbortOpTime());

    auto tenants = donorStateDoc.getTenantIds();
    invariant(tenants);

    for (const auto& tenantId : tenants.get()) {
        auto mtab = tenant_migration_access_blocker::getTenantMigrationDonorAccessBlocker(
            opCtx->getServiceContext(), tenantId);
        invariant(mtab);

        mtab->setCommitOpTime(opCtx, donorStateDoc.getCommitOrAbortOpTime().get());
    }
}

/**
 * Transitions the TenantMigrationDonorAccessBlocker to the aborted state.
 */
void onTransitionToAborted(OperationContext* opCtx, const ShardSplitDonorDocument& donorStateDoc) {
    invariant(donorStateDoc.getState() == ShardSplitDonorStateEnum::kAborted);
    invariant(donorStateDoc.getCommitOrAbortOpTime());

    auto tenants = donorStateDoc.getTenantIds();
    if (!tenants) {
        // The only case where there can be no tenants is when the instance is created by the abort
        // command. In that case, no tenant migration blockers are created and the state will go
        // straight to abort.
        invariant(donorStateDoc.getState() == ShardSplitDonorStateEnum::kUninitialized);
        return;
    }

    for (const auto& tenantId : tenants.get()) {
        auto mtab = tenant_migration_access_blocker::getTenantMigrationDonorAccessBlocker(
            opCtx->getServiceContext(), tenantId);
        invariant(mtab);

        mtab->setAbortOpTime(opCtx, donorStateDoc.getCommitOrAbortOpTime().get());
    }
}

/**
 * Used to update the TenantMigrationDonorAccessBlocker for the migration denoted by the donor's
 * state doc once the write for updating the doc is committed.
 */
class TenantMigrationDonorCommitOrAbortHandler final : public RecoveryUnit::Change {
public:
    TenantMigrationDonorCommitOrAbortHandler(OperationContext* opCtx,
                                             ShardSplitDonorDocument donorStateDoc)
        : _opCtx(opCtx), _donorStateDoc(std::move(donorStateDoc)) {}

    void commit(boost::optional<Timestamp>) override {
        switch (_donorStateDoc.getState()) {
            case ShardSplitDonorStateEnum::kCommitted:
                onTransitionToCommitted(_opCtx, _donorStateDoc);
                break;
            case ShardSplitDonorStateEnum::kAborted:
                onTransitionToAborted(_opCtx, _donorStateDoc);
                break;
            default:
                MONGO_UNREACHABLE;
        }
    }

    void rollback() override {}

private:
    OperationContext* _opCtx;
    const ShardSplitDonorDocument _donorStateDoc;
};

}  // namespace

void ShardSplitDonorOpObserver::onInserts(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          const UUID& uuid,
                                          std::vector<InsertStatement>::const_iterator first,
                                          std::vector<InsertStatement>::const_iterator last,
                                          bool fromMigrate) {
    if (tenant_migration_access_blocker::inRecoveryMode(opCtx) ||
        nss != NamespaceString::kTenantSplitDonorsNamespace) {
        return;
    }

    for (auto it = first; it != last; it++) {
        auto donorStateDoc = parseAndValidateDonorDocument(it->doc);
        switch (donorStateDoc.getState()) {
            case ShardSplitDonorStateEnum::kUninitialized:
                onBlockerInitialization(opCtx, donorStateDoc);
                break;
            case ShardSplitDonorStateEnum::kAborted:
                // If the operation starts aborted, do not do anything.
                break;
            case ShardSplitDonorStateEnum::kDataSync:
            case ShardSplitDonorStateEnum::kBlocking:
            case ShardSplitDonorStateEnum::kCommitted:
                uasserted(ErrorCodes::IllegalOperation,
                          "cannot insert a donor's state doc with 'state' other than 'aborting "
                          "index builds'");
                break;
            default:
                MONGO_UNREACHABLE;
        }
    }
}

void ShardSplitDonorOpObserver::onUpdate(OperationContext* opCtx,
                                         const OplogUpdateEntryArgs& args) {
    if (tenant_migration_access_blocker::inRecoveryMode(opCtx) ||
        args.nss != NamespaceString::kTenantSplitDonorsNamespace) {
        return;
    }

    auto donorStateDoc = parseAndValidateDonorDocument(args.updateArgs->updatedDoc);
    switch (donorStateDoc.getState()) {
        case ShardSplitDonorStateEnum::kDataSync:
            break;
        case ShardSplitDonorStateEnum::kBlocking:
            onTransitionToBlocking(opCtx, donorStateDoc);
            break;
        case ShardSplitDonorStateEnum::kCommitted:
        case ShardSplitDonorStateEnum::kAborted:
            opCtx->recoveryUnit()->registerChange(
                std::make_unique<TenantMigrationDonorCommitOrAbortHandler>(opCtx, donorStateDoc));
            break;
        default:
            MONGO_UNREACHABLE;
    }
}

void ShardSplitDonorOpObserver::aboutToDelete(OperationContext* opCtx,
                                              NamespaceString const& nss,
                                              const UUID& uuid,
                                              BSONObj const& doc) {
    if (tenant_migration_access_blocker::inRecoveryMode(opCtx) ||
        nss != NamespaceString::kTenantSplitDonorsNamespace) {
        return;
    }

    auto donorStateDoc = parseAndValidateDonorDocument(doc);

    // To support back-to-back migration retries, when a migration is aborted, we remove its
    // TenantMigrationDonorAccessBlocker as soon as its donor state doc is marked as garbage
    // collectable. So onDelete should skip removing the TenantMigrationDonorAccessBlocker for
    // aborted migrations.
    tenantIdsToDeleteDecoration(opCtx) =
        donorStateDoc.getState() == ShardSplitDonorStateEnum::kAborted
        ? boost::none
        : [tenantIds = donorStateDoc.getTenantIds()]() {
              std::vector<std::string> tenants;
              for (auto& tenantId : *tenantIds) {
                  tenants.push_back(tenantId.toString());
              }

              return boost::make_optional(tenants);
          }();
}

void ShardSplitDonorOpObserver::onDelete(OperationContext* opCtx,
                                         const NamespaceString& nss,
                                         const UUID& uuid,
                                         StmtId stmtId,
                                         const OplogDeleteEntryArgs& args) {
    if (!tenantIdsToDeleteDecoration(opCtx) ||
        tenant_migration_access_blocker::inRecoveryMode(opCtx) ||
        nss != NamespaceString::kTenantSplitDonorsNamespace) {
        return;
    }

    opCtx->recoveryUnit()->onCommit([opCtx](boost::optional<Timestamp>) {
        auto& registry = TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext());
        for (auto& tenantId : *tenantIdsToDeleteDecoration(opCtx)) {
            registry.remove(tenantId, TenantMigrationAccessBlocker::BlockerType::kDonor);
        }
    });
}

repl::OpTime ShardSplitDonorOpObserver::onDropCollection(OperationContext* opCtx,
                                                         const NamespaceString& collectionName,
                                                         const UUID& uuid,
                                                         std::uint64_t numRecords,
                                                         const CollectionDropType dropType) {
    if (collectionName == NamespaceString::kTenantSplitDonorsNamespace) {
        opCtx->recoveryUnit()->onCommit([opCtx](boost::optional<Timestamp>) {
            TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext())
                .removeAll(TenantMigrationAccessBlocker::BlockerType::kDonor);
        });
    }

    return {};
}

void ShardSplitDonorOpObserver::onMajorityCommitPointUpdate(ServiceContext* service,
                                                            const repl::OpTime& newCommitPoint) {
    TenantMigrationAccessBlockerRegistry::get(service).onMajorityCommitPointUpdate(newCommitPoint);
}

}  // namespace mongo
