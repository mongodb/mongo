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
}  // namespace

MobileIndex::MobileIndex(OperationContext* opCtx,
                         const IndexDescriptor* desc,
                         const std::string& ident)
    : SortedDataInterface(KeyString::Version::kLatestVersion, Ordering::make(desc->keyPattern())),
      _isUnique(desc->unique()),
      _ordering(Ordering::make(desc->keyPattern())),
      _ident(ident),
      _collectionNamespace(desc->parentNS()),
      _indexName(desc->indexName()),
      _keyPattern(desc->keyPattern()) {}

Status MobileIndex::insert(OperationContext* opCtx,
                           const BSONObj& key,
                           const RecordId& recId,
                           bool dupsAllowed) {
    invariant(recId.isValid());
    invariant(!key.hasFieldNames());

    KeyString::HeapBuilder keyString(_keyStringVersion, key, _ordering, recId);

    return insert(opCtx, std::move(keyString.release()), recId, dupsAllowed);
}

Status MobileIndex::insert(OperationContext* opCtx,
                           const KeyString::Value& keyString,
                           const RecordId& recId,
                           bool dupsAllowed) {
    return _insert(opCtx, keyString, recId, dupsAllowed);
}

template <typename ValueType>
Status MobileIndex::doInsert(OperationContext* opCtx,
                             const char* keyBuffer,
                             size_t keySize,
                             const KeyString::TypeBits& typeBits,
                             const ValueType& value,
                             bool isTransactional) {
    MobileSession* session;
    if (isTransactional) {
        session = MobileRecoveryUnit::get(opCtx)->getSession(opCtx, false);
    } else {
        session = MobileRecoveryUnit::get(opCtx)->getSessionNoTxn(opCtx);
    }

    SqliteStatement insertStmt(
        *session, "INSERT INTO \"", _ident, "\" (key, value) VALUES (?, ?);");

    insertStmt.bindBlob(0, keyBuffer, keySize);
    insertStmt.bindBlob(1, value.getBuffer(), value.getSize());

    int status = insertStmt.step();
    if (status == SQLITE_CONSTRAINT) {
        insertStmt.setExceptionStatus(status);
        if (isUnique()) {
            // Return error if duplicate key inserted in a unique index.
            BSONObj bson = KeyString::toBson(keyBuffer, keySize, _ordering, typeBits);
            return buildDupKeyErrorStatus(bson, _collectionNamespace, _indexName, _keyPattern);
        } else {
            // A record with same key could already be present in a standard index, that is OK. This
            // can happen when building a background index while documents are being written in
            // parallel.
            return Status::OK();
        }
    }
    embedded::checkStatus(status, SQLITE_DONE, "sqlite3_step");

    return Status::OK();
}

void MobileIndex::unindex(OperationContext* opCtx,
                          const BSONObj& key,
                          const RecordId& recId,
                          bool dupsAllowed) {
    invariant(recId.isValid());
    invariant(!key.hasFieldNames());

    KeyString::HeapBuilder keyString(_keyStringVersion, key, _ordering, recId);

    unindex(opCtx, std::move(keyString.release()), recId, dupsAllowed);
}

void MobileIndex::unindex(OperationContext* opCtx,
                          const KeyString::Value& keyString,
                          const RecordId& recId,
                          bool dupsAllowed) {
    _unindex(opCtx, keyString, recId, dupsAllowed);
}

void MobileIndex::_doDelete(OperationContext* opCtx,
                            const char* keyBuffer,
                            size_t keySize,
                            KeyString::Builder* value) {
    MobileSession* session = MobileRecoveryUnit::get(opCtx)->getSession(opCtx, false);

    SqliteStatement deleteStmt(
        *session, "DELETE FROM \"", _ident, "\" WHERE key = ?", value ? " AND value = ?" : "", ";");

    deleteStmt.bindBlob(0, keyBuffer, keySize);
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
        embedded::doValidate(opCtx, fullResults);
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
    return false;
}

