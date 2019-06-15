/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/system/error_code.hpp>
#include <memory>
#include <vector>

#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/lock_state.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/storage/mobile/mobile_index.h"
#include "mongo/db/storage/mobile/mobile_kv_engine.h"
#include "mongo/db/storage/mobile/mobile_record_store.h"
#include "mongo/db/storage/mobile/mobile_recovery_unit.h"
#include "mongo/db/storage/mobile/mobile_session.h"
#include "mongo/db/storage/mobile/mobile_sqlite_statement.h"
#include "mongo/db/storage/mobile/mobile_util.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace {
int64_t queryPragmaInt(const MobileSession& session, StringData pragma) {
    SqliteStatement stmt(session, "PRAGMA ", pragma, ";");
    stmt.step(SQLITE_ROW);
    return stmt.getColInt(0);
}

std::string queryPragmaStr(const MobileSession& session, StringData pragma) {
    SqliteStatement stmt(session, "PRAGMA ", pragma, ";");
    stmt.step(SQLITE_ROW);
    return stmt.getColText(0);
}
}  // namespace

MobileKVEngine::MobileKVEngine(const std::string& path,
                               const embedded::MobileOptions& options,
                               ServiceContext* serviceContext)
    : _options(options) {
    _initDBPath(path);

    _sessionPool.reset(new MobileSessionPool(_path, _options));

    // getSession only needs a valid opCtx if the pool has no available sessions and we need to wait
    // for one. But as this is init code and we've just initialized the pool that should not happen.
    // Passing nullptr as opCtx so we avoid creating one here.
    auto session = _sessionPool->getSession(nullptr);

    fassert(37001, queryPragmaStr(*session, "journal_mode"_sd) == "wal");
    LOG(MOBILE_LOG_LEVEL_LOW) << "MobileSE: Confirmed SQLite database opened in WAL mode";

    fassert(50869, queryPragmaInt(*session, "synchronous"_sd) == _options.durabilityLevel);
    LOG(MOBILE_LOG_LEVEL_LOW) << "MobileSE: Confirmed SQLite database has synchronous set to: "
                              << _options.durabilityLevel;

    fassert(50868, queryPragmaInt(*session, "fullfsync"_sd) == 1);
    LOG(MOBILE_LOG_LEVEL_LOW) << "MobileSE: Confirmed SQLite database is set to fsync with "
                                 "F_FULLFSYNC if the platform supports it (currently only darwin "
                                 "kernels). Value: 1";

    if (!_options.disableVacuumJob) {
        _vacuumJob = serviceContext->getPeriodicRunner()->makeJob(
            PeriodicRunner::PeriodicJob("SQLiteVacuumJob",
                                        [this](Client* client) {
                                            if (!client->getServiceContext()->getStorageEngine())
                                                return;
                                            maybeVacuum(client, Date_t::max());
                                        },
                                        Minutes(options.vacuumCheckIntervalMinutes)));
        _vacuumJob->start();
    }
}

void MobileKVEngine::cleanShutdown() {
    try {
        if (!_options.disableVacuumJob)
            maybeVacuum(Client::getCurrent(), Date_t());
    } catch (const std::exception& e) {
        LOG(MOBILE_LOG_LEVEL_LOW)
            << "MobileSE: Exception while doing vacuum at shutdown, surpressing. " << e.what();
    }
}

