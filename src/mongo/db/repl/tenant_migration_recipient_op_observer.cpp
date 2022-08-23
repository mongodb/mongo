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

#include "mongo/db/multitenancy_gen.h"
#include "mongo/db/repl/tenant_file_importer_service.h"
#include "mongo/db/repl/tenant_migration_access_blocker_util.h"
#include "mongo/db/repl/tenant_migration_decoration.h"
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
        mtab->startRejectingReadsBefore(recipientStateDoc.getRejectReadsBeforeTimestamp().value());
    } else {
        tenant_migration_access_blocker::startRejectingReadsBefore(
            opCtx, recipientStateDoc.getRejectReadsBeforeTimestamp().get());
    }
}

void handleMTMStateChange(OperationContext* opCtx,
                          const TenantMigrationRecipientDocument& recipientStateDoc) {
    auto state = recipientStateDoc.getState();

    switch (state) {
        case TenantMigrationRecipientStateEnum::kUninitialized:
            break;
        case TenantMigrationRecipientStateEnum::kStarted:
            tenant_migration_access_blocker::addTenantMigrationRecipientAccessBlocker(
                opCtx->getServiceContext(),
                recipientStateDoc.getTenantId(),
                recipientStateDoc.getId());
            break;
        case TenantMigrationRecipientStateEnum::kConsistent:
            if (recipientStateDoc.getRejectReadsBeforeTimestamp()) {
                onSetRejectReadsBeforeTimestamp(opCtx, recipientStateDoc);
            }
            break;
        case TenantMigrationRecipientStateEnum::kDone:
            break;
        default:
            MONGO_UNREACHABLE_TASSERT(6112900);
    }
}

void handleShardMergeStateChange(OperationContext* opCtx,
                                 const TenantMigrationRecipientDocument& recipientStateDoc) {
    auto state = recipientStateDoc.getState();

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
            if (recipientStateDoc.getRejectReadsBeforeTimestamp()) {
                onSetRejectReadsBeforeTimestamp(opCtx, recipientStateDoc);
            }
            break;
        case TenantMigrationRecipientStateEnum::kDone:
            break;
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
    if (shard_merge_utils::isDonatedFilesCollection(collectionName)) {
        auto collString = collectionName.coll().toString();
        auto migrationUUID =
            uassertStatusOK(UUID::parse(collString.substr(collString.find('.') + 1)));
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
    } else if (!collectionName.isOnInternalDb()) {
        const auto& recipientInfo = tenantMigrationInfo(opCtx);
        if (!recipientInfo)
            return;

        auto tenantId = tenant_migration_access_blocker::parseTenantIdFromDB(collectionName.db());

        tassert(
            6461602,
            "Unable to determine tenant id from provided database name"_format(collectionName.db()),
            tenantId);

        tenant_migration_access_blocker::addTenantMigrationRecipientAccessBlocker(
            opCtx->getServiceContext(), tenantId.get(), recipientInfo->uuid);
    }
}

void TenantMigrationRecipientOpObserver::onInserts(
    OperationContext* opCtx,
    const CollectionPtr& coll,
    std::vector<InsertStatement>::const_iterator first,
    std::vector<InsertStatement>::const_iterator last,
    bool fromMigrate) {

    if (!shard_merge_utils::isDonatedFilesCollection(coll->ns())) {
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
            if (recipientStateDoc.getExpireAt()) {
                repl::TenantFileImporterService::get(opCtx->getServiceContext())
                    ->interrupt(recipientStateDoc.getId());

                std::vector<std::string> tenantIdsToRemove;
                auto cleanUpBlockerIfGarbage =
                    [&](std::string tenantId, std::shared_ptr<TenantMigrationAccessBlocker>& mtab) {
                        if (recipientStateDoc.getId() != mtab->getMigrationId()) {
                            return;
                        }

                        auto recipientMtab =
                            checked_pointer_cast<TenantMigrationRecipientAccessBlocker>(mtab);
                        if (recipientMtab->inStateReject()) {
                            // The TenantMigrationRecipientAccessBlocker entry needs to be removed
                            // to re-allow reads and future migrations with the same tenantId as
                            // this migration has already been aborted and forgotten.
                            tenantIdsToRemove.push_back(tenantId);
                            return;
                        }
                        // Once the state doc is marked garbage collectable the TTL deletions should
                        // be unblocked.
                        recipientMtab->stopBlockingTTL();
                    };

                TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext())
                    .applyAll(TenantMigrationAccessBlocker::BlockerType::kRecipient,
                              cleanUpBlockerIfGarbage);

                for (const auto& tenantId : tenantIdsToRemove) {
                    // TODO SERVER-68799: Remove TenantMigrationAccessBlocker removal logic.
                    TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext())
                        .remove(tenantId, TenantMigrationAccessBlocker::BlockerType::kRecipient);
                }
            }

            auto protocol = recipientStateDoc.getProtocol().value_or(kDefaultMigrationProtocol);
            switch (protocol) {
                case MigrationProtocolEnum::kMultitenantMigrations:
                    handleMTMStateChange(opCtx, recipientStateDoc);
                    break;
                case MigrationProtocolEnum::kShardMerge:
                    handleShardMergeStateChange(opCtx, recipientStateDoc);
                    break;
                default:
                    MONGO_UNREACHABLE;
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
        tenantMigrationInfo(opCtx) =
            boost::make_optional(TenantMigrationInfo(recipientStateDoc.getId()));
    }
}

void TenantMigrationRecipientOpObserver::onDelete(OperationContext* opCtx,
                                                  const NamespaceString& nss,
                                                  const UUID& uuid,
                                                  StmtId stmtId,
                                                  const OplogDeleteEntryArgs& args) {
    if (nss == NamespaceString::kTenantMigrationRecipientsNamespace &&
        !tenant_migration_access_blocker::inRecoveryMode(opCtx)) {
        auto tmi = tenantMigrationInfo(opCtx);
        if (!tmi) {
            return;
        }

        auto migrationId = tmi->uuid;
        opCtx->recoveryUnit()->onCommit([opCtx, migrationId](boost::optional<Timestamp>) {
            LOGV2_INFO(6114101,
                       "Removing expired migration access blocker",
                       "migrationId"_attr = migrationId);
            repl::TenantFileImporterService::get(opCtx->getServiceContext())
                ->interrupt(migrationId);
            TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext())
                .removeAccessBlockersForMigration(
                    migrationId, TenantMigrationAccessBlocker::BlockerType::kRecipient);
        });
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
            repl::TenantFileImporterService::get(opCtx->getServiceContext())->interruptAll();
            TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext())
                .removeAll(TenantMigrationAccessBlocker::BlockerType::kRecipient);
        });
    }
    return {};
}

}  // namespace repl
}  // namespace mongo