long long MobileIndex::getSpaceUsedBytes(OperationContext* opCtx) const {
    MobileSession* session = MobileRecoveryUnit::get(opCtx)->getSession(opCtx);

    // Sum the number of bytes in each column.
    // SQLite aggregate functions return null if the column is empty or has only nulls, so return 0
    // bytes if there is no data in the column.
    SqliteStatement sizeStmt(*session,
                             "SELECT IFNULL(SUM(LENGTH(key)), 0) + ",
                             "IFNULL(SUM(LENGTH(value)), 0) FROM \"",
                             _ident,
                             "\";");

    sizeStmt.step(SQLITE_ROW);

    long long dataSize = sizeStmt.getColInt(0);
    return dataSize;
}

long long MobileIndex::numEntries(OperationContext* opCtx) const {
    MobileSession* session = MobileRecoveryUnit::get(opCtx)->getSession(opCtx);

    SqliteStatement countStmt(*session, "SELECT COUNT(*) FROM \"", _ident, "\";");

    countStmt.step(SQLITE_ROW);
    long long numRecs = countStmt.getColInt(0);
    return numRecs;
}

bool MobileIndex::isEmpty(OperationContext* opCtx) {
    MobileSession* session = MobileRecoveryUnit::get(opCtx)->getSession(opCtx);

    SqliteStatement emptyCheckStmt(*session, "SELECT * FROM \"", _ident, "\" LIMIT 1;");

    int status = emptyCheckStmt.step();
    if (status == SQLITE_DONE) {
        return true;
    }
    embedded::checkStatus(status, SQLITE_ROW, "sqlite3_step");
    return false;
}

Status MobileIndex::initAsEmpty(OperationContext* opCtx) {
    // No-op.
    return Status::OK();
}

Status MobileIndex::create(OperationContext* opCtx, const std::string& ident) {
    MobileSession* session = MobileRecoveryUnit::get(opCtx)->getSessionNoTxn(opCtx);

    SqliteStatement createTableStmt(
        *session, "CREATE TABLE \"", ident, "\"(key BLOB PRIMARY KEY, value BLOB);");

    createTableStmt.step(SQLITE_DONE);
    return Status::OK();
}

Status MobileIndex::dupKeyCheck(OperationContext* opCtx, const BSONObj& key) {
    invariant(!key.hasFieldNames());
    invariant(_isUnique);

    if (_isDup(opCtx, key))
        return buildDupKeyErrorStatus(key, _collectionNamespace, _indexName, _keyPattern);
    return Status::OK();
}

bool MobileIndex::_isDup(OperationContext* opCtx, const BSONObj& key) {
    MobileSession* session = MobileRecoveryUnit::get(opCtx)->getSession(opCtx);

    SqliteStatement dupCheckStmt(*session, "SELECT COUNT(*) FROM \"", _ident, "\" WHERE key = ?;");

    KeyString::Builder keyStr(_keyStringVersion, key, _ordering);
    dupCheckStmt.bindBlob(0, keyStr.getBuffer(), keyStr.getSize());

    dupCheckStmt.step(SQLITE_ROW);
    long long records = dupCheckStmt.getColInt(0);
    return records > 1;
}

class MobileIndex::BulkBuilderBase : public SortedDataBuilderInterface {
public:
    BulkBuilderBase(MobileIndex* index,
                    OperationContext* opCtx,
                    bool dupsAllowed,
                    const NamespaceString& collectionNamespace,
                    const std::string& indexName,
                    const BSONObj& keyPattern)
        : _index(index),
          _opCtx(opCtx),
          _lastKeyString(index->getKeyStringVersion()),
          _dupsAllowed(dupsAllowed),
          _collectionNamespace(collectionNamespace),
          _indexName(indexName),
          _keyPattern(keyPattern) {}

    virtual ~BulkBuilderBase() {}