void MobileKVEngine::maybeVacuum(Client* client, Date_t deadline) {
    ServiceContext::UniqueOperationContext opCtxUPtr;
    OperationContext* opCtx = client->getOperationContext();
    if (!opCtx) {
        opCtxUPtr = client->makeOperationContext();
        opCtx = opCtxUPtr.get();
    }

    std::unique_ptr<MobileSession> session;
    int64_t pageCount;
    int64_t freelistCount;
    {
        // There may be other threads doing write operations that has locked the database in
        // exclusive mode. Grab an S lock here to ensure that isn't the case while we get the
        // session and query the pragmas.
        Lock::GlobalLock lk(opCtx, MODE_S, deadline, Lock::InterruptBehavior::kThrow);
        if (!lk.isLocked())
            return;

        session = _sessionPool->getSession(opCtx);
        pageCount = queryPragmaInt(*session, "page_count"_sd);
        freelistCount = queryPragmaInt(*session, "freelist_count"_sd);
    }

    constexpr int kPageSize = 4096;  // SQLite default
    LOG(MOBILE_LOG_LEVEL_LOW) << "MobileSE: Evaluating if we need to vacuum. page_count = "
                              << pageCount << ", freelist_count = " << freelistCount;
    if ((pageCount > 0 && (float)freelistCount / pageCount >= _options.vacuumFreePageRatio) ||
        (freelistCount * kPageSize >= _options.vacuumFreeSizeMB * 1024 * 1024)) {
        LOG(MOBILE_LOG_LEVEL_LOW) << "MobileSE: Performing incremental vacuum";
        // Data will we moved on the file system, take an exclusive lock
        Lock::GlobalLock lk(opCtx, MODE_X, deadline, Lock::InterruptBehavior::kThrow);
        if (!lk.isLocked())
            return;

        SqliteStatement::execQuery(session.get(), "PRAGMA incremental_vacuum;");
    }
}

void MobileKVEngine::_initDBPath(const std::string& path) {
    boost::system::error_code err;
    boost::filesystem::path dbPath(path);

    if (!boost::filesystem::exists(dbPath, err)) {
        if (err) {
            uasserted(4085, err.message());
        }
        std::string errMsg("DB path not found: ");
        errMsg += dbPath.generic_string();
        uasserted(4086, errMsg);

    } else if (!boost::filesystem::is_directory(dbPath, err)) {
        if (err) {
            uasserted(4087, err.message());
        }
        std::string errMsg("DB path is not a valid directory: ");
        errMsg += dbPath.generic_string();
        uasserted(4088, errMsg);
    }

    dbPath /= "mobile.sqlite";

    if (boost::filesystem::exists(dbPath, err)) {
        if (err) {
            uasserted(4089, err.message());
        } else if (!boost::filesystem::is_regular_file(dbPath)) {
            std::string errMsg("Failed to open " + dbPath.generic_string() +
                               ": not a regular file");
            uasserted(4090, errMsg);
        }
    }
    _path = dbPath.generic_string();
}

RecoveryUnit* MobileKVEngine::newRecoveryUnit() {
    return new MobileRecoveryUnit(_sessionPool.get());
}

Status MobileKVEngine::createRecordStore(OperationContext* opCtx,
                                         StringData ns,
                                         StringData ident,
                                         const CollectionOptions& options) {
    // TODO: eventually will support file renaming but otherwise do not use collection options.

    // Mobile SE doesn't support creating an oplog
    if (NamespaceString::oplog(ns)) {
        return Status(ErrorCodes::InvalidOptions,
                      "Replication is not supported by the mobile storage engine");
    }

    // Mobile doesn't support capped collections
    if (options.capped) {
        return Status(ErrorCodes::InvalidOptions,
                      "Capped collections are not supported by the mobile storage engine");
    }

    MobileRecordStore::create(opCtx, ident.toString());
    return Status::OK();
}

std::unique_ptr<RecordStore> MobileKVEngine::getRecordStore(OperationContext* opCtx,
                                                            StringData ns,
                                                            StringData ident,
                                                            const CollectionOptions& options) {
    return stdx::make_unique<MobileRecordStore>(opCtx, ns, _path, ident.toString(), options);
}

std::unique_ptr<RecordStore> MobileKVEngine::makeTemporaryRecordStore(OperationContext* opCtx,
                                                                      StringData ident) {
    MobileRecordStore::create(opCtx, ident.toString());
    return std::make_unique<MobileRecordStore>(
        opCtx, "", _path, ident.toString(), CollectionOptions());
}


