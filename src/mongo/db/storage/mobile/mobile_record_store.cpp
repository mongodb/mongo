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

#include "mongo/db/storage/mobile/mobile_record_store.h"

#include <memory>
#include <sqlite3.h>

#include "mongo/base/static_assert.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/mobile/mobile_recovery_unit.h"
#include "mongo/db/storage/mobile/mobile_session.h"
#include "mongo/db/storage/mobile/mobile_sqlite_statement.h"
#include "mongo/db/storage/mobile/mobile_util.h"
#include "mongo/db/storage/oplog_hack.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/str.h"

namespace mongo {

class MobileRecordStore::Cursor final : public SeekableRecordCursor {
public:
    Cursor(OperationContext* opCtx,
           const MobileRecordStore& rs,
           const std::string& path,
           const std::string& ident,
           bool forward)
        : _opCtx(opCtx), _forward(forward) {

        MobileSession* session = MobileRecoveryUnit::get(_opCtx)->getSession(_opCtx);
        _stmt = std::make_unique<SqliteStatement>(*session,
                                                  "SELECT rec_id, data from \"",
                                                  ident,
                                                  "\" ",
                                                  "WHERE rec_id ",
                                                  (forward ? ">" : "<"),
                                                  " ? ",
                                                  "ORDER BY rec_id ",
                                                  (forward ? "ASC" : "DESC"),
                                                  ";");

        _startIdNum = (forward ? RecordId::min().repr() : RecordId::max().repr());
        _savedId = RecordId(_startIdNum);

        _stmt->bindInt(0, _savedId.repr());
    }

    boost::optional<Record> next() final {
        if (_eof) {
            return {};
        }

        int status = _stmt->step();
        // Reached end of result rows.
        if (status == SQLITE_DONE) {
            _eof = true;
            _savedId = RecordId(_startIdNum);
            return {};
        }

        // Checks no error was thrown and that step retrieved a row.
        embedded::checkStatus(status, SQLITE_ROW, "_stmt->step() in MobileCursor's next");

        long long recId = _stmt->getColInt(0);
        const void* data = _stmt->getColBlob(1);
        int64_t dataSize = _stmt->getColBytes(1);

        _savedId = RecordId(recId);
        // The data returned from sqlite3_column_blob is only valid until the next call to
        // sqlite3_step. Using getOwned copies the buffer so the data is not invalidated.
        return {{_savedId, RecordData(static_cast<const char*>(data), dataSize).getOwned()}};
    }

    boost::optional<Record> seekExact(const RecordId& id) final {
        // Set the saved position and use save/restore to reprepare the SQL statement so that
        // the cursor restarts at the parameter id.
        int decr = (_forward ? -1 : 1);
        _savedId = RecordId(id.repr() + decr);
        _eof = false;

        save();
        restore();

        boost::optional<Record> rec = next();
        if (rec && rec->id != id) {
            // The record we found isn't the one the caller asked for.
            return boost::none;
        }

        return rec;
    }

    void save() final {
        // SQLite acquires implicit locks over the snapshot this cursor is using. It is important
        // to finalize the corresponding statement to release these locks.
        _stmt->finalize();
    }

    void saveUnpositioned() final {
        save();
        _savedId = RecordId(_startIdNum);
    }

    bool restore() final {
        if (_eof) {
            return true;
        }

        // Obtaining a session starts a read transaction if not done already.
        MobileSession* session = MobileRecoveryUnit::get(_opCtx)->getSession(_opCtx);
        // save() finalized this cursor's SQLite statement. We need to prepare a new statement,
        // before re-positioning it at the saved state.
        _stmt->prepare(*session);

        _stmt->bindInt(0, _savedId.repr());
        return true;
    }

    void detachFromOperationContext() final {
        _opCtx = nullptr;
    }

    void reattachToOperationContext(OperationContext* opCtx) final {
        _opCtx = opCtx;
    }

private:
    /**
     * Resets the prepared statement.
     */
    void _resetStatement() {
        _stmt->reset();
    }

    OperationContext* _opCtx;
    std::unique_ptr<SqliteStatement> _stmt;

    bool _eof = false;

    // Saved location for restoring. RecordId(0) means EOF.
    RecordId _savedId;

