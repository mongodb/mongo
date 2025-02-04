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

#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/shard_id.h"
#include "mongo/s/catalog/type_database_gen.h"

namespace mongo {

/**
 * The ShardDatabaseMetadataClient class provides functionality to manage database metadata stored
 * in the local catalog of a shard. It supports operations for inserting and removing database
 * metadata entries in the shard local catalog.
 *
 * This class ensures that metadata operations are performed on the correct shard, either locally or
 * remotely. It abstracts the complexities of constructing appropriate commands and executing them
 * with proper write concerns.
 */
class ShardDatabaseMetadataClient {
    ShardDatabaseMetadataClient(const ShardDatabaseMetadataClient&) = delete;
    ShardDatabaseMetadataClient& operator=(const ShardDatabaseMetadataClient&) = delete;
    ShardDatabaseMetadataClient(ShardDatabaseMetadataClient&&) = delete;
    ShardDatabaseMetadataClient& operator=(ShardDatabaseMetadataClient&&) = delete;

public:
    ShardDatabaseMetadataClient(OperationContext* opCtx, const ShardId& shardId)
        : _opCtx(opCtx), _shardId(shardId) {}

    /**
     * Inserts a new database entry in the shard local catalog (`config.shard.databases`).
     *
     * If the operation targets the current shard, it is executed locally; otherwise, it is
     * forwarded to the relevant shard.
     *
     * This operation is idempotent. If the database entry already exists with the same data, the
     * operation will have no effect.
     */
    void insert(const DatabaseType& db);

    /**
     * Removes a database entry in the shard local catalog (`config.shard.databases`).
     *
     * If the operation targets the current shard, it is executed locally; otherwise, it is
     * forwarded to the relevant shard.
     */
    void remove(const DatabaseName& dbName);

private:
    OperationContext* const _opCtx;
    const ShardId _shardId;
};

}  // namespace mongo
