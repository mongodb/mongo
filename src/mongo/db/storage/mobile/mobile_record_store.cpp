/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
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
#include "mongo/stdx/memory.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

class MobileRecordStore::Cursor final : public SeekableRecordCursor {
public:
    Cursor(OperationContext* opCtx,
           const MobileRecordStore& rs,
           const std::string& path,
           const std::string& ident,
           bool forward)
        : _opCtx(opCtx), _isCapped(rs.isCapped()), _forward(forward) {

        str::stream cursorQuery;
        cursorQuery << "SELECT rec_id, data from \"" << ident << "\" "
                    << "WHERE rec_id " << (forward ? '>' : '<') << " ? "
                    << "ORDER BY rec_id " << (forward ? "ASC" : "DESC") << ';';

        MobileSession* session = MobileRecoveryUnit::get(_opCtx)->getSession(_opCtx);
        _stmt = stdx::make_unique<SqliteStatement>(*session, cursorQuery);

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
        checkStatus(status, SQLITE_ROW, "_stmt->step() in MobileCursor's next");

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
        return rec;
    }

    void save() final {
        _resetStatement();
    }

    void saveUnpositioned() final {
        save();
        _savedId = RecordId(_startIdNum);
    }

    bool restore() final {
        if (_eof) {
            return true;
        }

        bool isValid = true;

        if (_isCapped) {
            // Check that the cursor's saved position still exists. If not, it has been invalidated
            // by capped deletion.
            RecordId oldPosition = _savedId;
            int decr = _forward ? -1 : 1;
            if (_savedId == RecordId::min() || _savedId == RecordId::max()) {
                decr = 0;
            }

            _resetStatement();
            _stmt->bindInt(0, static_cast<int64_t>(_savedId.repr()) + decr);
            boost::optional<Record> newPosition = next();

            if (!newPosition || newPosition->id != oldPosition) {
                // The cursor's current position was invalidated.
                isValid = false;
            }

            _savedId = oldPosition;
            _eof = false;
        }

        _resetStatement();
        _stmt->bindInt(0, _savedId.repr());

        return isValid;
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

    const bool _isCapped;
    const bool _forward;
};

MobileRecordStore::MobileRecordStore(OperationContext* opCtx,
                                     StringData ns,
                                     const std::string& path,
                                     const std::string& ident,
                                     const CollectionOptions& options)
    : RecordStore(ns),
      _path(path),
      _ident(ident),
      _isOplog(NamespaceString::oplog(ns)),
      _isCapped(options.capped),
      _cappedMaxSize(options.cappedSize > 4096 ? options.cappedSize : 4096),
      _cappedMaxDocs(options.cappedMaxDocs) {

    // Mobile SE doesn't support creating an oplog, assert now
    massert(ErrorCodes::IllegalOperation,
            "Replication is not supported by the mobile storage engine",
            !_isOplog);

    // Determines the nextId to be used for a new record.
    MobileSession* session = MobileRecoveryUnit::get(opCtx)->getSession(opCtx);
    std::string maxRecIdQuery = "SELECT IFNULL(MAX(rec_id), 0) FROM \"" + _ident + "\";";
    SqliteStatement maxRecIdStmt(*session, maxRecIdQuery);

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
    std::string dataSizeQuery = "SELECT IFNULL(SUM(LENGTH(data)), 0) FROM \"" + _ident + "\";";
    SqliteStatement dataSizeStmt(*session, dataSizeQuery);

    dataSizeStmt.step(SQLITE_ROW);
    int64_t dataSize = dataSizeStmt.getColInt(0);

    _dataSize = dataSize;
    _isDataSizeInitialized = true;
}

long long MobileRecordStore::dataSize(OperationContext* opCtx) const {
    stdx::lock_guard<stdx::mutex> lock(_dataSizeMutex);
    _initDataSizeIfNeeded_inlock(opCtx);
    return _dataSize;
}

void MobileRecordStore::_initNumRecsIfNeeded_inlock(OperationContext* opCtx) const {
    if (_isNumRecsInitialized) {
        return;
    }

    MobileSession* session = MobileRecoveryUnit::get(opCtx)->getSession(opCtx);
    std::string numRecordsQuery = "SELECT COUNT(*) FROM \"" + _ident + "\";";
    SqliteStatement numRecordsStmt(*session, numRecordsQuery);

    numRecordsStmt.step(SQLITE_ROW);

    int64_t numRecs = numRecordsStmt.getColInt(0);

    _numRecs = numRecs;
    _isNumRecsInitialized = true;
}

long long MobileRecordStore::numRecords(OperationContext* opCtx) const {
    stdx::lock_guard<stdx::mutex> lock(_numRecsMutex);
    _initNumRecsIfNeeded_inlock(opCtx);
    return _numRecs;
}

RecordData MobileRecordStore::dataFor(OperationContext* opCtx, const RecordId& recId) const {
    RecordData recData;
    bool recFound = findRecord(opCtx, recId, &recData);
    invariant(recFound);
    return recData;
}

bool MobileRecordStore::findRecord(OperationContext* opCtx,
                                   const RecordId& recId,
                                   RecordData* rd) const {
    MobileSession* session = MobileRecoveryUnit::get(opCtx)->getSession(opCtx);
    std::string sqlQuery = "SELECT data FROM \"" + _ident + "\" WHERE rec_id = ?;";
    SqliteStatement stmt(*session, sqlQuery);

    stmt.bindInt(0, recId.repr());

    int status = stmt.step();
    if (status == SQLITE_DONE) {
        return false;
    }
    checkStatus(status, SQLITE_ROW, "sqlite3_step");

    const void* recData = stmt.getColBlob(0);
    int nBytes = stmt.getColBytes(0);
    *rd = RecordData(static_cast<const char*>(recData), nBytes).getOwned();
    return true;
}

void MobileRecordStore::deleteRecord(OperationContext* opCtx, const RecordId& recId) {
    invariant(!_isCapped);

    MobileSession* session = MobileRecoveryUnit::get(opCtx)->getSession(opCtx);
    std::string dataSizeQuery =
        "SELECT IFNULL(LENGTH(data), 0) FROM \"" + _ident + "\" WHERE rec_id = ?;";
    SqliteStatement dataSizeStmt(*session, dataSizeQuery);
    dataSizeStmt.bindInt(0, recId.repr());
    dataSizeStmt.step(SQLITE_ROW);

    int64_t dataSizeBefore = dataSizeStmt.getColInt(0);
    _changeNumRecs(opCtx, -1);
    _changeDataSize(opCtx, -dataSizeBefore);

    std::string deleteQuery = "DELETE FROM \"" + _ident + "\" WHERE rec_id = ?;";
    SqliteStatement deleteStmt(*session, deleteQuery);
    deleteStmt.bindInt(0, recId.repr());
    deleteStmt.step(SQLITE_DONE);
}

bool MobileRecordStore::_isCappedAndNeedsDelete(int64_t numRecs, int64_t numBytes) {
    if (!_isCapped) {
        return false;
    }

    return numBytes > _cappedMaxSize || (_cappedMaxDocs > 0 && numRecs > _cappedMaxDocs);
}

void MobileRecordStore::_notifyCappedCallbackIfNeeded_inlock(OperationContext* opCtx,
                                                             RecordId recId,
                                                             const RecordData& recData) {
    if (!_cappedCallback) {
        return;
    }

    uassertStatusOK(_cappedCallback->aboutToDeleteCapped(opCtx, recId, recData));
}

void MobileRecordStore::_doCappedDelete(OperationContext* opCtx,
                                        SqliteStatement& stmt,
                                        const std::string& direction,
                                        int64_t startRecId) {
    MobileSession* session = MobileRecoveryUnit::get(opCtx)->getSession(opCtx);

    bool isStartRecIdKnown = startRecId;

    int64_t numRecs = numRecords(opCtx), numBytes = dataSize(opCtx);
    int64_t numRecsRemoved = 0, numBytesRemoved = 0;
    {
        stdx::lock_guard<stdx::mutex> cappedCallbackLock(_cappedCallbackMutex);

        // Update how many records and how many bytes of data are about to be removed. Notify the
        // capped callback of each record that will be deleted.
        while ((isStartRecIdKnown && stmt.step() == SQLITE_ROW) ||
               (!isStartRecIdKnown &&
                _isCappedAndNeedsDelete(numRecs - numRecsRemoved, numBytes - numBytesRemoved) &&
                stmt.step() == SQLITE_ROW)) {
            int64_t id = stmt.getColInt(0);
            if (!isStartRecIdKnown) {
                startRecId = id;
            }

            const void* data = stmt.getColBlob(1);
            int64_t size = stmt.getColBytes(1);

            numRecsRemoved++;
            numBytesRemoved += size;

            _notifyCappedCallbackIfNeeded_inlock(
                opCtx, RecordId(id), RecordData(static_cast<const char*>(data), size));
        }
    }

    WriteUnitOfWork wuow(opCtx);

    _changeNumRecs(opCtx, -numRecsRemoved);
    _changeDataSize(opCtx, -numBytesRemoved);

    str::stream cappedDeleteQuery;
    cappedDeleteQuery << "DELETE FROM \"" << _ident << "\" WHERE rec_id " << direction << " ?;";
    SqliteStatement cappedDeleteStmt(*session, cappedDeleteQuery);
    cappedDeleteStmt.bindInt(0, startRecId);
    cappedDeleteStmt.step(SQLITE_DONE);

    wuow.commit();
}

void MobileRecordStore::_cappedDeleteIfNeeded(OperationContext* opCtx) {
    if (!_isCappedAndNeedsDelete(numRecords(opCtx), dataSize(opCtx))) {
        return;
    }

    MobileSession* session = MobileRecoveryUnit::get(opCtx)->getSession(opCtx);

    std::string recordRemovalQuery =
        "SELECT rec_id, data FROM \"" + _ident + "\" ORDER BY rec_id ASC;";
    SqliteStatement recordRemovalStmt(*session, recordRemovalQuery);

    _doCappedDelete(opCtx, recordRemovalStmt, "<=");
}

StatusWith<RecordId> MobileRecordStore::insertRecord(
    OperationContext* opCtx, const char* data, int len, Timestamp, bool enforceQuota) {
    // Inserts record into SQLite table (or replaces if duplicate record id).
    MobileSession* session = MobileRecoveryUnit::get(opCtx)->getSession(opCtx);

    if (_isCapped && len > _cappedMaxSize) {
        return Status(ErrorCodes::BadValue, "object to insert exceeds cappedMaxSize");
    }

    _changeNumRecs(opCtx, 1);
    _changeDataSize(opCtx, len);

    std::string insertQuery =
        "INSERT OR REPLACE INTO \"" + _ident + "\"(rec_id, data) VALUES(?, ?);";
    SqliteStatement insertStmt(*session, insertQuery);
    RecordId recId = _nextId();
    insertStmt.bindInt(0, recId.repr());
    insertStmt.bindBlob(1, data, len);
    insertStmt.step(SQLITE_DONE);

    _cappedDeleteIfNeeded(opCtx);

    return StatusWith<RecordId>(recId);
}

Status MobileRecordStore::insertRecordsWithDocWriter(OperationContext* opCtx,
                                                     const DocWriter* const* docs,
                                                     const Timestamp* timestamps,
                                                     size_t nDocs,
                                                     RecordId* idsOut) {
    // Calculates the total size of the data buffer.
    size_t totalSize = 0;
    for (size_t i = 0; i < nDocs; i++) {
        totalSize += docs[i]->documentSize();
    }

    std::unique_ptr<char[]> buffer(new char[totalSize]);
    char* pos = buffer.get();
    for (size_t i = 0; i < nDocs; i++) {
        docs[i]->writeDocument(pos);
        size_t docLen = docs[i]->documentSize();
        StatusWith<RecordId> res = insertRecord(opCtx, pos, docLen, timestamps[i], true);
        idsOut[i] = res.getValue();
        pos += docLen;
    }

    return Status::OK();
}

Status MobileRecordStore::updateRecord(OperationContext* opCtx,
                                       const RecordId& recId,
                                       const char* data,
                                       int len,
                                       bool enforceQuota,
                                       UpdateNotifier* notifier) {
    MobileSession* session = MobileRecoveryUnit::get(opCtx)->getSession(opCtx);
    std::string dataSizeQuery =
        "SELECT IFNULL(LENGTH(data), 0) FROM \"" + _ident + "\" WHERE rec_id = ?;";
    SqliteStatement dataSizeStmt(*session, dataSizeQuery);
    dataSizeStmt.bindInt(0, recId.repr());
    dataSizeStmt.step(SQLITE_ROW);

    int64_t dataSizeBefore = dataSizeStmt.getColInt(0);
    if (_isCapped && dataSizeBefore != len) {
        return Status(ErrorCodes::IllegalOperation, "cannot change the size of a document");
    }
    _changeDataSize(opCtx, -dataSizeBefore + len);

    if (notifier) {
        fassert(37054, notifier->recordStoreGoingToUpdateInPlace(opCtx, recId));
    }

    std::string updateQuery = "UPDATE \"" + _ident + "\" SET data = ? " + "WHERE rec_id = ?;";
    SqliteStatement updateStmt(*session, updateQuery);
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
    return stdx::make_unique<Cursor>(opCtx, *this, _path, _ident, forward);
}

/**
 * SQLite does not directly support truncate. The SQLite documentation recommends dropping then
 * recreating the table rather than deleting all the contents of a table.
 */
Status MobileRecordStore::truncate(OperationContext* opCtx) {
    MobileSession* session = MobileRecoveryUnit::get(opCtx)->getSession(opCtx);

    int64_t numRecsBefore = numRecords(opCtx);
    _changeNumRecs(opCtx, -numRecsBefore);
    int64_t dataSizeBefore = dataSize(opCtx);
    _changeDataSize(opCtx, -dataSizeBefore);

    std::string dropQuery = "DROP TABLE \"" + _ident + "\";";
    SqliteStatement::execQuery(session, dropQuery);
    MobileRecordStore::create(opCtx, _ident);

    return Status::OK();
}

/**
 * The method throws an assertion if the capped truncate results in an emptied table.
 */
void MobileRecordStore::cappedTruncateAfter(OperationContext* opCtx, RecordId end, bool inclusive) {
    MobileSession* session = MobileRecoveryUnit::get(opCtx)->getSession(opCtx);

    // Check that the table will not be empty after performing deletes.
    str::stream recordsRemainingQuery;
    recordsRemainingQuery << "SELECT * FROM \"" << _ident << "\" "
                          << "WHERE rec_id " << (inclusive ? "< " : "<= ") << end.repr()
                          << " LIMIT 1;";

    SqliteStatement recordsRemainingStmt(*session, recordsRemainingQuery);
    int status = recordsRemainingStmt.step();
    if (status == SQLITE_DONE) {
        massert(
            37003, str::stream() << "Cannot perform cappedTruncateAfter with end " << end, false);
    }
    checkStatus(status, SQLITE_ROW, "sqlite3_step");

    str::stream recordsRemovedQuery;
    recordsRemovedQuery << "SELECT rec_id, data FROM \"" << _ident << "\" "
                        << "WHERE rec_id " << (inclusive ? ">= " : "> ") << end.repr();
    SqliteStatement recordsRemovedStmt(*session, recordsRemovedQuery);

    _doCappedDelete(opCtx, recordsRemovedStmt, (inclusive ? ">=" : ">"), end.repr());
}

Status MobileRecordStore::compact(OperationContext* opCtx,
                                  RecordStoreCompactAdaptor* adaptor,
                                  const CompactOptions* options,
                                  CompactStats* stats) {
    return Status::OK();
}

/**
 * Note: on full validation, this validates the entire database file, not just the table used by
 * this record store.
 */
Status MobileRecordStore::validate(OperationContext* opCtx,
                                   ValidateCmdLevel level,
                                   ValidateAdaptor* adaptor,
                                   ValidateResults* results,
                                   BSONObjBuilder* output) {
    if (level == kValidateFull) {
        doValidate(opCtx, results);
    }

    if (!results->valid) {
        // The database was corrupt, so return without checking the table.
        return Status::OK();
    }

    MobileSession* session = MobileRecoveryUnit::get(opCtx)->getSession(opCtx);
    try {
        std::string selectQuery = "SELECT rec_id, data FROM \"" + _ident + "\";";
        SqliteStatement selectStmt(*session, selectQuery);

        int interruptInterval = 4096;
        long long actualNumRecs = 0;
        long long actualDataSize = 0;
        long long numInvalidRecs = 0;

        int status;
        while ((status = selectStmt.step()) == SQLITE_ROW) {
            if (!(actualNumRecs % interruptInterval)) {
                opCtx->checkForInterrupt();
            }

            long long id = selectStmt.getColInt(0);
            const void* data = selectStmt.getColBlob(1);
            int dataSize = selectStmt.getColBytes(1);

            ++actualNumRecs;
            actualDataSize += dataSize;

            RecordId recId(id);
            RecordData recData(reinterpret_cast<const char*>(data), dataSize);

            size_t validatedSize;
            Status status = adaptor->validate(recId, recData, &validatedSize);

            if (!status.isOK() || validatedSize != static_cast<size_t>(dataSize)) {
                if (results->valid) {
                    std::string errMsg = "detected one or more invalid documents";
                    validateLogAndAppendError(results, errMsg);
                }

                ++numInvalidRecs;
                log() << "document at location " << recId << " is corrupted";
            }
        }

        if (status == SQLITE_CORRUPT) {
            uasserted(ErrorCodes::UnknownError, sqlite3_errstr(status));
        }
        checkStatus(status, SQLITE_DONE, "sqlite3_step");

        // Verify that _numRecs and _dataSize are accurate.
        int64_t cachedNumRecs = numRecords(opCtx);
        if (_resetNumRecsIfNeeded(opCtx, actualNumRecs)) {
            str::stream errMsg;
            errMsg << "cached number of records does not match actual number of records - ";
            errMsg << "cached number of records = " << cachedNumRecs << "; ";
            errMsg << "actual number of records = " << actualNumRecs;
            validateLogAndAppendError(results, errMsg);
        }
        int64_t cachedDataSize = dataSize(opCtx);
        if (_resetDataSizeIfNeeded(opCtx, actualDataSize)) {
            str::stream errMsg;
            errMsg << "cached data size does not match actual data size - ";
            errMsg << "cached data size = " << cachedDataSize << "; ";
            errMsg << "actual data size = " << actualDataSize;
            validateLogAndAppendError(results, errMsg);
        }

        if (level == kValidateFull) {
            output->append("nInvalidDocuments", numInvalidRecs);
        }
        output->appendNumber("nrecords", actualNumRecs);

    } catch (const DBException& e) {
        std::string errMsg = "record store is corrupt, could not read documents - " + e.toString();
        validateLogAndAppendError(results, errMsg);
    }

    return Status::OK();
}

void MobileRecordStore::appendCustomStats(OperationContext* opCtx,
                                          BSONObjBuilder* result,
                                          double scale) const {
    result->appendBool("capped", _isCapped);
    if (_isCapped) {
        result->appendIntOrLL("max", _cappedMaxDocs);
        result->appendIntOrLL("maxSize", static_cast<long long>(_cappedMaxSize / scale));
    }
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
        stdx::lock_guard<stdx::mutex> lock(_rs->_numRecsMutex);
        _rs->_numRecs -= _diff;
    }

private:
    MobileRecordStore* _rs;
    int64_t _diff;
};