    Status addKey(const BSONObj& key, const RecordId& recId) override {
        invariant(recId.isValid());
        invariant(!key.hasFieldNames());

        KeyString::HeapBuilder keyString(
            _index->getKeyStringVersion(), key, _index->getOrdering(), recId);
        return addKey(std::move(keyString.release()), recId);
    }

    Status addKey(const KeyString::Value& keyString, const RecordId& recId) override {
        Status status = _checkNextKey(keyString);
        if (!status.isOK()) {
            return status;
        }

        _lastKeyString.resetFromBuffer(keyString.getBuffer(), keyString.getSize());

        return _addKey(keyString, recId);
    }

    void commit(bool mayInterrupt) override {}

protected:
    /**
     * Checks whether the new key to be inserted is > or >= the previous one depending
     * on _dupsAllowed.
     */
    Status _checkNextKey(const KeyString::Value& keyString) {
        const int cmp = _lastKeyString.compare(keyString);
        if (!_dupsAllowed && cmp == 0) {
            auto key = KeyString::toBson(keyString, _index->getOrdering());
            return buildDupKeyErrorStatus(key, _collectionNamespace, _indexName, _keyPattern);
        } else if (cmp > 0) {
            return Status(ErrorCodes::InternalError, "expected higher RecordId in bulk builder");
        }
        return Status::OK();
    }

    virtual Status _addKey(const KeyString::Value& keyString, const RecordId& recId) = 0;

    MobileIndex* _index;
    OperationContext* const _opCtx;
    KeyString::Builder _lastKeyString;
    const bool _dupsAllowed;
    const NamespaceString _collectionNamespace;
    const std::string _indexName;
    const BSONObj _keyPattern;
};

/**
 * Bulk builds a non-unique index.
 */
class MobileIndex::BulkBuilderStandard final : public BulkBuilderBase {
public:
    BulkBuilderStandard(MobileIndex* index,
                        OperationContext* opCtx,
                        bool dupsAllowed,
                        const NamespaceString& collectionNamespace,
                        const std::string& indexName,
                        const BSONObj& keyPattern)
        : BulkBuilderBase(index, opCtx, dupsAllowed, collectionNamespace, indexName, keyPattern) {}

protected:
    Status _addKey(const KeyString::Value& keyString, const RecordId&) override {
        KeyString::TypeBits typeBits = keyString.getTypeBits();
        return _index->doInsert(
            _opCtx, keyString.getBuffer(), keyString.getSize(), typeBits, typeBits, false);
    }
};

/**
 * Bulk builds a unique index.
 */
class MobileIndex::BulkBuilderUnique : public BulkBuilderBase {
public:
    BulkBuilderUnique(MobileIndex* index,
                      OperationContext* opCtx,
                      bool dupsAllowed,
                      const NamespaceString& collectionNamespace,
                      const std::string& indexName,
                      const BSONObj& keyPattern)
        : BulkBuilderBase(index, opCtx, dupsAllowed, collectionNamespace, indexName, keyPattern) {
        // Replication is not supported so dups are not allowed.
        invariant(!dupsAllowed);
    }

protected:
    Status _addKey(const KeyString::Value& keyString, const RecordId& recId) override {
        dassert(recId ==
                KeyString::decodeRecordIdAtEnd(keyString.getBuffer(), keyString.getSize()));

        KeyString::Builder value(_index->getKeyStringVersion(), recId);
        KeyString::TypeBits typeBits = keyString.getTypeBits();
        if (!typeBits.isAllZeros()) {
            value.appendTypeBits(typeBits);
        }
        auto sizeWithoutRecordId =
            KeyString::sizeWithoutRecordIdAtEnd(keyString.getBuffer(), keyString.getSize());

        return _index->doInsert(_opCtx,
                                keyString.getBuffer(),
                                sizeWithoutRecordId,
                                keyString.getTypeBits(),
                                value,
                                false);
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

        _stmt = std::make_unique<SqliteStatement>(*session,
                                                  "SELECT key, value FROM \"",
                                                  _index.getIdent(),
                                                  "\" WHERE key ",
                                                  (_isForward ? ">=" : "<="),
                                                  " ? ORDER BY key ",
                                                  (_isForward ? "ASC" : "DESC"),
                                                  ";");
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
        const auto discriminator = _isForward == inclusive
            ? KeyString::Discriminator::kExclusiveAfter
            : KeyString::Discriminator::kExclusiveBefore;
        _endPosition = std::make_unique<KeyString::Builder>(_index.getKeyStringVersion());
        _endPosition->resetToKey(
            BSONObj::stripFieldNames(key), _index.getOrdering(), discriminator);
    }

