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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/tenant_migration_access_blocker_util.h"
#include "mongo/db/repl/tenant_migration_recipient_access_blocker.h"
#include "mongo/db/repl/tenant_migration_recipient_op_observer.h"
#include "mongo/db/repl/tenant_migration_state_machine_gen.h"
#include "mongo/logv2/log.h"

namespace mongo {
namespace repl {

namespace {

const auto tenantIdToDeleteDecoration =
    OperationContext::declareDecoration<boost::optional<std::string>>();

/**
 * Initializes the TenantMigrationRecipientAccessBlocker for the tenant migration denoted by the
 * given state doc.
 */
void createAccessBlockerIfNeeded(OperationContext* opCtx,
                                 const TenantMigrationRecipientDocument& recipientStateDoc) {
    if (tenant_migration_access_blocker::getTenantMigrationRecipientAccessBlocker(
            opCtx->getServiceContext(), recipientStateDoc.getTenantId())) {
        // The migration failed part-way on the recipient with a retryable error, and got retried
        // internally.
        return;
    }

    auto mtab = std::make_shared<TenantMigrationRecipientAccessBlocker>(
        opCtx->getServiceContext(),
        recipientStateDoc.getId(),
        recipientStateDoc.getTenantId().toString(),
        recipientStateDoc.getDonorConnectionString().toString());

    TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext())
        .add(recipientStateDoc.getTenantId(), mtab);
}

/**
 * Transitions the TenantMigrationRecipientAccessBlocker to the rejectBefore state.
 */
void onSetRejectReadsBeforeTimestamp(OperationContext* opCtx,
                                     const TenantMigrationRecipientDocument& recipientStateDoc) {
    invariant(recipientStateDoc.getState() == TenantMigrationRecipientStateEnum::kConsistent);
    invariant(recipientStateDoc.getRejectReadsBeforeTimestamp());

    auto mtab = tenant_migration_access_blocker::getTenantMigrationRecipientAccessBlocker(
        opCtx->getServiceContext(), recipientStateDoc.getTenantId());
    invariant(mtab);

    mtab->startRejectingReadsBefore(recipientStateDoc.getRejectReadsBeforeTimestamp().get());
}

}  // namespace

void TenantMigrationRecipientOpObserver::onUpdate(OperationContext* opCtx,
                                                  const OplogUpdateEntryArgs& args) {
    if (args.nss == NamespaceString::kTenantMigrationRecipientsNamespace &&
        !tenant_migration_access_blocker::inRecoveryMode(opCtx)) {
        auto recipientStateDoc = TenantMigrationRecipientDocument::parse(
            IDLParserErrorContext("recipientStateDoc"), args.updateArgs.updatedDoc);
        opCtx->recoveryUnit()->onCommit([opCtx, recipientStateDoc](boost::optional<Timestamp>) {
            auto mtab = tenant_migration_access_blocker::getTenantMigrationRecipientAccessBlocker(
                opCtx->getServiceContext(), recipientStateDoc.getTenantId());

            if (recipientStateDoc.getExpireAt() && mtab) {
                if (mtab->inStateReject()) {
                    // The TenantMigrationRecipientAccessBlocker entry needs to be removed to
                    // re-allow reads and future migrations with the same tenantId as this migration
                    // has already been aborted and forgotten.
                    TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext())
                        .remove(recipientStateDoc.getTenantId(),
                                TenantMigrationAccessBlocker::BlockerType::kRecipient);
                    return;
                }
                // Once the state doc is marked garbage collectable the TTL deletions should be
                // unblocked.
                mtab->stopBlockingTTL();
            }

            switch (recipientStateDoc.getState()) {
                case TenantMigrationRecipientStateEnum::kUninitialized:
                case TenantMigrationRecipientStateEnum::kDone:
                    break;
                case TenantMigrationRecipientStateEnum::kStarted:
                    createAccessBlockerIfNeeded(opCtx, recipientStateDoc);
                    break;
                case TenantMigrationRecipientStateEnum::kConsistent:
                    if (recipientStateDoc.getRejectReadsBeforeTimestamp()) {
                        onSetRejectReadsBeforeTimestamp(opCtx, recipientStateDoc);
                    }
                    break;
            }
        });
    }
}

void TenantMigrationRecipientOpObserver::aboutToDelete(OperationContext* opCtx,
                                                       NamespaceString const& nss,
                                                       BSONObj const& doc) {
    if (nss == NamespaceString::kTenantMigrationRecipientsNamespace &&
        !tenant_migration_access_blocker::inRecoveryMode(opCtx)) {
        auto recipientStateDoc = TenantMigrationRecipientDocument::parse(
            IDLParserErrorContext("recipientStateDoc"), doc);
        uassert(ErrorCodes::IllegalOperation,
                str::stream() << "cannot delete a recipient's state document " << doc
                              << " since it has not been marked as garbage collectable",
                recipientStateDoc.getExpireAt());

        // TenantMigrationRecipientAccessBlocker is created only after cloning finishes so it
        // would not exist if the state doc is deleted prior to that (e.g. in the case where
        // recipientForgetMigration is received before recipientSyncData).
        auto mtab = tenant_migration_access_blocker::getTenantMigrationRecipientAccessBlocker(
            opCtx->getServiceContext(), recipientStateDoc.getTenantId());
        tenantIdToDeleteDecoration(opCtx) =
            mtab ? boost::make_optional(recipientStateDoc.getTenantId().toString()) : boost::none;
    }
}

void TenantMigrationRecipientOpObserver::onDelete(OperationContext* opCtx,
                                                  const NamespaceString& nss,
                                                  OptionalCollectionUUID uuid,
                                                  StmtId stmtId,
                                                  const OplogDeleteEntryArgs& args) {
    if (nss == NamespaceString::kTenantMigrationRecipientsNamespace &&
        tenantIdToDeleteDecoration(opCtx) &&
        !tenant_migration_access_blocker::inRecoveryMode(opCtx)) {
        opCtx->recoveryUnit()->onCommit([opCtx](boost::optional<Timestamp>) {
            TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext())
                .remove(tenantIdToDeleteDecoration(opCtx).get(),
                        TenantMigrationAccessBlocker::BlockerType::kRecipient);
        });
    }
}

repl::OpTime TenantMigrationRecipientOpObserver::onDropCollection(
    OperationContext* opCtx,
    const NamespaceString& collectionName,
    OptionalCollectionUUID uuid,
    std::uint64_t numRecords,
    const CollectionDropType dropType) {
    if (collectionName == NamespaceString::kTenantMigrationRecipientsNamespace) {
        opCtx->recoveryUnit()->onCommit([opCtx](boost::optional<Timestamp>) {
            TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext())
                .removeAll(TenantMigrationAccessBlocker::BlockerType::kRecipient);
        });
    }
    return {};
}

}  // namespace repl
}  // namespace mongo
