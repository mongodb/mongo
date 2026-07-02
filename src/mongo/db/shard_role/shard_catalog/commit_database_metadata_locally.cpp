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

#include "mongo/db/shard_role/shard_catalog/commit_database_metadata_locally.h"

#include "mongo/db/admission/execution_control/execution_admission_context.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/query/write_ops/write_ops.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/shard_role/lock_manager/exception_util.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/database_sharding_runtime.h"
#include "mongo/db/shard_role/shard_catalog/type_oplog_catalog_metadata_gen.h"
#include "mongo/db/sharding_environment/sharding_statistics.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/version_context.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace shard_catalog_commit {
namespace {

repl::MutableOplogEntry makeDatabaseMetadataOplogEntry(OperationContext* opCtx,
                                                       const DatabaseName& dbName,
                                                       BSONObj oplogObject) {
    repl::MutableOplogEntry oplogEntry;
    oplogEntry.setOpType(repl::OpTypeEnum::kCommand);
    oplogEntry.setVersionContextIfHasOperationFCV(VersionContext::getDecoration(opCtx));
    oplogEntry.setNss(NamespaceString::makeCommandNamespace(dbName));
    oplogEntry.setObject(std::move(oplogObject));
    oplogEntry.setOpTime(OplogSlot());
    oplogEntry.setWallClockTime(opCtx->fastClockSource().now());
    return oplogEntry;
}

void writeDatabaseMetadataOplogEntry(OperationContext* opCtx,
                                     repl::MutableOplogEntry& oplogEntry,
                                     std::string_view commandName) {
    writeConflictRetry(opCtx, commandName, NamespaceString::kRsOplogNamespace, [&] {
        AutoGetOplogFastPath oplogWrite(opCtx, OplogAccessMode::kWrite);
        WriteUnitOfWork wuow(opCtx);
        const repl::OpTime opTime = repl::logOp(opCtx, &oplogEntry);
        uassert(9980400,
                str::stream() << "Failed to create oplog entry for " << commandName
                              << " with opTime: " << oplogEntry.getOpTime().toString() << ": "
                              << redact(oplogEntry.toBSON()),
                !opTime.isNull());
        wuow.commit();
    });
}

}  // namespace

void commitCreateDatabaseMetadataLocally(OperationContext* opCtx,
                                         const DatabaseType& dbMetadata,
                                         bool fromClone) {
    // The shard catalog commit holds the critical section blocking reads and writes, so it must not
    // be deprioritized by execution control.
    admission::execution_control::ScopedTaskTypeNonDeprioritizable deprioGuard(opCtx);

    const auto dbName = dbMetadata.getDbName();
    const auto dbNameStr =
        DatabaseNameUtil::serialize(dbName, SerializationContext::stateDefault());

    LOGV2_DEBUG(12920516,
                1,
                "Committing database metadata to the local shard catalog",
                logAttrs(dbName),
                "dbVersion"_attr = dbMetadata.getVersion(),
                "fromClone"_attr = fromClone);

    // Write to `config.shard.catalog.databases` to insert database metadata.
    {
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

    // Write an oplog 'c' entry to inform secondaries on how to populate the DSS.
    auto oplogEntry = makeDatabaseMetadataOplogEntry(
        opCtx, dbName, CreateDatabaseMetadataOplogEntry{dbNameStr, dbMetadata, fromClone}.toBSON());
    writeDatabaseMetadataOplogEntry(opCtx, oplogEntry, "createDatabaseMetadata");

    // Apply the entry on this (primary) node through the same op observer hook used to apply it
    // on secondaries, so the DSR update and any stale collection metadata clearing stay in sync
    // between the two paths.
    opCtx->getServiceContext()->getOpObserver()->onCreateDatabaseMetadata(
        opCtx, repl::OplogEntry(oplogEntry.toBSON()));

    auto& dbStats = ShardingStatistics::get(opCtx).databaseShardingMetadataStatistics;
    fromClone ? dbStats.registerLocalDatabaseMetadataClone()
              : dbStats.registerLocalDatabaseMetadataCommit();
}

void commitDropDatabaseMetadataLocally(OperationContext* opCtx, const DatabaseName& dbName) {
    // The shard catalog commit holds the critical section blocking reads and writes, so it must not
    // be deprioritized by execution control.
    admission::execution_control::ScopedTaskTypeNonDeprioritizable deprioGuard(opCtx);

    const auto dbNameStr =
        DatabaseNameUtil::serialize(dbName, SerializationContext::stateDefault());

    LOGV2_DEBUG(
        12920517, 1, "Dropping database metadata from the local shard catalog", logAttrs(dbName));

    // Remove database metadata from `config.shard.catalog.databases`.
    {
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

    // Write an oplog 'c' entry to invalidate the DSS on secondaries.
    auto oplogEntry = makeDatabaseMetadataOplogEntry(
        opCtx, dbName, DropDatabaseMetadataOplogEntry{dbNameStr, dbName}.toBSON());
    writeDatabaseMetadataOplogEntry(opCtx, oplogEntry, "dropDatabaseMetadata");

    // Update DSR in primary node.
    {
        auto scopedDsr = DatabaseShardingRuntime::acquireExclusive(opCtx, dbName);
        scopedDsr->clearDbMetadata(opCtx);
    }

    ShardingStatistics::get(opCtx)
        .databaseShardingMetadataStatistics.registerLocalDatabaseMetadataDrop();
}

}  // namespace shard_catalog_commit
}  // namespace mongo