    // Default start ID number that is specific to cursor direction.
    int64_t _startIdNum;

    const bool _forward;
};

MobileRecordStore::MobileRecordStore(OperationContext* opCtx,
                                     StringData ns,
                                     const std::string& path,
                                     const std::string& ident,
                                     const CollectionOptions& options)
    : RecordStore(ns), _path(path), _ident(ident) {

    // Mobile SE doesn't support creating an oplog, assert now
    massert(ErrorCodes::IllegalOperation,
            "Replication is not supported by the mobile storage engine",
            !NamespaceString::oplog(ns));

    // Mobile SE doesn't support creating a capped collection, assert now
    massert(ErrorCodes::IllegalOperation,
            "Capped Collections are not supported by the mobile storage engine",
            !options.capped);

    // Determines the nextId to be used for a new record.
    MobileSession* session = MobileRecoveryUnit::get(opCtx)->getSession(opCtx);
    SqliteStatement maxRecIdStmt(*session, "SELECT IFNULL(MAX(rec_id), 0) FROM \"", _ident, "\";");

    maxRecIdStmt.step(SQLITE_ROW);

    long long nextId = maxRecIdStmt.getColInt(0);
    _nextIdNum.store(nextId + 1);
}

const char* MobileRecordStore::name() const {
    return "Mobile";
}

const std::string& MobileRecordStore::getIdent() const {
    return _ident;
}

void MobileRecordStore::_initDataSizeIfNeeded_inlock(OperationContext* opCtx) const {
    if (_isDataSizeInitialized) {
        return;
    }

    MobileSession* session = MobileRecoveryUnit::get(opCtx)->getSession(opCtx);
    SqliteStatement dataSizeStmt(
        *session, "SELECT IFNULL(SUM(LENGTH(data)), 0) FROM \"", _ident, "\";");

    dataSizeStmt.step(SQLITE_ROW);
    int64_t dataSize = dataSizeStmt.getColInt(0);

    _dataSize = dataSize;
    _isDataSizeInitialized = true;
}

long long MobileRecordStore::dataSize(OperationContext* opCtx) const {
    stdx::lock_guard<Latch> lock(_dataSizeMutex);
    _initDataSizeIfNeeded_inlock(opCtx);
    return _dataSize;
}

void MobileRecordStore::_initNumRecsIfNeeded_inlock(OperationContext* opCtx) const {
    if (_isNumRecsInitialized) {
        return;
    }

    MobileSession* session = MobileRecoveryUnit::get(opCtx)->getSession(opCtx);
    SqliteStatement numRecordsStmt(*session, "SELECT COUNT(*) FROM \"", _ident, "\";");

    numRecordsStmt.step(SQLITE_ROW);

    int64_t numRecs = numRecordsStmt.getColInt(0);

    _numRecs = numRecs;
    _isNumRecsInitialized = true;
}

long long MobileRecordStore::numRecords(OperationContext* opCtx) const {
    stdx::lock_guard<Latch> lock(_numRecsMutex);
    _initNumRecsIfNeeded_inlock(opCtx);
    return _numRecs;
}

bool MobileRecordStore::findRecord(OperationContext* opCtx,
                                   const RecordId& recId,
                                   RecordData* rd) const {
    MobileSession* session = MobileRecoveryUnit::get(opCtx)->getSession(opCtx);
    SqliteStatement stmt(*session, "SELECT data FROM \"", _ident, "\" WHERE rec_id = ?;");

    stmt.bindInt(0, recId.repr());

    int status = stmt.step();
    if (status == SQLITE_DONE) {
        return false;
    }
    embedded::checkStatus(status, SQLITE_ROW, "sqlite3_step");

    const void* recData = stmt.getColBlob(0);
    int nBytes = stmt.getColBytes(0);
    *rd = RecordData(static_cast<const char*>(recData), nBytes).getOwned();
    return true;
}

void MobileRecordStore::deleteRecord(OperationContext* opCtx, const RecordId& recId) {
    MobileSession* session = MobileRecoveryUnit::get(opCtx)->getSession(opCtx, false);

    SqliteStatement dataSizeStmt(
        *session, "SELECT IFNULL(LENGTH(data), 0) FROM \"", _ident, "\" WHERE rec_id = ?;");
    dataSizeStmt.bindInt(0, recId.repr());
    dataSizeStmt.step(SQLITE_ROW);

    int64_t dataSizeBefore = dataSizeStmt.getColInt(0);
    _changeNumRecs(opCtx, -1);
    _changeDataSize(opCtx, -dataSizeBefore);

    SqliteStatement deleteStmt(*session, "DELETE FROM \"", _ident, "\" WHERE rec_id = ?;");
    deleteStmt.bindInt(0, recId.repr());
    deleteStmt.step(SQLITE_DONE);
}

Status MobileRecordStore::insertRecords(OperationContext* opCtx,
                                        std::vector<Record>* inOutRecords,
                                        const std::vector<Timestamp>& timestamps) {
    // Inserts record into SQLite table (or replaces if duplicate record id).
    MobileSession* session = MobileRecoveryUnit::get(opCtx)->getSession(opCtx, false);

    SqliteStatement insertStmt(
        *session, "INSERT OR REPLACE INTO \"", _ident, "\"(rec_id, data) VALUES(?, ?);");

    for (auto& record : *inOutRecords) {
        const auto data = record.data.data();
        const auto len = record.data.size();

        _changeNumRecs(opCtx, 1);
        _changeDataSize(opCtx, len);

        RecordId recId = _nextId();
        insertStmt.bindInt(0, recId.repr());
        insertStmt.bindBlob(1, data, len);
        insertStmt.step(SQLITE_DONE);

        record.id = recId;
        insertStmt.reset();
    }

    return Status::OK();
}

Status MobileRecordStore::updateRecord(OperationContext* opCtx,
                                       const RecordId& recId,
                                       const char* data,
                                       int len) {
    MobileSession* session = MobileRecoveryUnit::get(opCtx)->getSession(opCtx, false);

    SqliteStatement dataSizeStmt(
        *session, "SELECT IFNULL(LENGTH(data), 0) FROM \"", _ident, "\" WHERE rec_id = ?;");
    dataSizeStmt.bindInt(0, recId.repr());
    dataSizeStmt.step(SQLITE_ROW);

    int64_t dataSizeBefore = dataSizeStmt.getColInt(0);
    _changeDataSize(opCtx, -dataSizeBefore + len);

    SqliteStatement updateStmt(
        *session, "UPDATE \"", _ident, "\" SET data = ? ", "WHERE rec_id = ?;");
    updateStmt.bindBlob(0, data, len);
    updateStmt.bindInt(1, recId.repr());
    updateStmt.step(SQLITE_DONE);

    return Status::OK();
}

bool MobileRecordStore::updateWithDamagesSupported() const {
    return false;
}

StatusWith<RecordData> MobileRecordStore::updateWithDamages(
    OperationContext* opCtx,
    const RecordId& recId,
    const RecordData& oldRec,
    const char* damageSource,
    const mutablebson::DamageVector& damages) {
    return RecordData();
}

std::unique_ptr<SeekableRecordCursor> MobileRecordStore::getCursor(OperationContext* opCtx,
                                                                   bool forward) const {
    return std::make_unique<Cursor>(opCtx, *this, _path, _ident, forward);
}

/**
 * SQLite does not directly support truncate. The SQLite documentation recommends a DELETE
 * statement without a WHERE clause. A Truncate Optimizer deletes all of the table content
 * without having to visit each row of the table individually.
 */
Status MobileRecordStore::truncate(OperationContext* opCtx) {
    MobileSession* session = MobileRecoveryUnit::get(opCtx)->getSession(opCtx, false);

    int64_t numRecsBefore = numRecords(opCtx);
    _changeNumRecs(opCtx, -numRecsBefore);
    int64_t dataSizeBefore = dataSize(opCtx);
    _changeDataSize(opCtx, -dataSizeBefore);

    SqliteStatement::execQuery(session, "DELETE FROM \"", _ident, "\";");

    return Status::OK();
}

void MobileRecordStore::validate(OperationContext* opCtx,
                                 ValidateResults* results,
                                 BSONObjBuilder* output) {
    embedded::doValidate(opCtx, results);
}

Status MobileRecordStore::touch(OperationContext* opCtx, BSONObjBuilder* output) const {
    return Status(ErrorCodes::CommandNotSupported, "this storage engine does not support touch");
}

/**
 * Note: does not accurately return the size of the table on disk. Instead, it returns the number of
 * bytes used to store the BSON documents.
 */
int64_t MobileRecordStore::storageSize(OperationContext* opCtx,
                                       BSONObjBuilder* extraInfo,
                                       int infoLevel) const {
    return dataSize(opCtx);
}

RecordId MobileRecordStore::_nextId() {
    RecordId out = RecordId(_nextIdNum.fetchAndAdd(1));
    invariant(out.isNormal());
    return out;
}

/**
 * Keeps track of the changes to the number of records.
 */
class MobileRecordStore::NumRecsChange final : public RecoveryUnit::Change {
public:
    NumRecsChange(MobileRecordStore* rs, int64_t diff) : _rs(rs), _diff(diff) {}

