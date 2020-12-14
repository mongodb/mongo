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

#include "mongo/db/commands.h"
#include "mongo/db/commands/tenant_migration_donor_cmds_gen.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/tenant_migration_access_blocker_registry.h"
#include "mongo/db/repl/tenant_migration_conflict_info.h"
#include "mongo/db/repl/tenant_migration_state_machine_gen.h"
#include "mongo/db/request_execution_context.h"
#include "mongo/executor/task_executor.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/util/functional.h"
#include "mongo/util/future.h"

namespace mongo {

namespace tenant_migration_donor {

/**
 * Returns a TenantMigrationDonorDocument constructed from the given bson doc and validate the
 * resulting doc.
 */
TenantMigrationDonorDocument parseDonorStateDocument(const BSONObj& doc);

/**
 * If the operation has read concern "snapshot" or includes afterClusterTime, and the database is
 * in the read blocking state at the given atClusterTime or afterClusterTime or the selected read
 * timestamp, blocks until the migration is committed or aborted.
 * TODO SERVER-53505: Change this to return SharedSemiFuture<TenantMigrationAccessBlocker::State>.
 */
void checkIfCanReadOrBlock(OperationContext* opCtx, StringData dbName);

/**
 * If the operation has read concern "linearizable", throws TenantMigrationCommitted error if the
 * database has been migrated to a different replica set.
 */
void checkIfLinearizableReadWasAllowedOrThrow(OperationContext* opCtx, StringData dbName);

/**
 * Throws TenantMigrationConflict if the database is being migrated and the migration is in the
 * blocking state. Throws TenantMigrationCommitted if it is in committed.
 */
void onWriteToDatabase(OperationContext* opCtx, StringData dbName);

/**
 * Scan config.tenantMigrationDonors and creates the necessary TenantMigrationAccessBlockers for
 * unfinished migrations.
 */
void recoverTenantMigrationAccessBlockers(OperationContext* opCtx);

/**
 * Blocks until the migration commits or aborts, then returns TenantMigrationCommitted or
 * TenantMigrationAborted.
 */
void handleTenantMigrationConflict(OperationContext* opCtx, Status status);

/**
 * Append a no-op to the oplog.
 */
void performNoopWrite(OperationContext* opCtx, StringData msg);

}  // namespace tenant_migration_donor

}  // namespace mongo