    boost::optional<IndexKeyEntry> seek(const BSONObj& key,
                                        bool inclusive,
                                        RequestedInfo parts) override {
        const BSONObj startKey = BSONObj::stripFieldNames(key);
        // By using a discriminator other than kInclusive, there is no need to distinguish
        // unique vs non-unique key formats since both start with the key.
        const auto discriminator = _isForward == inclusive
            ? KeyString::Discriminator::kExclusiveBefore
            : KeyString::Discriminator::kExclusiveAfter;
        _startPosition.resetToKey(startKey, _index.getOrdering(), discriminator);

        _doSeek();
        _updatePosition();

        return getCurrentEntry(parts);
    }

    boost::optional<IndexKeyEntry> seek(const IndexSeekPoint& seekPoint,
                                        RequestedInfo parts) override {
        BSONObj startKey = IndexEntryComparison::makeQueryObject(seekPoint, _isForward);

        const auto discriminator = _isForward ? KeyString::Discriminator::kExclusiveBefore
                                              : KeyString::Discriminator::kExclusiveAfter;
        _startPosition.resetToKey(startKey, _index.getOrdering(), discriminator);

        _doSeek();
        _updatePosition();

        return getCurrentEntry(parts);
    }

    // All work is done in restore().
    void save() override {
        // SQLite acquires implicit locks over the snapshot this cursor is using. It is important
        // to finalize the corresponding statement to release these locks.
        _stmt->finalize();
    }

    void saveUnpositioned() override {
        save();
    }

