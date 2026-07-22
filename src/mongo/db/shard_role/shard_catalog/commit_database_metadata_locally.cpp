// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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

}  // namespace

void writeDatabaseMetadataOplogEntry(OperationContext* opCtx,
                                     repl::MutableOplogEntry& oplogEntry,
                                     std::string_view commandName) {
    repl::OpTime opTime;
    writeConflictRetry(opCtx, commandName, NamespaceString::kRsOplogNamespace, [&] {
        AutoGetOplogFastPath oplogWrite(opCtx, OplogAccessMode::kWrite);
        WriteUnitOfWork wuow(opCtx);
        opTime = repl::logOp(opCtx, &oplogEntry);
        uassert(9980400,
                str::stream() << "Failed to create oplog entry for " << commandName
                              << " with opTime: " << oplogEntry.getOpTime().toString() << ": "
                              << redact(oplogEntry.toBSON()),
                !opTime.isNull());
        wuow.commit();
    });

    // repl::logOp() clears the mutable oplog entry's OpTime before returning so the entry can be
    // reused on a write-conflict retry. Restore the OpTime to the actual value after the oplog
    // entry is successfully committed; otherwise, it will remain 0.
    oplogEntry.setOpTime(opTime);
}

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
