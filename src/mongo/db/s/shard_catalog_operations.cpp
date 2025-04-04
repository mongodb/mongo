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

#include "mongo/db/s/shard_catalog_operations.h"

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/query/find_command_gen.h"
#include "mongo/db/query/write_ops/delete.h"
#include "mongo/db/shard_role.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/s/catalog/type_database_gen.h"

namespace mongo {

namespace shard_catalog_operations {

std::unique_ptr<DBClientCursor> readAllDatabaseMetadata(OperationContext* opCtx) {
    FindCommandRequest findOp(NamespaceString::kConfigShardCatalogDatabasesNamespace);
    DBDirectClient client(opCtx);

    auto cursor = client.find(std::move(findOp));
    tassert(9813600, "Failed to retrieve cursor while reading database metadata", cursor);

    return cursor;
}

std::unique_ptr<DBClientCursor> readDatabaseMetadata(OperationContext* opCtx,
                                                     const DatabaseName& dbName) {
    DBDirectClient client(opCtx);

    const auto dbNameStr =
        DatabaseNameUtil::serialize(dbName, SerializationContext::stateDefault());

    FindCommandRequest findOp{NamespaceString::kConfigShardCatalogDatabasesNamespace};
    findOp.setFilter(BSON(DatabaseType::kDbNameFieldName << dbNameStr));

    auto cursor = client.find(std::move(findOp));

    tassert(
        10078300,
        str::stream() << "Failed to retrieve cursor while reading database metadata for database: "
                      << dbName.toStringForErrorMsg(),
        cursor);

    return cursor;
}

void insertDatabaseMetadata(OperationContext* opCtx, const DatabaseType& dbMetadata) {
    const auto dbNameStr =
        DatabaseNameUtil::serialize(dbMetadata.getDbName(), SerializationContext::stateDefault());

    write_ops::UpdateCommandRequest updateOp(
        NamespaceString::kConfigShardCatalogDatabasesNamespace);
    updateOp.setUpdates({[&] {
        write_ops::UpdateOpEntry entry;
        entry.setQ(BSON(DatabaseType::kDbNameFieldName << dbNameStr));
        entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(dbMetadata.toBSON()));
        entry.setUpsert(true);
        entry.setMulti(false);
        return entry;
    }()});
    updateOp.setWriteConcern(defaultMajorityWriteConcern());

    DBDirectClient client(opCtx);
    const auto result = client.update(std::move(updateOp));
    write_ops::checkWriteErrors(result);
}

void deleteDatabaseMetadata(OperationContext* opCtx, const DatabaseName& dbName) {
    const auto dbNameStr =
        DatabaseNameUtil::serialize(dbName, SerializationContext::stateDefault());

    write_ops::DeleteCommandRequest deleteOp(
        NamespaceString::kConfigShardCatalogDatabasesNamespace);
    deleteOp.setDeletes({[&] {
        write_ops::DeleteOpEntry entry;
        entry.setQ(BSON(DatabaseType::kDbNameFieldName << dbNameStr));
        entry.setMulti(false);
        return entry;
    }()});
    deleteOp.setWriteConcern(defaultMajorityWriteConcern());

    DBDirectClient client(opCtx);
    const auto result = client.remove(std::move(deleteOp));
    write_ops::checkWriteErrors(result);
}

}  // namespace shard_catalog_operations

}  // namespace mongo
