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

#include "mongo/platform/basic.h"

#include <sqlite3.h>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/db/storage/mobile/mobile_index.h"
#include "mongo/db/storage/mobile/mobile_recovery_unit.h"
#include "mongo/db/storage/mobile/mobile_sqlite_statement.h"
#include "mongo/db/storage/mobile/mobile_util.h"

namespace mongo {
namespace {

using std::shared_ptr;
using std::string;
using std::vector;

// BTree stuff

const int TempKeyMaxSize = 1024;  // This goes away with SERVER-3372.

bool hasFieldNames(const BSONObj& obj) {
    BSONForEach(e, obj) {
        if (e.fieldName()[0])
            return true;
    }
    return false;
}

BSONObj stripFieldNames(const BSONObj& query) {
    if (!hasFieldNames(query))
        return query;

    BSONObjBuilder bb;
    BSONForEach(e, query) {
        bb.appendAs(e, StringData());
    }
    return bb.obj();
}

}  // namespace

MobileIndex::MobileIndex(OperationContext* opCtx,
                         const IndexDescriptor* desc,
                         const std::string& ident)
    : _isUnique(desc->unique()), _ordering(Ordering::make(desc->keyPattern())), _ident(ident) {}

MobileIndex::MobileIndex(bool isUnique, const Ordering& ordering, const std::string& ident)
    : _isUnique(isUnique), _ordering(ordering), _ident(ident) {}

Status MobileIndex::insert(OperationContext* opCtx,
                           const BSONObj& key,
                           const RecordId& recId,
                           bool dupsAllowed) {
    invariant(recId.isNormal());
    invariant(!hasFieldNames(key));

    Status status = _checkKeySize(key);
    if (!status.isOK()) {
        return status;
    }

    return _insert(opCtx, key, recId, dupsAllowed);
}

template <typename ValueType>
Status MobileIndex::doInsert(OperationContext* opCtx,
                             const KeyString& key,
                             const ValueType& value,
                             bool isTransactional) {
    MobileSession* session;
    if (isTransactional) {
        session = MobileRecoveryUnit::get(opCtx)->getSession(opCtx, false);
    } else {
        session = MobileRecoveryUnit::get(opCtx)->getSessionNoTxn(opCtx);
    }

    std::string insertQuery = "INSERT INTO \"" + _ident + "\" (key, value) VALUES (?, ?);";
    SqliteStatement insertStmt(*session, insertQuery);

    insertStmt.bindBlob(0, key.getBuffer(), key.getSize());
    insertStmt.bindBlob(1, value.getBuffer(), value.getSize());

    int status = insertStmt.step();
    if (status == SQLITE_CONSTRAINT) {
        insertStmt.setExceptionStatus(status);
        if (isUnique()) {
            // Return error if duplicate key inserted in a unique index.
            BSONObj bson =
                KeyString::toBson(key.getBuffer(), key.getSize(), _ordering, key.getTypeBits());
            return _dupKeyError(bson);
        } else {
            // A record with same key could already be present in a standard index, that is OK. This
            // can happen when building a background index while documents are being written in
            // parallel.
            return Status::OK();
        }
    }
    checkStatus(status, SQLITE_DONE, "sqlite3_step");

    return Status::OK();
}

void MobileIndex::unindex(OperationContext* opCtx,
                          const BSONObj& key,
                          const RecordId& recId,
                          bool dupsAllowed) {
    invariant(recId.isNormal());
    invariant(!hasFieldNames(key));

    return _unindex(opCtx, key, recId, dupsAllowed);
}

void MobileIndex::_doDelete(OperationContext* opCtx, const KeyString& key, KeyString* value) {
    MobileSession* session = MobileRecoveryUnit::get(opCtx)->getSession(opCtx, false);

    str::stream deleteQuery;
    deleteQuery << "DELETE FROM \"" << _ident << "\" WHERE key = ?";
    if (value) {
        deleteQuery << " AND value = ?";
    }
    deleteQuery << ";";
    SqliteStatement deleteStmt(*session, deleteQuery);

    deleteStmt.bindBlob(0, key.getBuffer(), key.getSize());
    if (value) {
        deleteStmt.bindBlob(1, value->getBuffer(), value->getSize());
    }
    deleteStmt.step(SQLITE_DONE);
}

/**
 * Note: this validates the entire database file, not just the table used by this index.
 */
void MobileIndex::fullValidate(OperationContext* opCtx,
                               long long* numKeysOut,
                               ValidateResults* fullResults) const {
    if (fullResults) {
        doValidate(opCtx, fullResults);
        if (!fullResults->valid) {
            return;
        }
    }
    if (numKeysOut) {
        *numKeysOut = numEntries(opCtx);
    }
}

bool MobileIndex::appendCustomStats(OperationContext* opCtx,
                                    BSONObjBuilder* output,
                                    double scale) const {
    return true;
}

long long MobileIndex::getSpaceUsedBytes(OperationContext* opCtx) const {
    MobileSession* session = MobileRecoveryUnit::get(opCtx)->getSession(opCtx);

    // Sum the number of bytes in each column.
    // SQLite aggregate functions return null if the column is empty or has only nulls, so return 0
    // bytes if there is no data in the column.
    str::stream sizeQuery;
    sizeQuery << "SELECT IFNULL(SUM(LENGTH(key)), 0) + "
              << "IFNULL(SUM(LENGTH(value)), 0) FROM \"" << _ident + "\";";
    SqliteStatement sizeStmt(*session, sizeQuery);

    sizeStmt.step(SQLITE_ROW);

    long long dataSize = sizeStmt.getColInt(0);
    return dataSize;
}

long long MobileIndex::numEntries(OperationContext* opCtx) const {
    MobileSession* session = MobileRecoveryUnit::get(opCtx)->getSession(opCtx);
    std::string countQuery = "SELECT COUNT(*) FROM \"" + _ident + "\";";
    SqliteStatement countStmt(*session, countQuery);

    countStmt.step(SQLITE_ROW);
    long long numRecs = countStmt.getColInt(0);
    return numRecs;
}

bool MobileIndex::isEmpty(OperationContext* opCtx) {
    MobileSession* session = MobileRecoveryUnit::get(opCtx)->getSession(opCtx);
    std::string emptyCheckQuery = "SELECT * FROM \"" + _ident + "\" LIMIT 1;";
    SqliteStatement emptyCheckStmt(*session, emptyCheckQuery);

    int status = emptyCheckStmt.step();
    if (status == SQLITE_DONE) {
        return true;
    }
    checkStatus(status, SQLITE_ROW, "sqlite3_step");
    return false;
}

Status MobileIndex::initAsEmpty(OperationContext* opCtx) {
    // No-op.
    return Status::OK();
}

Status MobileIndex::create(OperationContext* opCtx, const std::string& ident) {
    MobileSession* session = MobileRecoveryUnit::get(opCtx)->getSessionNoTxn(opCtx);
    std::string createTableQuery =
        "CREATE TABLE \"" + ident + "\"(key BLOB PRIMARY KEY, value BLOB);";
    SqliteStatement createTableStmt(*session, createTableQuery.c_str());

    createTableStmt.step(SQLITE_DONE);
    return Status::OK();
}

Status MobileIndex::dupKeyCheck(OperationContext* opCtx,
                                const BSONObj& key,
                                const RecordId& recId) {
    invariant(!hasFieldNames(key));
    invariant(_isUnique);

    if (_isDup(opCtx, key, recId))
        return _dupKeyError(key);
    return Status::OK();
}

bool MobileIndex::_isDup(OperationContext* opCtx, const BSONObj& key, RecordId recId) {
    MobileSession* session = MobileRecoveryUnit::get(opCtx)->getSession(opCtx);
    std::string dupCheckQuery = "SELECT value FROM \"" + _ident + "\" WHERE key = ?;";
    SqliteStatement dupCheckStmt(*session, dupCheckQuery);

    KeyString keyStr(_keyStringVersion, key, _ordering);

    dupCheckStmt.bindBlob(0, keyStr.getBuffer(), keyStr.getSize());

    bool isEntryFound = false;

    // If the key exists, check if we already have this record id at this key. If so, we don't
    // consider that to be a dup.
    int status;
    while ((status = dupCheckStmt.step()) == SQLITE_ROW) {
        const void* value = dupCheckStmt.getColBlob(0);
        int64_t size = dupCheckStmt.getColBytes(0);

        isEntryFound = true;

        BufReader br(value, size);
        if (KeyString::decodeRecordId(&br) == recId) {
            return false;
        }
    }
    checkStatus(status, SQLITE_DONE, "sqlite3_step");

    return isEntryFound;
}

Status MobileIndex::_dupKeyError(const BSONObj& key) {
    StringBuilder sb;
    sb << "E11000 duplicate key error ";
    sb << "index: " << _ident << " ";
    sb << "dup key: " << key;
    return Status(ErrorCodes::DuplicateKey, sb.str());
}

Status MobileIndex::_checkKeySize(const BSONObj& key) {
    if (key.objsize() >= TempKeyMaxSize) {
        return Status(ErrorCodes::KeyTooLong, "key too big");
    }
    return Status::OK();
}

class MobileIndex::BulkBuilderBase : public SortedDataBuilderInterface {
public:
    BulkBuilderBase(MobileIndex* index, OperationContext* opCtx, bool dupsAllowed)
        : _index(index), _opCtx(opCtx), _dupsAllowed(dupsAllowed) {}

