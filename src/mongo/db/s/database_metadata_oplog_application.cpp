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

#include "mongo/db/s/database_metadata_oplog_application.h"

#include "mongo/db/db_raii.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/s/type_oplog_catalog_metadata_gen.h"

namespace mongo {

namespace {

void applyOplogEntryToInsertDatabaseMetadata(OperationContext* opCtx, const DatabaseType& db) {
    AutoGetDb autoDb(opCtx, db.getDbName(), MODE_IX);
    auto scopedDss =
        DatabaseShardingState::assertDbLockedAndAcquireExclusive(opCtx, db.getDbName());
    scopedDss->setDbInfo(opCtx, db);
}

void applyOplogEntryToRemoveDatabaseMetadata(OperationContext* opCtx, const DatabaseName& dbName) {
    AutoGetDb autoDb(opCtx, dbName, MODE_IX);
    auto scopedDss = DatabaseShardingState::assertDbLockedAndAcquireExclusive(opCtx, dbName);
    scopedDss->clearDbInfo(opCtx);
}

}  // namespace

void applyCreateDatabaseMetadata(OperationContext* opCtx, const repl::OplogEntry& op) {
    auto oplogEntry = CreateDatabaseMetadataOplogEntry::parse(
        IDLParserContext("OplogCreateDatabaseMetadataOplogEntryyContext"), op.getObject());

    applyOplogEntryToInsertDatabaseMetadata(opCtx, oplogEntry.getDb());
}

void applyDropDatabaseMetadata(OperationContext* opCtx, const repl::OplogEntry& op) {
    auto oplogEntry = DropDatabaseMetadataOplogEntry::parse(
        IDLParserContext("OplogDropDatabaseMetadataOplogEntryContext"), op.getObject());

    applyOplogEntryToRemoveDatabaseMetadata(opCtx, oplogEntry.getDbName());
}

}  // namespace mongo
