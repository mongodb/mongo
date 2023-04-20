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

#include "mongo/db/operation_context.h"
#include "mongo/db/repl/tenant_migration_access_blocker_registry.h"
#include "mongo/db/repl/tenant_migration_conflict_info.h"
#include "mongo/db/repl/tenant_migration_donor_access_blocker.h"
#include "mongo/db/repl/tenant_migration_recipient_access_blocker.h"
#include "mongo/db/repl/tenant_migration_state_machine_gen.h"

namespace mongo {

namespace tenant_migration_access_blocker {

std::shared_ptr<TenantMigrationDonorAccessBlocker> getDonorAccessBlockerForMigration(
    ServiceContext* serviceContext, const UUID& migrationId);

std::shared_ptr<TenantMigrationRecipientAccessBlocker> getRecipientAccessBlockerForMigration(
    ServiceContext* serviceContext, const UUID& migrationId);

std::shared_ptr<TenantMigrationRecipientAccessBlocker> getTenantMigrationRecipientAccessBlocker(
    ServiceContext* serviceContext, StringData tenantId);

void fassertOnUnsafeInitialSync(const UUID& migrationId);

/**
 * Add an access blocker if one does not already exist.
 */
void addTenantMigrationRecipientAccessBlocker(ServiceContext* serviceContext,
                                              const StringData& tenantId,
                                              const UUID& migrationId);

/**
 * Parse the tenantId from a database name, or return boost::none if there is no tenantId.
 */
boost::optional<std::string> parseTenantIdFromDB(const DatabaseName& dbName);

/**
 * Validates that the tenant that owns nss belongs to the migration identified by migrationId. The
 * function throws ErrorCodes::InvalidTenant if there is no tenantId or if the tenant is not being
 * migrated by the expected migration.
 */
void validateNssIsBeingMigrated(const boost::optional<TenantId>& tenantId,
                                const NamespaceString& nss,
                                const UUID& migrationId);

/**
 * Returns a TenantMigrationDonorDocument constructed from the given bson doc and validate the
 * resulting doc.
 */
TenantMigrationDonorDocument parseDonorStateDocument(const BSONObj& doc);

/**
 * Checks if a request is allowed to read based on the tenant migration states of this node as a
 * donor or as a recipient. TenantMigrationCommitted is returned if the request needs to be
 * re-routed to the new owner of the tenant. If the tenant is currently being migrated and the
 * request needs to block, a future for when the request is unblocked is returned, and the promise
 * will be set for the returned future when the migration is committed or aborted. Note: for better
 * performance, check if the future is immediately ready.
 */
SemiFuture<void> checkIfCanReadOrBlock(OperationContext* opCtx,
                                       const DatabaseName& dbName,
                                       const OpMsgRequest& request);

/**
 * If the operation has read concern "linearizable", throws TenantMigrationCommitted error if the
 * database has been migrated to a different replica set.
 */
void checkIfLinearizableReadWasAllowedOrThrow(OperationContext* opCtx, const DatabaseName& dbName);

/**
 * Throws TenantMigrationConflict if the database is being migrated and the migration is in the
 * blocking state. Throws TenantMigrationCommitted if it is in committed.
 */
void checkIfCanWriteOrThrow(OperationContext* opCtx, const DatabaseName& dbName, Timestamp writeTs);

/**
 * Returns TenantMigrationConflict if the database is being migrated (even if migration is not yet
 * in the blocking state). Returns TenantMigrationCommitted if it is in committed.
 */
Status checkIfCanBuildIndex(OperationContext* opCtx, const DatabaseName& dbName);

/**
 * Returns true if there is either a donor or recipient access blocker for the given dbName.
 */
bool hasActiveTenantMigration(OperationContext* opCtx, const DatabaseName& dbName);

/**
 * Scan config.tenantMigrationDonors and creates the necessary TenantMigrationAccessBlockers for
 * unfinished migrations.
 */
void recoverTenantMigrationAccessBlockers(OperationContext* opCtx);

/**
 * Blocks until the migration commits or aborts, then returns TenantMigrationCommitted or
 * TenantMigrationAborted, or a non-retryable error if the given status is
 * NonRetryableTenantMigrationConflict.
 */
Status handleTenantMigrationConflict(OperationContext* opCtx, Status status);

/**
 * Appends a no-op to the oplog.
 */
void performNoopWrite(OperationContext* opCtx, StringData msg);

/**
 * Returns true if the node is in startup recovery, initial sync or rollback. If the node is any
 * of these mode, the TenantMigrationAccessBlocker will be recovered outside of the OpObserver
 * by tenant_migration_access_blocker::recoverTenantMigrationAccessBlockers.
 */
bool inRecoveryMode(OperationContext* opCtx);

/*
 * Returns true if a read should be excluded from access blocker filtering.
 */
bool shouldExcludeRead(OperationContext* opCtx);

/**
 * Parse the 'TenantId' from the provided DatabaseName.
 */
boost::optional<TenantId> parseTenantIdFromDatabaseName(const DatabaseName& dbName);

/**
 * Retrieves the 'tenant id' from the provided DatabaseName.
 */
boost::optional<std::string> extractTenantFromDatabaseName(const DatabaseName& dbName);

}  // namespace tenant_migration_access_blocker

}  // namespace mongo
