/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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
#include "mongo/db/repl/tenant_migration_donor_op_observer.h"
#include "mongo/db/repl/tenant_migration_state_machine_gen.h"

namespace mongo {
namespace repl {

namespace {

MONGO_FAIL_POINT_DEFINE(donorOpObserverFailAfterOnInsert);
MONGO_FAIL_POINT_DEFINE(donorOpObserverFailAfterOnUpdate);

const auto tenantIdToDeleteDecoration =
    OperationContext::declareDecoration<boost::optional<std::string>>();

/**
 * Initializes the TenantMigrationDonorAccessBlocker for the tenant migration denoted by the given
 * state doc.
 */
void onTransitionToAbortingIndexBuilds(OperationContext* opCtx,
                                       const TenantMigrationDonorDocument& donorStateDoc) {
    invariant(donorStateDoc.getState() == TenantMigrationDonorStateEnum::kAbortingIndexBuilds);

    auto mtab = std::make_shared<TenantMigrationDonorAccessBlocker>(
        opCtx->getServiceContext(),
        donorStateDoc.getTenantId().toString(),
        donorStateDoc.getRecipientConnectionString().toString());

    TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext())
        .add(donorStateDoc.getTenantId(), mtab);

    if (opCtx->writesAreReplicated()) {
        // onRollback is not registered on secondaries since secondaries should not fail to apply
        // the write.
        opCtx->recoveryUnit()->onRollback([opCtx, donorStateDoc] {
            TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext())
                .remove(donorStateDoc.getTenantId());
        });
    }
}

/**
 * Transitions the TenantMigrationDonorAccessBlocker to the blocking state.
 */
void onTransitionToBlocking(OperationContext* opCtx,
                            const TenantMigrationDonorDocument& donorStateDoc) {
    invariant(donorStateDoc.getState() == TenantMigrationDonorStateEnum::kBlocking);
    invariant(donorStateDoc.getBlockTimestamp());

    auto mtab = tenant_migration_access_blocker::getTenantMigrationDonorAccessBlocker(
        opCtx->getServiceContext(), donorStateDoc.getTenantId());
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

/**
 * Transitions the TenantMigrationDonorAccessBlocker to the committed state.
 */
void onTransitionToCommitted(OperationContext* opCtx,
                             const TenantMigrationDonorDocument& donorStateDoc) {
    invariant(donorStateDoc.getState() == TenantMigrationDonorStateEnum::kCommitted);
    invariant(donorStateDoc.getCommitOrAbortOpTime());

    auto mtab = tenant_migration_access_blocker::getTenantMigrationDonorAccessBlocker(
        opCtx->getServiceContext(), donorStateDoc.getTenantId());
    invariant(mtab);

    mtab->setCommitOpTime(opCtx, donorStateDoc.getCommitOrAbortOpTime().get());
}

/**
 * Transitions the TenantMigrationDonorAccessBlocker to the aborted state.
 */
void onTransitionToAborted(OperationContext* opCtx,
                           const TenantMigrationDonorDocument& donorStateDoc) {
    invariant(donorStateDoc.getState() == TenantMigrationDonorStateEnum::kAborted);
    invariant(donorStateDoc.getCommitOrAbortOpTime());

    auto mtab = tenant_migration_access_blocker::getTenantMigrationDonorAccessBlocker(
        opCtx->getServiceContext(), donorStateDoc.getTenantId());
    invariant(mtab);
    mtab->setAbortOpTime(opCtx, donorStateDoc.getCommitOrAbortOpTime().get());
}

/**
 * Used to update the TenantMigrationDonorAccessBlocker for the migration denoted by the donor's
 * state doc once the write for updating the doc is committed.
 */
class TenantMigrationDonorCommitOrAbortHandler final : public RecoveryUnit::Change {
public:
    TenantMigrationDonorCommitOrAbortHandler(OperationContext* opCtx,
                                             const TenantMigrationDonorDocument donorStateDoc)
        : _opCtx(opCtx), _donorStateDoc(std::move(donorStateDoc)) {}

    void commit(boost::optional<Timestamp>) override {
        if (_donorStateDoc.getExpireAt()) {
            // The TenantMigrationDonorAccessBlocker entry needs to be removed to re-allow writes,
            // reads and future migrations with the same tenantId as this migration has already
            // been aborted and forgotten.
            if (_donorStateDoc.getState() == TenantMigrationDonorStateEnum::kAborted) {
                TenantMigrationAccessBlockerRegistry::get(_opCtx->getServiceContext())
                    .remove(_donorStateDoc.getTenantId());
            }
            return;
        }

        switch (_donorStateDoc.getState()) {
            case TenantMigrationDonorStateEnum::kCommitted:
                onTransitionToCommitted(_opCtx, _donorStateDoc);
                break;
            case TenantMigrationDonorStateEnum::kAborted:
                onTransitionToAborted(_opCtx, _donorStateDoc);
                break;
            default:
                MONGO_UNREACHABLE;
        }
    }

    void rollback() override {}

private:
    OperationContext* _opCtx;
    const TenantMigrationDonorDocument _donorStateDoc;
};

}  // namespace

void TenantMigrationDonorOpObserver::onInserts(OperationContext* opCtx,
                                               const NamespaceString& nss,
                                               OptionalCollectionUUID uuid,
                                               std::vector<InsertStatement>::const_iterator first,
                                               std::vector<InsertStatement>::const_iterator last,
                                               bool fromMigrate) {
    if (nss == NamespaceString::kTenantMigrationDonorsNamespace &&
        !tenant_migration_access_blocker::inRecoveryMode(opCtx)) {
        for (auto it = first; it != last; it++) {
            auto donorStateDoc = tenant_migration_access_blocker::parseDonorStateDocument(it->doc);
            switch (donorStateDoc.getState()) {
                case TenantMigrationDonorStateEnum::kAbortingIndexBuilds:
                    onTransitionToAbortingIndexBuilds(opCtx, donorStateDoc);
                    break;
                case TenantMigrationDonorStateEnum::kDataSync:
                case TenantMigrationDonorStateEnum::kBlocking:
                case TenantMigrationDonorStateEnum::kCommitted:
                case TenantMigrationDonorStateEnum::kAborted:
                    uasserted(ErrorCodes::IllegalOperation,
                              "cannot insert a donor's state doc with 'state' other than 'aborting "
                              "index builds'");
                    break;
                default:
                    MONGO_UNREACHABLE;
            }
        }

        if (MONGO_unlikely(donorOpObserverFailAfterOnInsert.shouldFail())) {
            uasserted(ErrorCodes::InternalError, "fail donor's state doc insert");
        }
    }
}

void TenantMigrationDonorOpObserver::onUpdate(OperationContext* opCtx,
                                              const OplogUpdateEntryArgs& args) {
    if (args.nss == NamespaceString::kTenantMigrationDonorsNamespace &&
        !tenant_migration_access_blocker::inRecoveryMode(opCtx)) {
        auto donorStateDoc =
            tenant_migration_access_blocker::parseDonorStateDocument(args.updateArgs.updatedDoc);
        switch (donorStateDoc.getState()) {
            case TenantMigrationDonorStateEnum::kDataSync:
                break;
            case TenantMigrationDonorStateEnum::kBlocking:
                onTransitionToBlocking(opCtx, donorStateDoc);
                break;
            case TenantMigrationDonorStateEnum::kCommitted:
            case TenantMigrationDonorStateEnum::kAborted:
                opCtx->recoveryUnit()->registerChange(
                    std::make_unique<TenantMigrationDonorCommitOrAbortHandler>(opCtx,
                                                                               donorStateDoc));
                break;
            default:
                MONGO_UNREACHABLE;
        }

        if (MONGO_unlikely(donorOpObserverFailAfterOnUpdate.shouldFail())) {
            uasserted(ErrorCodes::InternalError, "fail donor's state doc update");
        }
    }
}

void TenantMigrationDonorOpObserver::aboutToDelete(OperationContext* opCtx,
                                                   NamespaceString const& nss,
                                                   BSONObj const& doc) {
    if (nss == NamespaceString::kTenantMigrationDonorsNamespace &&
        !tenant_migration_access_blocker::inRecoveryMode(opCtx)) {
        auto donorStateDoc = tenant_migration_access_blocker::parseDonorStateDocument(doc);
        uassert(ErrorCodes::IllegalOperation,
                str::stream() << "cannot delete a donor's state document " << doc
                              << " since it has not been marked as garbage collectable",
                donorStateDoc.getExpireAt());

        // To support back-to-back migration retries, when a migration is aborted, we remove its
        // TenantMigrationDonorAccessBlocker as soon as its donor state doc is marked as garbage
        // collectable. So onDelete should skip removing the TenantMigrationDonorAccessBlocker for
        // aborted migrations.
        tenantIdToDeleteDecoration(opCtx) =
            donorStateDoc.getState() == TenantMigrationDonorStateEnum::kAborted
            ? boost::none
            : boost::make_optional(donorStateDoc.getTenantId().toString());
    }
}

void TenantMigrationDonorOpObserver::onDelete(OperationContext* opCtx,
                                              const NamespaceString& nss,
                                              OptionalCollectionUUID uuid,
                                              StmtId stmtId,
                                              bool fromMigrate,
                                              const boost::optional<BSONObj>& deletedDoc) {
    if (nss == NamespaceString::kTenantMigrationDonorsNamespace &&
        tenantIdToDeleteDecoration(opCtx) &&
        !tenant_migration_access_blocker::inRecoveryMode(opCtx)) {
        opCtx->recoveryUnit()->onCommit([opCtx](boost::optional<Timestamp>) {
            TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext())
                .remove(tenantIdToDeleteDecoration(opCtx).get());
        });
    }
}

void TenantMigrationDonorOpObserver::onMajorityCommitPointUpdate(
    ServiceContext* service, const repl::OpTime& newCommitPoint) {
    TenantMigrationAccessBlockerRegistry::get(service).onMajorityCommitPointUpdate(newCommitPoint);
}

}  // namespace repl
}  // namespace mongo
