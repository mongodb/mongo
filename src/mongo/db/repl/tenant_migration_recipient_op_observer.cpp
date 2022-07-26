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


#include "mongo/db/repl/tenant_migration_recipient_op_observer.h"

#include <fmt/format.h>

#include "mongo/db/repl/tenant_file_importer_service.h"
#include "mongo/db/repl/tenant_migration_access_blocker_util.h"
#include "mongo/db/repl/tenant_migration_recipient_access_blocker.h"
#include "mongo/db/repl/tenant_migration_recipient_service.h"
#include "mongo/db/repl/tenant_migration_shard_merge_util.h"
#include "mongo/db/repl/tenant_migration_state_machine_gen.h"
#include "mongo/db/repl/tenant_migration_util.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {
namespace repl {
using namespace fmt;
namespace {

// For "multitenant migration" migrations.
const auto tenantIdToDeleteDecoration =
    OperationContext::declareDecoration<boost::optional<std::string>>();
// For "shard merge" migrations.
const auto migrationIdToDeleteDecoration =
    OperationContext::declareDecoration<boost::optional<UUID>>();

/**
 * Initializes the TenantMigrationRecipientAccessBlocker for the tenant migration denoted by the
 * given state doc.
 *
 * TODO (SERVER-64616): Skip for protocol kShardMerge.
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
        recipientStateDoc.getProtocol().value_or(MigrationProtocolEnum::kMultitenantMigrations),
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

    if (recipientStateDoc.getProtocol() == MigrationProtocolEnum::kMultitenantMigrations) {
        auto mtab = tenant_migration_access_blocker::getTenantMigrationRecipientAccessBlocker(
            opCtx->getServiceContext(), recipientStateDoc.getTenantId());
        invariant(mtab);
        mtab->startRejectingReadsBefore(recipientStateDoc.getRejectReadsBeforeTimestamp().get());
    } else {
        tenant_migration_access_blocker::startRejectingReadsBefore(
            opCtx,
            recipientStateDoc.getId(),
            recipientStateDoc.getRejectReadsBeforeTimestamp().get());
    }
}
}  // namespace

void TenantMigrationRecipientOpObserver::onCreateCollection(OperationContext* opCtx,
                                                            const CollectionPtr& coll,
                                                            const NamespaceString& collectionName,
                                                            const CollectionOptions& options,
                                                            const BSONObj& idIndex,
                                                            const OplogSlot& createOpTime,
                                                            bool fromMigrate) {
    if (!shard_merge_utils::isDonatedFilesCollection(collectionName))
        return;

    auto collString = collectionName.coll().toString();
    auto migrationUUID = uassertStatusOK(UUID::parse(collString.substr(collString.find('.') + 1)));
    auto fileClonerTempDirPath = shard_merge_utils::fileClonerTempDir(migrationUUID);

    // This is possible when a secondary restarts or rollback and the donated files collection
    // is created as part of oplog replay.
    if (boost::filesystem::exists(fileClonerTempDirPath)) {
        LOGV2_DEBUG(6113316,
                    1,
                    "File cloner temp directory already exists",
                    "directory"_attr = fileClonerTempDirPath.generic_string());

        // Ignoring the errors because if this step fails, then the following step
        // create_directory() will fail and that will throw an exception.
        boost::system::error_code ec;
        boost::filesystem::remove_all(fileClonerTempDirPath, ec);
    }

    try {
        boost::filesystem::create_directory(fileClonerTempDirPath);
    } catch (std::exception& e) {
        LOGV2_ERROR(6113317,
                    "Error creating file cloner temp directory",
                    "directory"_attr = fileClonerTempDirPath.generic_string(),
                    "error"_attr = e.what());
        throw;
    }
}

void TenantMigrationRecipientOpObserver::onInserts(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const UUID& uuid,
    std::vector<InsertStatement>::const_iterator first,
    std::vector<InsertStatement>::const_iterator last,
    bool fromMigrate) {

    if (!shard_merge_utils::isDonatedFilesCollection(nss)) {
        return;
    }

    auto fileImporter = repl::TenantFileImporterService::get(opCtx->getServiceContext());
    for (auto it = first; it != last; it++) {
        const auto& metadataDoc = it->doc;
        auto migrationId =
            uassertStatusOK(UUID::parse(metadataDoc[shard_merge_utils::kMigrationIdFieldName]));
        fileImporter->learnedFilename(migrationId, metadataDoc);
    }
}

void TenantMigrationRecipientOpObserver::onUpdate(OperationContext* opCtx,
                                                  const OplogUpdateEntryArgs& args) {
    if (args.nss == NamespaceString::kTenantMigrationRecipientsNamespace &&
        !tenant_migration_access_blocker::inRecoveryMode(opCtx)) {
        auto recipientStateDoc = TenantMigrationRecipientDocument::parse(
            IDLParserContext("recipientStateDoc"), args.updateArgs->updatedDoc);
        opCtx->recoveryUnit()->onCommit([opCtx, recipientStateDoc](boost::optional<Timestamp>) {
            auto mtab = tenant_migration_access_blocker::getTenantMigrationRecipientAccessBlocker(
                opCtx->getServiceContext(), recipientStateDoc.getTenantId());

            if (recipientStateDoc.getExpireAt() && mtab) {
                if (mtab->inStateReject()) {
                    // The TenantMigrationRecipientAccessBlocker entry needs to be removed to
                    // re-allow reads and future migrations with the same tenantId as this
                    // migration has already been aborted and forgotten.
                    TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext())
                        .remove(recipientStateDoc.getTenantId(),
                                TenantMigrationAccessBlocker::BlockerType::kRecipient);
                    return;
                }
                // Once the state doc is marked garbage collectable the TTL deletions should be
                // unblocked.
                mtab->stopBlockingTTL();
            }

            auto state = recipientStateDoc.getState();
            auto protocol = recipientStateDoc.getProtocol().value_or(kDefaultMigrationProtocol);
            if (state == TenantMigrationRecipientStateEnum::kLearnedFilenames) {
                tassert(6112900,
                        "Bad state '{}' for protocol '{}'"_format(
                            TenantMigrationRecipientState_serializer(state),
                            MigrationProtocol_serializer(protocol)),
                        protocol == MigrationProtocolEnum::kShardMerge);
            }

            switch (state) {
                case TenantMigrationRecipientStateEnum::kUninitialized:
                    break;
                case TenantMigrationRecipientStateEnum::kStarted:
                    createAccessBlockerIfNeeded(opCtx, recipientStateDoc);
                    break;
                case TenantMigrationRecipientStateEnum::kLearnedFilenames:
                    break;
                case TenantMigrationRecipientStateEnum::kConsistent:
                    if (recipientStateDoc.getRejectReadsBeforeTimestamp()) {
                        onSetRejectReadsBeforeTimestamp(opCtx, recipientStateDoc);
                    }
                    break;
                case TenantMigrationRecipientStateEnum::kDone:
                    break;
            }

            if (protocol != MigrationProtocolEnum::kShardMerge) {
                return;
            }

            if (recipientStateDoc.getExpireAt() && mtab) {
                repl::TenantFileImporterService::get(opCtx->getServiceContext())
                    ->interrupt(recipientStateDoc.getId());
            }

            auto fileImporter = repl::TenantFileImporterService::get(opCtx->getServiceContext());

            switch (state) {
                case TenantMigrationRecipientStateEnum::kUninitialized:
                    break;
                case TenantMigrationRecipientStateEnum::kStarted:
                    fileImporter->startMigration(recipientStateDoc.getId(),
                                                 recipientStateDoc.getDonorConnectionString());
                    break;
                case TenantMigrationRecipientStateEnum::kLearnedFilenames:
                    fileImporter->learnedAllFilenames(recipientStateDoc.getId());
                    break;
                case TenantMigrationRecipientStateEnum::kConsistent:
                    break;
                case TenantMigrationRecipientStateEnum::kDone:
                    break;
            }
        });
    }
}

void TenantMigrationRecipientOpObserver::aboutToDelete(OperationContext* opCtx,
                                                       NamespaceString const& nss,
                                                       const UUID& uuid,
                                                       BSONObj const& doc) {
    if (nss == NamespaceString::kTenantMigrationRecipientsNamespace &&
        !tenant_migration_access_blocker::inRecoveryMode(opCtx)) {
        auto recipientStateDoc =
            TenantMigrationRecipientDocument::parse(IDLParserContext("recipientStateDoc"), doc);
        uassert(ErrorCodes::IllegalOperation,
                str::stream() << "cannot delete a recipient's state document " << doc
                              << " since it has not been marked as garbage collectable",
                recipientStateDoc.getExpireAt());

        // TenantMigrationRecipientAccessBlocker is created at the start of a migration (in this
        // case the recipient state will be kStarted). If the recipient primary receives
        // recipientForgetMigration before receiving recipientSyncData, we set recipient state to
        // kDone in order to avoid creating an unnecessary TenantMigrationRecipientAccessBlocker.
        // In this case, the TenantMigrationRecipientAccessBlocker will not exist for a given
        // tenant.
        if (recipientStateDoc.getProtocol() == MigrationProtocolEnum::kMultitenantMigrations) {
            auto mtab = tenant_migration_access_blocker::getTenantMigrationRecipientAccessBlocker(
                opCtx->getServiceContext(), recipientStateDoc.getTenantId());
            tenantIdToDeleteDecoration(opCtx) = mtab
                ? boost::make_optional(recipientStateDoc.getTenantId().toString())
                : boost::none;
        } else {
            migrationIdToDeleteDecoration(opCtx) = recipientStateDoc.getId();
        }
    }
}

void TenantMigrationRecipientOpObserver::onDelete(OperationContext* opCtx,
                                                  const NamespaceString& nss,
                                                  const UUID& uuid,
                                                  StmtId stmtId,
                                                  const OplogDeleteEntryArgs& args) {
    if (nss == NamespaceString::kTenantMigrationRecipientsNamespace &&
        !tenant_migration_access_blocker::inRecoveryMode(opCtx)) {
        if (tenantIdToDeleteDecoration(opCtx)) {
            auto tenantId = tenantIdToDeleteDecoration(opCtx).get();
            LOGV2_INFO(8423337,
                       "Removing expired 'multitenant migration' migration",
                       "tenantId"_attr = tenantId);
            opCtx->recoveryUnit()->onCommit([opCtx, tenantId](boost::optional<Timestamp>) {
                TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext())
                    .remove(tenantId, TenantMigrationAccessBlocker::BlockerType::kRecipient);
            });
        }

        if (migrationIdToDeleteDecoration(opCtx)) {
            auto migrationId = migrationIdToDeleteDecoration(opCtx).get();
            LOGV2_INFO(6114101,
                       "Removing expired 'shard merge' migration",
                       "migrationId"_attr = migrationId);
            opCtx->recoveryUnit()->onCommit([opCtx, migrationId](boost::optional<Timestamp>) {
                TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext())
                    .removeRecipientAccessBlockersForMigration(migrationId);
                repl::TenantFileImporterService::get(opCtx->getServiceContext())
                    ->interrupt(migrationId);
            });
        }
    }
}

repl::OpTime TenantMigrationRecipientOpObserver::onDropCollection(
    OperationContext* opCtx,
    const NamespaceString& collectionName,
    const UUID& uuid,
    std::uint64_t numRecords,
    const CollectionDropType dropType) {
    if (collectionName == NamespaceString::kTenantMigrationRecipientsNamespace) {
        opCtx->recoveryUnit()->onCommit([opCtx](boost::optional<Timestamp>) {
            TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext())
                .removeAll(TenantMigrationAccessBlocker::BlockerType::kRecipient);

            repl::TenantFileImporterService::get(opCtx->getServiceContext())->interruptAll();
        });
    }
    return {};
}

}  // namespace repl
}  // namespace mongo
