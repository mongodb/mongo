/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/s/catalog/type_database_gen.h"

namespace mongo {

namespace shard_local_catalog_operations {

/**
 * Adds database metadata to the shard-local catalog.
 *
 * This function inserts the database metadata into the replicated durable collection
 * `config.shard.databases`.
 */
void insertDatabaseMetadata(OperationContext* opCtx, const DatabaseType& db);

/**
 * Deletes database metadata from the shard-local catalog.
 *
 * This function removes the database metadata from the replicated durable collection
 * `config.shard.databases`.
 */
void removeDatabaseMetadata(OperationContext* opCtx, const DatabaseName& dbName);

/**
 * Read database metadata from all databases in the shard-local catalog.
 *
 * This function reads the database metadata from the replicated durable collection
 * `config.shard.databases`.
 */
std::unique_ptr<DBClientCursor> readAllDatabaseMetadata(OperationContext* opCtx);

}  // namespace shard_local_catalog_operations

}  // namespace mongo