    virtual ~BulkBuilderBase() {}

    Status addKey(const BSONObj& key, const RecordId& recId) override {
        invariant(recId.isNormal());
        invariant(!hasFieldNames(key));

        Status status = _checkKeySize(key);
        if (!status.isOK()) {
            return status;
        }

        status = _checkNextKey(key);
        if (!status.isOK()) {
            return status;
        }

        _lastKey = key.getOwned();

        return _addKey(key, recId);
    }

    void commit(bool mayInterrupt) override {}

protected:
    /**
     * Checks whether the new key to be inserted is > or >= the previous one depending
     * on _dupsAllowed.
     */
    Status _checkNextKey(const BSONObj& key) {
        const int cmp = key.woCompare(_lastKey, _index->getOrdering());
        if (!_dupsAllowed && cmp == 0) {
            return _index->_dupKeyError(key);
        } else if (cmp < 0) {
            return Status(ErrorCodes::InternalError, "expected higher RecordId in bulk builder");
        }
        return Status::OK();
    }

    virtual Status _addKey(const BSONObj& key, const RecordId& recId) = 0;

    MobileIndex* _index;
    OperationContext* const _opCtx;
    BSONObj _lastKey;
    const bool _dupsAllowed;
};

/**
 * Bulk builds a non-unique index.
 */
class MobileIndex::BulkBuilderStandard final : public BulkBuilderBase {
public:
    BulkBuilderStandard(MobileIndex* index, OperationContext* opCtx, bool dupsAllowed)
        : BulkBuilderBase(index, opCtx, dupsAllowed) {}

protected:
    Status _addKey(const BSONObj& key, const RecordId& recId) override {
        KeyString keyStr(_index->getKeyStringVersion(), key, _index->getOrdering(), recId);
        KeyString::TypeBits value = keyStr.getTypeBits();
        return _index->doInsert(_opCtx, keyStr, value, false);
    }
};

/**
 * Bulk builds a unique index.
 */
class MobileIndex::BulkBuilderUnique : public BulkBuilderBase {
public:
    BulkBuilderUnique(MobileIndex* index, OperationContext* opCtx, bool dupsAllowed)
        : BulkBuilderBase(index, opCtx, dupsAllowed) {
        // Replication is not supported so dups are not allowed.
        invariant(!dupsAllowed);
    }

protected:
    Status _addKey(const BSONObj& key, const RecordId& recId) override {
        const KeyString keyStr(_index->getKeyStringVersion(), key, _index->getOrdering());

        KeyString value(_index->getKeyStringVersion(), recId);
        KeyString::TypeBits typeBits = keyStr.getTypeBits();
        if (!typeBits.isAllZeros()) {
            value.appendTypeBits(typeBits);
        }

        return _index->doInsert(_opCtx, keyStr, value, false);
    }
};

namespace {

/**
 * Implements basic cursor functionality used by standard and unique indexes.
 */
class CursorBase : public SortedDataInterface::Cursor {
public:
    CursorBase(const MobileIndex& index, OperationContext* opCtx, bool isForward)
        : _index(index),
          _opCtx(opCtx),
          _isForward(isForward),
          _savedKey(index.getKeyStringVersion()),
          _savedRecId(0),
          _savedTypeBits(index.getKeyStringVersion()),
          _startPosition(index.getKeyStringVersion()) {
        MobileSession* session = MobileRecoveryUnit::get(opCtx)->getSession(opCtx);
        str::stream cursorQuery;
        cursorQuery << "SELECT key, value FROM \"" << _index.getIdent() << "\" WHERE key ";
        cursorQuery << (_isForward ? ">=" : "<=") << " ? ORDER BY key ";
        cursorQuery << (_isForward ? "ASC" : "DESC") << ";";
        _stmt = stdx::make_unique<SqliteStatement>(*session, cursorQuery);
    }

