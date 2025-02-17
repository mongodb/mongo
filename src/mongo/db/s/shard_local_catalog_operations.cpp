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

#include "mongo/db/s/shard_local_catalog_operations.h"

#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/query/write_ops/delete.h"

namespace mongo {

namespace {

CollectionAcquisition acquireConfigShardDatabasesCollection(OperationContext* opCtx) {
    return acquireCollection(
        opCtx,
        CollectionAcquisitionRequest(NamespaceString::kConfigShardDatabasesNamespace,
                                     PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                     repl::ReadConcernArgs::kLocal,
                                     AcquisitionPrerequisites::kWrite),
        MODE_IX);
}

void insertDatabaseMetadataEntry(OperationContext* opCtx,
                                 const CollectionAcquisition& collection,
                                 const BSONObj& doc) {
    uassertStatusOK(Helpers::insert(opCtx, collection, doc));
}

void deleteDatabaseMetadataEntry(OperationContext* opCtx,
                                 const CollectionAcquisition& collection,
                                 const DatabaseName& dbName) {
    const auto dbNameStr =
        DatabaseNameUtil::serialize(dbName, SerializationContext::stateDefault());

    deleteObjects(
        opCtx, collection, BSON(DatabaseType::kDbNameFieldName << dbNameStr), true /* justOne */);
}

bool existsDatabaseMetadata(OperationContext* opCtx, const DatabaseName& dbName) {
    const auto dbNameStr =
        DatabaseNameUtil::serialize(dbName, SerializationContext::stateDefault());

    BSONObj res;
    return Helpers::findById(opCtx,
                             NamespaceString::kConfigShardDatabasesNamespace,
                             BSON(DatabaseType::kDbNameFieldName << dbNameStr),
                             res);
}

}  // namespace

namespace shard_local_catalog_operations {

void insertDatabaseMetadata(OperationContext* opCtx, const DatabaseType& db) {
    auto coll = acquireConfigShardDatabasesCollection(opCtx);

    if (coll.exists() && existsDatabaseMetadata(opCtx, db.getDbName())) {
        return;
    }

    WriteUnitOfWork wuow(opCtx, WriteUnitOfWork::kGroupForTransaction);

    if (!coll.exists()) {
        ScopedLocalCatalogWriteFence scopedLocalCatalogWriteFence(opCtx, &coll);
        auto db = DatabaseHolder::get(opCtx)->openDb(opCtx, coll.nss().dbName());
        db->createCollection(opCtx, coll.nss());
    }

    invariant(coll.exists());

    // Perform writes to update the database metadata to a durable collection.
    insertDatabaseMetadataEntry(opCtx, coll, db.toBSON());

    wuow.commit();
}

void removeDatabaseMetadata(OperationContext* opCtx, const DatabaseName& dbName) {
    auto coll = acquireConfigShardDatabasesCollection(opCtx);

    // This method is based on the assumption that previous database metadata exists, which implies
    // that the authoritative collection should also be present. If that collection is not found, it
    // indicates an inconsistency in the metadata. In other words, there is a database that was not
    // registered in the shard-local catalog.
    invariant(coll.exists());

    if (!existsDatabaseMetadata(opCtx, dbName)) {
        return;
    }

    WriteUnitOfWork wuow(opCtx, WriteUnitOfWork::kGroupForTransaction);

    // Perform deletes to remove the database metadata from the durable collection.
    deleteDatabaseMetadataEntry(opCtx, coll, dbName);

    wuow.commit();
}

std::unique_ptr<DBClientCursor> readAllDatabaseMetadata(OperationContext* opCtx) {
    FindCommandRequest findOp(NamespaceString::kConfigShardDatabasesNamespace);
    DBDirectClient client(opCtx);

    auto cursor = client.find(std::move(findOp));
    tassert(9813600, "Failed to retrieve cursor", cursor);

    return cursor;
}

}  // namespace shard_local_catalog_operations

}  // namespace mongo