    void commit(boost::optional<Timestamp>) override {}

    void rollback() override {
        stdx::lock_guard<Latch> lock(_rs->_numRecsMutex);
        _rs->_numRecs -= _diff;
    }

private:
    MobileRecordStore* _rs;
    int64_t _diff;
};

void MobileRecordStore::_changeNumRecs(OperationContext* opCtx, int64_t diff) {
    stdx::lock_guard<Latch> lock(_numRecsMutex);
    opCtx->recoveryUnit()->registerChange(std::make_unique<NumRecsChange>(this, diff));
    _initNumRecsIfNeeded_inlock(opCtx);
    _numRecs += diff;
}

bool MobileRecordStore::_resetNumRecsIfNeeded(OperationContext* opCtx, int64_t newNumRecs) {
    bool wasReset = false;
    int64_t currNumRecs = numRecords(opCtx);
    if (currNumRecs != newNumRecs) {
        wasReset = true;
        stdx::lock_guard<Latch> lock(_numRecsMutex);
        _numRecs = newNumRecs;
    }
    return wasReset;
}

/**
 * Keeps track of the total data size.
 */
class MobileRecordStore::DataSizeChange final : public RecoveryUnit::Change {
public:
    DataSizeChange(MobileRecordStore* rs, int64_t diff) : _rs(rs), _diff(diff) {}