void MobileRecordStore::_changeNumRecs(OperationContext* opCtx, int64_t diff) {
    stdx::lock_guard<stdx::mutex> lock(_numRecsMutex);
    opCtx->recoveryUnit()->registerChange(new NumRecsChange(this, diff));
    _initNumRecsIfNeeded_inlock(opCtx);
    _numRecs += diff;
}

bool MobileRecordStore::_resetNumRecsIfNeeded(OperationContext* opCtx, int64_t newNumRecs) {
    bool wasReset = false;
    int64_t currNumRecs = numRecords(opCtx);
    if (currNumRecs != newNumRecs) {
        wasReset = true;
        stdx::lock_guard<stdx::mutex> lock(_numRecsMutex);
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
        stdx::lock_guard<stdx::mutex> lock(_rs->_dataSizeMutex);
        _rs->_dataSize -= _diff;
    }

private:
    MobileRecordStore* _rs;
    int64_t _diff;
};

void MobileRecordStore::_changeDataSize(OperationContext* opCtx, int64_t diff) {
    stdx::lock_guard<stdx::mutex> lock(_dataSizeMutex);
    opCtx->recoveryUnit()->registerChange(new DataSizeChange(this, diff));
    _initDataSizeIfNeeded_inlock(opCtx);
    _dataSize += diff;
}

bool MobileRecordStore::_resetDataSizeIfNeeded(OperationContext* opCtx, int64_t newDataSize) {
    bool wasReset = false;
    int64_t currDataSize = dataSize(opCtx);

    if (currDataSize != _dataSize) {
        wasReset = true;
        stdx::lock_guard<stdx::mutex> lock(_dataSizeMutex);
        _dataSize = newDataSize;
    }
    return wasReset;
}

Status MobileRecordStore::updateCappedSize(OperationContext* opCtx, long long cappedSize) {
    _cappedMaxSize = cappedSize;
    return Status::OK();
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
    std::string sqlQuery =
        "CREATE TABLE IF NOT EXISTS \"" + ident + "\"(rec_id INT, data BLOB, PRIMARY KEY(rec_id));";
    SqliteStatement::execQuery(session, sqlQuery);
}

}  // namespace mongo