    virtual ~CursorBase() {}

    boost::optional<IndexKeyEntry> next(RequestedInfo parts) override {
        if (_isEOF) {
            return {};
        }

        _advance();
        _updatePosition();

        return getCurrentEntry(parts);
    }

    void setEndPosition(const BSONObj& key, bool inclusive) override {
        // Scan to end of index.
        if (key.isEmpty()) {
            _endPosition.reset();
            return;
        }

        // This uses the opposite rules as a normal seek because a forward scan should end after the
        // key if inclusive and before if exclusive.
        const auto discriminator =
            _isForward == inclusive ? KeyString::kExclusiveAfter : KeyString::kExclusiveBefore;
        _endPosition = stdx::make_unique<KeyString>(_index.getKeyStringVersion());
        _endPosition->resetToKey(stripFieldNames(key), _index.getOrdering(), discriminator);
    }

    boost::optional<IndexKeyEntry> seek(const BSONObj& key,
                                        bool inclusive,
                                        RequestedInfo parts) override {
        const BSONObj startKey = stripFieldNames(key);
        // By using a discriminator other than kInclusive, there is no need to distinguish
        // unique vs non-unique key formats since both start with the key.
        const auto discriminator =
            _isForward == inclusive ? KeyString::kExclusiveBefore : KeyString::kExclusiveAfter;
        _startPosition.resetToKey(startKey, _index.getOrdering(), discriminator);

        _doSeek();
        _updatePosition();

        return getCurrentEntry(parts);
    }