Status MobileKVEngine::createSortedDataInterface(OperationContext* opCtx,
                                                 const CollectionOptions& collOptions,
                                                 StringData ident,
                                                 const IndexDescriptor* desc) {
    return MobileIndex::create(opCtx, ident.toString());
}

SortedDataInterface* MobileKVEngine::getSortedDataInterface(OperationContext* opCtx,
                                                            StringData ident,
                                                            const IndexDescriptor* desc) {
    if (desc->unique()) {
        return new MobileIndexUnique(opCtx, desc, ident.toString());
    }
    return new MobileIndexStandard(opCtx, desc, ident.toString());
}

Status MobileKVEngine::dropIdent(OperationContext* opCtx, StringData ident) {
    MobileSession* session = MobileRecoveryUnit::get(opCtx)->getSessionNoTxn(opCtx);
    std::string dropQuery = "DROP TABLE IF EXISTS \"" + ident + "\";";

    try {
        SqliteStatement::execQuery(session, dropQuery);
    } catch (const WriteConflictException&) {
        // It is possible that this drop fails because of transaction running in parallel.
        // We pretend that it succeeded, queue it for now and keep retrying later.
        LOG(MOBILE_LOG_LEVEL_LOW)
            << "MobileSE: Caught WriteConflictException while dropping table, "
               "queuing to retry later";
        MobileRecoveryUnit::get(opCtx)->enqueueFailedDrop(dropQuery);
    }
    return Status::OK();
}

/**
 * Note: this counts the total number of bytes in the key and value columns, not the actual
 * number of bytes on disk used by this ident.
 */
int64_t MobileKVEngine::getIdentSize(OperationContext* opCtx, StringData ident) {
    MobileSession* session = MobileRecoveryUnit::get(opCtx)->getSession(opCtx);

    // Get key-value column names.
    SqliteStatement colNameStmt(*session, "PRAGMA table_info(\"", ident, "\")");

    colNameStmt.step(SQLITE_ROW);
    std::string keyColName(colNameStmt.getColText(1));
    colNameStmt.step(SQLITE_ROW);
    std::string valueColName(colNameStmt.getColText(1));
    colNameStmt.step(SQLITE_DONE);

    // Get total data size of key-value columns.
    SqliteStatement dataSizeStmt(*session,
                                 "SELECT IFNULL(SUM(LENGTH(",
                                 keyColName,
                                 ")), 0) + ",
                                 "IFNULL(SUM(LENGTH(",
                                 valueColName,
                                 ")), 0) FROM \"",
                                 ident,
                                 "\";");

    dataSizeStmt.step(SQLITE_ROW);
    return dataSizeStmt.getColInt(0);
}

bool MobileKVEngine::hasIdent(OperationContext* opCtx, StringData ident) const {
    MobileSession* session = MobileRecoveryUnit::get(opCtx)->getSession(opCtx);

    SqliteStatement findTableStmt(*session,
                                  "SELECT * FROM sqlite_master WHERE type='table' AND name = ?;");
    findTableStmt.bindText(0, ident.rawData(), ident.size());

    int status = findTableStmt.step();
    if (status == SQLITE_DONE) {
        return false;
    }
    embedded::checkStatus(status, SQLITE_ROW, "sqlite3_step");

    return true;
}

std::vector<std::string> MobileKVEngine::getAllIdents(OperationContext* opCtx) const {
    std::vector<std::string> idents;
    MobileSession* session = MobileRecoveryUnit::get(opCtx)->getSession(opCtx);

    SqliteStatement getTablesStmt(*session, "SELECT name FROM sqlite_master WHERE type='table';");

    int status;
    while ((status = getTablesStmt.step()) == SQLITE_ROW) {
        std::string tableName(getTablesStmt.getColText(0));
        idents.push_back(tableName);
    }
    embedded::checkStatus(status, SQLITE_DONE, "sqlite3_step");
    return idents;
}

}  // namespace mongo