    void restore() override {
        if (_isEOF) {
            return;
        }

        // Obtaining a session starts a read transaction if not done already.
        MobileSession* session = MobileRecoveryUnit::get(_opCtx)->getSession(_opCtx);
        // save() finalized this cursor's SQLite statement. We need to prepare a new statement,
        // before re-positioning it at the saved state.
        _stmt->prepare(*session);

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
        embedded::checkStatus(status, SQLITE_ROW, "sqlite3_step");

        _isEOF = false;

        const void* key = _stmt->getColBlob(0);
        const long long size = _stmt->getColBytes(0);

        KeyString::Builder currKey(_index.getKeyStringVersion());
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

        KeyString::Builder nearestKey(_index.getKeyStringVersion());
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

    KeyString::Builder _savedKey;
    RecordId _savedRecId;
    KeyString::TypeBits _savedTypeBits;

    // The statement executed to fetch rows from SQLite.
    std::unique_ptr<SqliteStatement> _stmt;

    KeyString::Builder _startPosition;
    std::unique_ptr<KeyString::Builder> _endPosition;
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

SortedDataBuilderInterface* MobileIndexStandard::getBulkBuilder(OperationContext* opCtx,
                                                                bool dupsAllowed) {
    invariant(dupsAllowed);
    return new BulkBuilderStandard(
        this, opCtx, dupsAllowed, _collectionNamespace, _indexName, _keyPattern);
}

std::unique_ptr<SortedDataInterface::Cursor> MobileIndexStandard::newCursor(OperationContext* opCtx,
                                                                            bool isForward) const {
    return std::make_unique<CursorStandard>(*this, opCtx, isForward);
}

Status MobileIndexStandard::_insert(OperationContext* opCtx,
                                    const KeyString::Value& keyString,
                                    const RecordId& recId,
                                    bool dupsAllowed) {
    invariant(dupsAllowed);
    dassert(recId == KeyString::decodeRecordIdAtEnd(keyString.getBuffer(), keyString.getSize()));

    const KeyString::TypeBits typeBits = keyString.getTypeBits();
    return doInsert(opCtx, keyString.getBuffer(), keyString.getSize(), typeBits, typeBits);
}

void MobileIndexStandard::_unindex(OperationContext* opCtx,
                                   const KeyString::Value& keyString,
                                   const RecordId&,
                                   bool dupsAllowed) {
    invariant(dupsAllowed);

    _doDelete(opCtx, keyString.getBuffer(), keyString.getSize());
}

MobileIndexUnique::MobileIndexUnique(OperationContext* opCtx,
                                     const IndexDescriptor* desc,
                                     const std::string& ident)
    : MobileIndex(opCtx, desc, ident), _isPartial(desc->isPartial()) {}

SortedDataBuilderInterface* MobileIndexUnique::getBulkBuilder(OperationContext* opCtx,
                                                              bool dupsAllowed) {
    // Replication is not supported so dups are not allowed.
    invariant(!dupsAllowed);
    return new BulkBuilderUnique(
        this, opCtx, dupsAllowed, _collectionNamespace, _indexName, _keyPattern);
}

std::unique_ptr<SortedDataInterface::Cursor> MobileIndexUnique::newCursor(OperationContext* opCtx,
                                                                          bool isForward) const {
    return std::make_unique<CursorUnique>(*this, opCtx, isForward);
}

Status MobileIndexUnique::_insert(OperationContext* opCtx,
                                  const KeyString::Value& keyString,
                                  const RecordId& recId,
                                  bool dupsAllowed) {
    // Replication is not supported so dups are not allowed.
    invariant(!dupsAllowed);

    invariant(recId.isValid());
    dassert(recId == KeyString::decodeRecordIdAtEnd(keyString.getBuffer(), keyString.getSize()));

    KeyString::Builder value(_keyStringVersion, recId);
    KeyString::TypeBits typeBits = keyString.getTypeBits();
    if (!typeBits.isAllZeros()) {
        value.appendTypeBits(typeBits);
    }
    auto sizeWithoutRecordId =
        KeyString::sizeWithoutRecordIdAtEnd(keyString.getBuffer(), keyString.getSize());

    return doInsert(
        opCtx, keyString.getBuffer(), sizeWithoutRecordId, keyString.getTypeBits(), value);
}

void MobileIndexUnique::_unindex(OperationContext* opCtx,
                                 const KeyString::Value& keyString,
                                 const RecordId& recId,
                                 bool dupsAllowed) {
    // Replication is not supported so dups are not allowed.
    invariant(!dupsAllowed);

    // A partial index may attempt to delete a non-existent record id. If it is a partial index, it
    // must delete a row that matches both key and value.
    auto sizeWithoutRecordId =
        KeyString::sizeWithoutRecordIdAtEnd(keyString.getBuffer(), keyString.getSize());
    if (_isPartial) {
        invariant(recId.isValid());
        dassert(recId ==
                KeyString::decodeRecordIdAtEnd(keyString.getBuffer(), keyString.getSize()));

        KeyString::Builder value(_keyStringVersion, recId);
        KeyString::TypeBits typeBits = keyString.getTypeBits();
        if (!typeBits.isAllZeros()) {
            value.appendTypeBits(typeBits);
        }

        _doDelete(opCtx, keyString.getBuffer(), sizeWithoutRecordId, &value);
    } else {
        _doDelete(opCtx, keyString.getBuffer(), sizeWithoutRecordId);
    }
}

}  // namespace mongo
