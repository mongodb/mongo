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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/index_build_entry_helpers.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/db/repl/tenant_migration_state_machine_gen.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/util/str.h"

namespace mongo {
namespace repl {
namespace {
/**
 * Creates the tenant migration recipients collection if it doesn't exist.
 * Note: Throws WriteConflictException if the collection already exist.
 */
CollectionPtr ensureTenantMigrationRecipientsCollectionExists(OperationContext* opCtx,
                                                              Database* db,
                                                              const NamespaceString& nss) {
    // Sanity checks.
    invariant(db);
    invariant(opCtx->lockState()->isCollectionLockedForMode(nss, MODE_IX));

    auto collection = CollectionCatalog::get(opCtx).lookupCollectionByNamespace(opCtx, nss);
    if (!collection) {
        WriteUnitOfWork wuow(opCtx);

        collection = db->createCollection(opCtx, nss, CollectionOptions());
        // Ensure the collection exists.
        invariant(collection);

        wuow.commit();
    }
    return collection;
}

}  // namespace

namespace tenantMigrationRecipientEntryHelpers {

Status insertStateDoc(OperationContext* opCtx, const TenantMigrationRecipientDocument& stateDoc) {
    const auto nss = NamespaceString::kTenantMigrationRecipientsNamespace;
    return writeConflictRetry(opCtx, "insertStateDoc", nss.ns(), [&]() -> Status {
        // TODO SERVER-50741: Should be replaced by AutoGetCollection.
        AutoGetOrCreateDb db(opCtx, nss.db(), MODE_IX);
        Lock::CollectionLock collLock(opCtx, nss, MODE_IX);

        uassert(ErrorCodes::PrimarySteppedDown,
                str::stream() << "No longer primary while attempting to insert tenant migration "
                                 "recipient state document",
                repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, nss));

        // TODO SERVER-50741: ensureTenantMigrationRecipientsCollectionExists() should be removed
        // and this should return ErrorCodes::NamespaceNotFound when collection is missing.
        auto collection = ensureTenantMigrationRecipientsCollectionExists(opCtx, db.getDb(), nss);

        WriteUnitOfWork wuow(opCtx);
        Status status =
            collection->insertDocument(opCtx, InsertStatement(stateDoc.toBSON()), nullptr);
        if (!status.isOK()) {
            return status;
        }
        wuow.commit();
        return Status::OK();
    });
}

Status updateStateDoc(OperationContext* opCtx, const TenantMigrationRecipientDocument& stateDoc) {
    const auto nss = NamespaceString::kTenantMigrationRecipientsNamespace;
    AutoGetCollection collection(opCtx, nss, MODE_IX);

    if (!collection) {
        return Status(ErrorCodes::NamespaceNotFound,
                      str::stream() << nss.ns() << " does not exist");
    }
    auto updateReq = UpdateRequest();
    updateReq.setNamespaceString(nss);
    updateReq.setQuery(BSON("_id" << stateDoc.getId()));
    updateReq.setUpdateModification(
        write_ops::UpdateModification::parseFromClassicUpdate(stateDoc.toBSON()));
    auto updateResult = update(opCtx, collection.getDb(), updateReq);
    if (updateResult.numMatched == 0) {
        return {ErrorCodes::NoSuchKey,
                str::stream() << "Existing Tenant Migration State Document not found for id: "
                              << stateDoc.getId()};
    }
    return Status::OK();
}

StatusWith<TenantMigrationRecipientDocument> getStateDoc(OperationContext* opCtx,
                                                         const UUID& migrationUUID) {
    // Read the most up to date data.
    ReadSourceScope readSourceScope(opCtx, RecoveryUnit::ReadSource::kNoTimestamp);
    AutoGetCollectionForRead collection(opCtx,
                                        NamespaceString::kTenantMigrationRecipientsNamespace);

    if (!collection) {
        return Status(ErrorCodes::NamespaceNotFound,
                      str::stream() << "Collection not found: "
                                    << NamespaceString::kTenantMigrationRecipientsNamespace.ns());
    }

    BSONObj result;
    auto foundDoc = Helpers::findOne(opCtx,
                                     collection.getCollection(),
                                     BSON("_id" << migrationUUID),
                                     result,
                                     /*requireIndex=*/true);
    if (!foundDoc) {
        return Status(ErrorCodes::NoMatchingDocument,
                      str::stream() << "No matching state doc found with tenant migration UUID: "
                                    << migrationUUID);
    }

    try {
        return TenantMigrationRecipientDocument::parse(IDLParserErrorContext("recipientStateDoc"),
                                                       result);
    } catch (DBException& ex) {
        return ex.toStatus(
            str::stream() << "Invalid BSON found for matching document with tenant migration UUID: "
                          << migrationUUID << " , res: " << result);
    }
}

}  // namespace tenantMigrationRecipientEntryHelpers
}  // namespace repl
}  // namespace mongo
