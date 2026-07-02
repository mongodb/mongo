/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/type_database_gen.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"

namespace mongo {

namespace MONGO_MOD_PARENT_PRIVATE shard_catalog_commit {

/**
 * Persists the database metadata into the shard catalog (config.shard.catalog.databases), writes an
 * oplog 'c' entry to inform secondaries on how to populate the DatabaseShardingState (DSS), and
 * applies that same entry locally on this (primary) node via the op observer's
 * onCreateDatabaseMetadata hook.
 */
void commitCreateDatabaseMetadataLocally(OperationContext* opCtx,
                                         const DatabaseType& dbMetadata,
                                         bool fromClone = false);

/**
 * Deletes the database metadata from the shard catalog (config.shard.catalog.databases), writes an
 * oplog 'c' entry to invalidate the DatabaseShardingState (DSS) on secondaries, and clears the
 * in-memory DatabaseShardingRuntime (DSR) on this (primary) node.
 */
void commitDropDatabaseMetadataLocally(OperationContext* opCtx, const DatabaseName& dbName);

}  // namespace MONGO_MOD_PARENT_PRIVATE shard_catalog_commit
}  // namespace mongo