    boost::optional<IndexKeyEntry> seek(const IndexSeekPoint& seekPoint,
                                        RequestedInfo parts) override {
        BSONObj startKey = IndexEntryComparison::makeQueryObject(seekPoint, _isForward);

        const auto discriminator =
            _isForward ? KeyString::kExclusiveBefore : KeyString::kExclusiveAfter;
        _startPosition.resetToKey(startKey, _index.getOrdering(), discriminator);

        _doSeek();
        _updatePosition();

        return getCurrentEntry(parts);
    }

    // All work is done in restore().
    void save() override {
        _resetStatement();
    }

    void saveUnpositioned() override {
        save();
    }

    void restore() override {
        if (_isEOF) {
            return;
        }

        _startPosition.resetFromBuffer(_savedKey.getBuffer(), _savedKey.getSize());
        bool isExactMatch = _doSeek();

        if (!isExactMatch) {
            _isEOF = false;
            _resetStatement();
        }
    }

    void detachFromOperationContext() override {
        _opCtx = nullptr;
    }

    void reattachToOperationContext(OperationContext* opCtx) override {
        _opCtx = opCtx;
    }

protected:
    /**
     * Advances the cursor and determines if end reached.
     */
    void _advance() {
        int status = _stmt->step();
        if (status == SQLITE_DONE) {
            _isEOF = true;
            return;
        }
        checkStatus(status, SQLITE_ROW, "sqlite3_step");

        _isEOF = false;

        const void* key = _stmt->getColBlob(0);
        const long long size = _stmt->getColBytes(0);

        KeyString currKey(_index.getKeyStringVersion());
        currKey.resetFromBuffer(key, size);

        // The cursor has reached EOF if the current row passes the end position.
        _isEOF = (_endPosition && _isForward && currKey > *_endPosition) ||
            (_endPosition && !_isForward && currKey < *_endPosition);
    }

