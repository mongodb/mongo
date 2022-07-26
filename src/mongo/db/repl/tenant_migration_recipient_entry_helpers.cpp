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

#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/db/repl/tenant_migration_recipient_entry_helpers.h"
#include "mongo/db/repl/tenant_migration_state_machine_gen.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {
namespace repl {

namespace tenantMigrationRecipientEntryHelpers {

Status insertStateDoc(OperationContext* opCtx, const TenantMigrationRecipientDocument& stateDoc) {
    const auto nss = NamespaceString::kTenantMigrationRecipientsNamespace;
    AutoGetCollection collection(opCtx, nss, MODE_IX);

    // Sanity check
    uassert(ErrorCodes::PrimarySteppedDown,
            str::stream() << "No longer primary while attempting to insert tenant migration "
                             "recipient state document",
            repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, nss));

    return writeConflictRetry(
        opCtx, "insertTenantMigrationRecipientStateDoc", nss.ns(), [&]() -> Status {
            // Insert the 'stateDoc' if no active tenant migration found for the 'tenantId' provided
            // in the 'stateDoc'. Tenant Migration is considered as active for a tenantId if a state
            // document exists on the disk for that 'tenantId' and not marked to be garbage
            // collected (i.e, 'expireAt' not set).
            const auto filter = BSON(TenantMigrationRecipientDocument::kTenantIdFieldName
                                     << stateDoc.getTenantId().toString()
                                     << TenantMigrationRecipientDocument::kExpireAtFieldName
                                     << BSON("$exists" << false));
            const auto updateMod = BSON("$setOnInsert" << stateDoc.toBSON());
            auto updateResult =
                Helpers::upsert(opCtx, nss, filter, updateMod, /*fromMigrate=*/false);

            // '$setOnInsert' update operator can no way modify the existing on-disk state doc.
            invariant(!updateResult.numDocsModified);
            if (updateResult.upsertedId.isEmpty()) {
                return {ErrorCodes::ConflictingOperationInProgress,
                        str::stream() << "Failed to insert the state doc: "
                                      << tenant_migration_util::redactStateDoc(stateDoc.toBSON())
                                      << "; Found active tenant migration for tenantId: "
                                      << stateDoc.getTenantId()};
            }
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

    return writeConflictRetry(
        opCtx, "updateTenantMigrationRecipientStateDoc", nss.ns(), [&]() -> Status {
            auto updateResult =
                Helpers::upsert(opCtx, nss, stateDoc.toBSON(), /*fromMigrate=*/false);
            if (updateResult.numMatched == 0) {
                return {ErrorCodes::NoSuchKey,
                        str::stream()
                            << "Existing tenant migration state document not found for id: "
                            << stateDoc.getId()};
            }
            return Status::OK();
        });
}

StatusWith<bool> deleteStateDocIfMarkedAsGarbageCollectable(OperationContext* opCtx,
                                                            StringData tenantId) {
    const auto nss = NamespaceString::kTenantMigrationRecipientsNamespace;
    AutoGetCollection collection(opCtx, nss, MODE_IX);

    if (!collection) {
        return Status(ErrorCodes::NamespaceNotFound,
                      str::stream() << nss.ns() << " does not exist");
    }

    auto query = BSON(TenantMigrationRecipientDocument::kTenantIdFieldName
                      << tenantId << TenantMigrationRecipientDocument::kExpireAtFieldName
                      << BSON("$exists" << 1));
    return writeConflictRetry(
        opCtx, "deleteTenantMigrationRecipientStateDoc", nss.ns(), [&]() -> bool {
            auto nDeleted =
                deleteObjects(opCtx, collection.getCollection(), nss, query, true /* justOne */);
            return nDeleted > 0;
        });
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
    auto foundDoc =
        Helpers::findOne(opCtx, collection.getCollection(), BSON("_id" << migrationUUID), result);
    if (!foundDoc) {
        return Status(ErrorCodes::NoMatchingDocument,
                      str::stream() << "No matching state doc found with tenant migration UUID: "
                                    << migrationUUID);
    }

    try {
        return TenantMigrationRecipientDocument::parse(IDLParserContext("recipientStateDoc"),
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
