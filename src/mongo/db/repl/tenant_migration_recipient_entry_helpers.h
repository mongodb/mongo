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

#pragma once

namespace mongo {

class OperationContext;
class TenantMigrationRecipientDocument;
class Status;
class UUID;
template <typename T>
class StatusWith;

namespace repl {
namespace tenantMigrationRecipientEntryHelpers {

/**
 * Writes the state doc to the disk.
 *
 * Returns 'DuplicateKey' error code if a document already exists on the disk with the same
 * 'migrationUUID'.
 */
Status insertStateDoc(OperationContext* opCtx, const TenantMigrationRecipientDocument& stateDoc);

/**
 * Updates the state doc in the database.
 *
 * Returns 'NoSuchKey' error code if no state document already exists on the disk with the same
 * 'migrationUUID'.
 */
Status updateStateDoc(OperationContext* opCtx, const TenantMigrationRecipientDocument& stateDoc);

/**
 * Returns the state doc matching the document with 'migrationUUID' from the disk if it
 * exists. Reads at "no" timestamp i.e, reading with the "latest" snapshot reflecting up to date
 * data.
 *
 * If the stored state doc on disk contains invalid BSON, the 'InvalidBSON' error code is
 * returned.
 *
 * Returns 'NoMatchingDocument' error code if no document with 'migrationUUID' is found.
 */
StatusWith<TenantMigrationRecipientDocument> getStateDoc(OperationContext* opCtx,
                                                         const UUID& migrationUUID);

}  // namespace tenantMigrationRecipientEntryHelpers
}  // namespace repl
}  // namespace mongo