    /**
     * Updates the cursor state to reflect current position by setting the current key value,
     * record id, and type bits.
     */
    void _updatePosition() {
        if (_isEOF) {
            return;
        }
        const void* key = _stmt->getColBlob(0);
        const long long size = _stmt->getColBytes(0);

        _savedKey.resetFromBuffer(key, size);
        _updateRecIdAndTypeBits();
    }

    /**
     * Returns the requested parts of the entry at the cursor's current position.
     */
    boost::optional<IndexKeyEntry> getCurrentEntry(RequestedInfo parts) const {
        if (_isEOF) {
            return {};
        }

        BSONObj bson;
        if (parts & kWantKey) {
            bson = KeyString::toBson(
                _savedKey.getBuffer(), _savedKey.getSize(), _index.getOrdering(), _savedTypeBits);
        }

        return {{std::move(bson), _savedRecId}};
    }

    /**
     * Moves the cursor to begin at the given position. Returns true if the new position matches
     * the saved position; returns false otherwise.
     */
    bool _doSeek() {
        _resetStatement();
        _bindStartPoint();

        _isEOF = false;

        _advance();

        if (_isEOF) {
            return false;
        }

        const void* key = _stmt->getColBlob(0);
        const long long size = _stmt->getColBytes(0);

        KeyString nearestKey(_index.getKeyStringVersion());
        nearestKey.resetFromBuffer(key, size);

        return nearestKey == _startPosition;
    }

    /**
     * Binds the start point for the cursor.
     */
    void _bindStartPoint() {
        _stmt->bindBlob(0, _startPosition.getBuffer(), _startPosition.getSize());
    }

    /**
     * Resets the prepared statement on the SQLite statement that performs the iteration.
     */
    void _resetStatement() {
        _stmt->reset();
    }

    virtual void _updateRecIdAndTypeBits() = 0;

    const MobileIndex& _index;
    OperationContext* _opCtx;  // Not owned.

    bool _isForward;
    bool _isEOF = true;

    KeyString _savedKey;
    RecordId _savedRecId;
    KeyString::TypeBits _savedTypeBits;

    // The statement executed to fetch rows from SQLite.
    std::unique_ptr<SqliteStatement> _stmt;