    void commit(boost::optional<Timestamp>) override {}

    void rollback() override {
        stdx::lock_guard<Latch> lock(_rs->_dataSizeMutex);
        _rs->_dataSize -= _diff;
    }

private:
    MobileRecordStore* _rs;
    int64_t _diff;
};

void MobileRecordStore::_changeDataSize(OperationContext* opCtx, int64_t diff) {
    stdx::lock_guard<Latch> lock(_dataSizeMutex);
    opCtx->recoveryUnit()->registerChange(std::make_unique<DataSizeChange>(this, diff));
    _initDataSizeIfNeeded_inlock(opCtx);
    _dataSize += diff;
}

bool MobileRecordStore::_resetDataSizeIfNeeded(OperationContext* opCtx, int64_t newDataSize) {
    bool wasReset = false;
    int64_t currDataSize = dataSize(opCtx);

    if (currDataSize != _dataSize) {
        wasReset = true;
        stdx::lock_guard<Latch> lock(_dataSizeMutex);
        _dataSize = newDataSize;
    }
    return wasReset;
}

boost::optional<RecordId> MobileRecordStore::oplogStartHack(
    OperationContext* opCtx, const RecordId& startingPosition) const {
    return {};
}

/**
 * Creates a new record store within SQLite.
 * The method is not transactional. Callers are responsible for handling transactional semantics.
 */
void MobileRecordStore::create(OperationContext* opCtx, const std::string& ident) {
    MobileSession* session = MobileRecoveryUnit::get(opCtx)->getSessionNoTxn(opCtx);
    SqliteStatement::execQuery(session,
                               "CREATE TABLE IF NOT EXISTS \"",
                               ident,
                               "\"(rec_id INT, data BLOB, PRIMARY KEY(rec_id));");
}

void MobileRecordStore::updateStatsAfterRepair(OperationContext* opCtx,
                                               long long numRecords,
                                               long long dataSize) {
    _resetDataSizeIfNeeded(opCtx, (int64_t)dataSize);
    _resetNumRecsIfNeeded(opCtx, (int64_t)numRecords);
}

}  // namespace mongo