    KeyString _startPosition;
    std::unique_ptr<KeyString> _endPosition;
};

/**
 * Cursor for a non-unique index.
 */
class CursorStandard final : public CursorBase {
public:
    CursorStandard(const MobileIndex& index, OperationContext* opCtx, bool isForward)
        : CursorBase(index, opCtx, isForward) {}

protected:
    void _updateRecIdAndTypeBits() override {
        _savedRecId = KeyString::decodeRecordIdAtEnd(_savedKey.getBuffer(), _savedKey.getSize());

        const void* value = _stmt->getColBlob(1);
        const long long size = _stmt->getColBytes(1);
        BufReader br(value, size);
        _savedTypeBits.resetFromBuffer(&br);
    }
};

/**
 * Cursor for a unique index.
 */
class CursorUnique final : public CursorBase {
public:
    CursorUnique(const MobileIndex& index, OperationContext* opCtx, bool isForward)
        : CursorBase(index, opCtx, isForward) {}

protected:
    void _updateRecIdAndTypeBits() override {
        const void* value = _stmt->getColBlob(1);
        const long long size = _stmt->getColBytes(1);
        BufReader br(value, size);
        _savedRecId = KeyString::decodeRecordId(&br);
        _savedTypeBits.resetFromBuffer(&br);
    }
};
}  // namespace

MobileIndexStandard::MobileIndexStandard(OperationContext* opCtx,
                                         const IndexDescriptor* desc,
                                         const std::string& ident)
    : MobileIndex(opCtx, desc, ident) {}

MobileIndexStandard::MobileIndexStandard(const Ordering& ordering, const std::string& ident)
    : MobileIndex(false, ordering, ident) {}

SortedDataBuilderInterface* MobileIndexStandard::getBulkBuilder(OperationContext* opCtx,
                                                                bool dupsAllowed) {
    invariant(dupsAllowed);
    return new BulkBuilderStandard(this, opCtx, dupsAllowed);
}

std::unique_ptr<SortedDataInterface::Cursor> MobileIndexStandard::newCursor(OperationContext* opCtx,
                                                                            bool isForward) const {
    return stdx::make_unique<CursorStandard>(*this, opCtx, isForward);
}

Status MobileIndexStandard::_insert(OperationContext* opCtx,
                                    const BSONObj& key,
                                    const RecordId& recId,
                                    bool dupsAllowed) {
    invariant(dupsAllowed);

    const KeyString keyStr(_keyStringVersion, key, _ordering, recId);
    const KeyString::TypeBits value = keyStr.getTypeBits();
    return doInsert(opCtx, keyStr, value);
}

void MobileIndexStandard::_unindex(OperationContext* opCtx,
                                   const BSONObj& key,
                                   const RecordId& recId,
                                   bool dupsAllowed) {
    invariant(dupsAllowed);

    const KeyString keyStr(_keyStringVersion, key, _ordering, recId);
    _doDelete(opCtx, keyStr);
}

MobileIndexUnique::MobileIndexUnique(OperationContext* opCtx,
                                     const IndexDescriptor* desc,
                                     const std::string& ident)
    : MobileIndex(opCtx, desc, ident), _isPartial(desc->isPartial()) {}

MobileIndexUnique::MobileIndexUnique(const Ordering& ordering, const std::string& ident)
    : MobileIndex(true, ordering, ident) {}

SortedDataBuilderInterface* MobileIndexUnique::getBulkBuilder(OperationContext* opCtx,
                                                              bool dupsAllowed) {
    // Replication is not supported so dups are not allowed.
    invariant(!dupsAllowed);
    return new BulkBuilderUnique(this, opCtx, dupsAllowed);
}

std::unique_ptr<SortedDataInterface::Cursor> MobileIndexUnique::newCursor(OperationContext* opCtx,
                                                                          bool isForward) const {
    return stdx::make_unique<CursorUnique>(*this, opCtx, isForward);
}

Status MobileIndexUnique::_insert(OperationContext* opCtx,
                                  const BSONObj& key,
                                  const RecordId& recId,
                                  bool dupsAllowed) {
    // Replication is not supported so dups are not allowed.
    invariant(!dupsAllowed);
    const KeyString keyStr(_keyStringVersion, key, _ordering);

    KeyString value(_keyStringVersion, recId);
    KeyString::TypeBits typeBits = keyStr.getTypeBits();
    if (!typeBits.isAllZeros()) {
        value.appendTypeBits(typeBits);
    }

    return doInsert(opCtx, keyStr, value);
}

void MobileIndexUnique::_unindex(OperationContext* opCtx,
                                 const BSONObj& key,
                                 const RecordId& recId,
                                 bool dupsAllowed) {
    // Replication is not supported so dups are not allowed.
    invariant(!dupsAllowed);
    const KeyString keyStr(_keyStringVersion, key, _ordering);

    // A partial index may attempt to delete a non-existent record id. If it is a partial index, it
    // must delete a row that matches both key and value.
    if (_isPartial) {
        KeyString value(_keyStringVersion, recId);
        KeyString::TypeBits typeBits = keyStr.getTypeBits();
        if (!typeBits.isAllZeros()) {
            value.appendTypeBits(typeBits);
        }

        _doDelete(opCtx, keyStr, &value);
    } else {
        _doDelete(opCtx, keyStr);
    }
}

}  // namespace mongo
