// ephemeral_for_test_record_store.cpp

/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
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

#include "mongo/db/storage/ephemeral_for_test/ephemeral_for_test_record_store.h"


#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/oplog_hack.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/unowned_ptr.h"

namespace mongo {

using std::shared_ptr;

class EphemeralForTestRecordStore::InsertChange : public RecoveryUnit::Change {
public:
    InsertChange(Data* data, RecordId loc) : _data(data), _loc(loc) {}
    virtual void commit() {}
    virtual void rollback() {
        Records::iterator it = _data->records.find(_loc);
        if (it != _data->records.end()) {
            _data->dataSize -= it->second.size;
            _data->records.erase(it);
        }
    }

private:
    Data* const _data;
    const RecordId _loc;
};

// Works for both removes and updates
class EphemeralForTestRecordStore::RemoveChange : public RecoveryUnit::Change {
public:
    RemoveChange(Data* data, RecordId loc, const EphemeralForTestRecord& rec)
        : _data(data), _loc(loc), _rec(rec) {}

    virtual void commit() {}
    virtual void rollback() {
        Records::iterator it = _data->records.find(_loc);
        if (it != _data->records.end()) {
            _data->dataSize -= it->second.size;
        }

        _data->dataSize += _rec.size;
        _data->records[_loc] = _rec;
    }

private:
    Data* const _data;
    const RecordId _loc;
    const EphemeralForTestRecord _rec;
};

class EphemeralForTestRecordStore::TruncateChange : public RecoveryUnit::Change {
public:
    TruncateChange(Data* data) : _data(data), _dataSize(0) {
        using std::swap;
        swap(_dataSize, _data->dataSize);
        swap(_records, _data->records);
    }

    virtual void commit() {}
    virtual void rollback() {
        using std::swap;
        swap(_dataSize, _data->dataSize);
        swap(_records, _data->records);
    }

private:
    Data* const _data;
    int64_t _dataSize;
    Records _records;
};

class EphemeralForTestRecordStore::Cursor final : public SeekableRecordCursor {
public:
    Cursor(OperationContext* txn, const EphemeralForTestRecordStore& rs)
        : _records(rs._data->records), _isCapped(rs.isCapped()) {}

    boost::optional<Record> next() final {
        if (_needFirstSeek) {
            _needFirstSeek = false;
            _it = _records.begin();
        } else if (!_lastMoveWasRestore && _it != _records.end()) {
            ++_it;
        }
        _lastMoveWasRestore = false;

        if (_it == _records.end())
            return {};
        return {{_it->first, _it->second.toRecordData()}};
    }

    boost::optional<Record> seekExact(const RecordId& id) final {
        _lastMoveWasRestore = false;
        _needFirstSeek = false;
        _it = _records.find(id);
        if (_it == _records.end())
            return {};
        return {{_it->first, _it->second.toRecordData()}};
    }

    void save() final {
        if (!_needFirstSeek && !_lastMoveWasRestore)
            _savedId = _it == _records.end() ? RecordId() : _it->first;
    }

    void saveUnpositioned() final {
        _savedId = RecordId();
    }

    bool restore() final {
        if (_savedId.isNull()) {
            _it = _records.end();
            return true;
        }

        _it = _records.lower_bound(_savedId);
        _lastMoveWasRestore = _it == _records.end() || _it->first != _savedId;

        // Capped iterators die on invalidation rather than advancing.
        return !(_isCapped && _lastMoveWasRestore);
    }

    void detachFromOperationContext() final {}
    void reattachToOperationContext(OperationContext* txn) final {}

private:
    Records::const_iterator _it;
    bool _needFirstSeek = true;
    bool _lastMoveWasRestore = false;
    RecordId _savedId;  // Location to restore() to. Null means EOF.

    const EphemeralForTestRecordStore::Records& _records;
    const bool _isCapped;
};

class EphemeralForTestRecordStore::ReverseCursor final : public SeekableRecordCursor {
public:
    ReverseCursor(OperationContext* txn, const EphemeralForTestRecordStore& rs)
        : _records(rs._data->records), _isCapped(rs.isCapped()) {}

    boost::optional<Record> next() final {
        if (_needFirstSeek) {
            _needFirstSeek = false;
            _it = _records.rbegin();
        } else if (!_lastMoveWasRestore && _it != _records.rend()) {
            ++_it;
        }
        _lastMoveWasRestore = false;

        if (_it == _records.rend())
            return {};
        return {{_it->first, _it->second.toRecordData()}};
    }

    boost::optional<Record> seekExact(const RecordId& id) final {
        _lastMoveWasRestore = false;
        _needFirstSeek = false;

        auto forwardIt = _records.find(id);
        if (forwardIt == _records.end()) {
            _it = _records.rend();
            return {};
        }

        // The reverse_iterator will point to the preceding element, so increment the base
        // iterator to make it point past the found element.
        ++forwardIt;
        _it = Records::const_reverse_iterator(forwardIt);
        dassert(_it != _records.rend());
        dassert(_it->first == id);
        return {{_it->first, _it->second.toRecordData()}};
    }

    void save() final {
        if (!_needFirstSeek && !_lastMoveWasRestore)
            _savedId = _it == _records.rend() ? RecordId() : _it->first;
    }

    void saveUnpositioned() final {
        _savedId = RecordId();
    }

    bool restore() final {
        if (_savedId.isNull()) {
            _it = _records.rend();
            return true;
        }

        // Note: upper_bound returns the first entry > _savedId and reverse_iterators
        // dereference to the element before their base iterator. This combine to make this
        // dereference to the first element <= _savedId which is what we want here.
        _it = Records::const_reverse_iterator(_records.upper_bound(_savedId));
        _lastMoveWasRestore = _it == _records.rend() || _it->first != _savedId;

        // Capped iterators die on invalidation rather than advancing.
        return !(_isCapped && _lastMoveWasRestore);
    }

    void detachFromOperationContext() final {}
    void reattachToOperationContext(OperationContext* txn) final {}

private:
    Records::const_reverse_iterator _it;
    bool _needFirstSeek = true;
    bool _lastMoveWasRestore = false;
    RecordId _savedId;  // Location to restore() to. Null means EOF.
    const EphemeralForTestRecordStore::Records& _records;
    const bool _isCapped;
};


//
// RecordStore
//

EphemeralForTestRecordStore::EphemeralForTestRecordStore(StringData ns,
                                                         std::shared_ptr<void>* dataInOut,
                                                         bool isCapped,
                                                         int64_t cappedMaxSize,
                                                         int64_t cappedMaxDocs,
                                                         CappedCallback* cappedCallback)
    : RecordStore(ns),
      _isCapped(isCapped),
      _cappedMaxSize(cappedMaxSize),
      _cappedMaxDocs(cappedMaxDocs),
      _cappedCallback(cappedCallback),
      _data(*dataInOut ? static_cast<Data*>(dataInOut->get())
                       : new Data(NamespaceString::oplog(ns))) {
    if (!*dataInOut) {
        dataInOut->reset(_data);  // takes ownership
    }

    if (_isCapped) {
        invariant(_cappedMaxSize > 0);
        invariant(_cappedMaxDocs == -1 || _cappedMaxDocs > 0);
    } else {
        invariant(_cappedMaxSize == -1);
        invariant(_cappedMaxDocs == -1);
    }
}

const char* EphemeralForTestRecordStore::name() const {
    return "EphemeralForTest";
}

RecordData EphemeralForTestRecordStore::dataFor(OperationContext* txn, const RecordId& loc) const {
    return recordFor(loc)->toRecordData();
}

const EphemeralForTestRecordStore::EphemeralForTestRecord* EphemeralForTestRecordStore::recordFor(
    const RecordId& loc) const {
    Records::const_iterator it = _data->records.find(loc);
    if (it == _data->records.end()) {
        error() << "EphemeralForTestRecordStore::recordFor cannot find record for " << ns() << ":"
                << loc;
    }
    invariant(it != _data->records.end());
    return &it->second;
}

EphemeralForTestRecordStore::EphemeralForTestRecord* EphemeralForTestRecordStore::recordFor(
    const RecordId& loc) {
    Records::iterator it = _data->records.find(loc);
    if (it == _data->records.end()) {
        error() << "EphemeralForTestRecordStore::recordFor cannot find record for " << ns() << ":"
                << loc;
    }
    invariant(it != _data->records.end());
    return &it->second;
}

bool EphemeralForTestRecordStore::findRecord(OperationContext* txn,
                                             const RecordId& loc,
                                             RecordData* rd) const {
    Records::const_iterator it = _data->records.find(loc);
    if (it == _data->records.end()) {
        return false;
    }
    *rd = it->second.toRecordData();
    return true;
}

void EphemeralForTestRecordStore::deleteRecord(OperationContext* txn, const RecordId& loc) {
    EphemeralForTestRecord* rec = recordFor(loc);
    txn->recoveryUnit()->registerChange(new RemoveChange(_data, loc, *rec));
    _data->dataSize -= rec->size;
    invariant(_data->records.erase(loc) == 1);
}

bool EphemeralForTestRecordStore::cappedAndNeedDelete(OperationContext* txn) const {
    if (!_isCapped)
        return false;

    if (_data->dataSize > _cappedMaxSize)
        return true;

    if ((_cappedMaxDocs != -1) && (numRecords(txn) > _cappedMaxDocs))
        return true;

    return false;
}

void EphemeralForTestRecordStore::cappedDeleteAsNeeded(OperationContext* txn) {
    while (cappedAndNeedDelete(txn)) {
        invariant(!_data->records.empty());

        Records::iterator oldest = _data->records.begin();
        RecordId id = oldest->first;
        RecordData data = oldest->second.toRecordData();

        if (_cappedCallback)
            uassertStatusOK(_cappedCallback->aboutToDeleteCapped(txn, id, data));

        deleteRecord(txn, id);
    }
}

StatusWith<RecordId> EphemeralForTestRecordStore::extractAndCheckLocForOplog(const char* data,
                                                                             int len) const {
    StatusWith<RecordId> status = oploghack::extractKey(data, len);
    if (!status.isOK())
        return status;

    if (!_data->records.empty() && status.getValue() <= _data->records.rbegin()->first)
        return StatusWith<RecordId>(ErrorCodes::BadValue, "ts not higher than highest");

    return status;
}

StatusWith<RecordId> EphemeralForTestRecordStore::insertRecord(OperationContext* txn,
                                                               const char* data,
                                                               int len,
                                                               bool enforceQuota) {
    if (_isCapped && len > _cappedMaxSize) {
        // We use dataSize for capped rollover and we don't want to delete everything if we know
        // this won't fit.
        return StatusWith<RecordId>(ErrorCodes::BadValue, "object to insert exceeds cappedMaxSize");
    }

    EphemeralForTestRecord rec(len);
    memcpy(rec.data.get(), data, len);

    RecordId loc;
    if (_data->isOplog) {
        StatusWith<RecordId> status = extractAndCheckLocForOplog(data, len);
        if (!status.isOK())
            return status;
        loc = status.getValue();
    } else {
        loc = allocateLoc();
    }

    txn->recoveryUnit()->registerChange(new InsertChange(_data, loc));
    _data->dataSize += len;
    _data->records[loc] = rec;

    cappedDeleteAsNeeded(txn);

    return StatusWith<RecordId>(loc);
}

Status EphemeralForTestRecordStore::insertRecordsWithDocWriter(OperationContext* txn,
                                                               const DocWriter* const* docs,
                                                               size_t nDocs,
                                                               RecordId* idsOut) {
    for (size_t i = 0; i < nDocs; i++) {
        const int len = docs[i]->documentSize();
        if (_isCapped && len > _cappedMaxSize) {
            // We use dataSize for capped rollover and we don't want to delete everything if we know
            // this won't fit.
            return Status(ErrorCodes::BadValue, "object to insert exceeds cappedMaxSize");
        }

        EphemeralForTestRecord rec(len);
        docs[i]->writeDocument(rec.data.get());

        RecordId loc;
        if (_data->isOplog) {
            StatusWith<RecordId> status = extractAndCheckLocForOplog(rec.data.get(), len);
            if (!status.isOK())
                return status.getStatus();
            loc = status.getValue();
        } else {
            loc = allocateLoc();
        }

        txn->recoveryUnit()->registerChange(new InsertChange(_data, loc));
        _data->dataSize += len;
        _data->records[loc] = rec;

        cappedDeleteAsNeeded(txn);

        if (idsOut)
            idsOut[i] = loc;
    }

    return Status::OK();
}

Status EphemeralForTestRecordStore::updateRecord(OperationContext* txn,
                                                 const RecordId& loc,
                                                 const char* data,
                                                 int len,
                                                 bool enforceQuota,
                                                 UpdateNotifier* notifier) {
    EphemeralForTestRecord* oldRecord = recordFor(loc);
    int oldLen = oldRecord->size;

    // Documents in capped collections cannot change size. We check that above the storage layer.
    invariant(!_isCapped || len == oldLen);

    if (notifier) {
        // The in-memory KV engine uses the invalidation framework (does not support
        // doc-locking), and therefore must notify that it is updating a document.
        Status callbackStatus = notifier->recordStoreGoingToUpdateInPlace(txn, loc);
        if (!callbackStatus.isOK()) {
            return callbackStatus;
        }
    }

    EphemeralForTestRecord newRecord(len);
    memcpy(newRecord.data.get(), data, len);

    txn->recoveryUnit()->registerChange(new RemoveChange(_data, loc, *oldRecord));
    _data->dataSize += len - oldLen;
    *oldRecord = newRecord;

    cappedDeleteAsNeeded(txn);

    return Status::OK();
}

bool EphemeralForTestRecordStore::updateWithDamagesSupported() const {
    return true;
}

StatusWith<RecordData> EphemeralForTestRecordStore::updateWithDamages(
    OperationContext* txn,
    const RecordId& loc,
    const RecordData& oldRec,
    const char* damageSource,
    const mutablebson::DamageVector& damages) {
    EphemeralForTestRecord* oldRecord = recordFor(loc);
    const int len = oldRecord->size;

    EphemeralForTestRecord newRecord(len);
    memcpy(newRecord.data.get(), oldRecord->data.get(), len);

    txn->recoveryUnit()->registerChange(new RemoveChange(_data, loc, *oldRecord));
    *oldRecord = newRecord;

    cappedDeleteAsNeeded(txn);

    char* root = newRecord.data.get();
    mutablebson::DamageVector::const_iterator where = damages.begin();
    const mutablebson::DamageVector::const_iterator end = damages.end();
    for (; where != end; ++where) {
        const char* sourcePtr = damageSource + where->sourceOffset;
        char* targetPtr = root + where->targetOffset;
        std::memcpy(targetPtr, sourcePtr, where->size);
    }

    *oldRecord = newRecord;

    return newRecord.toRecordData();
}

std::unique_ptr<SeekableRecordCursor> EphemeralForTestRecordStore::getCursor(OperationContext* txn,
                                                                             bool forward) const {
    if (forward)
        return stdx::make_unique<Cursor>(txn, *this);
    return stdx::make_unique<ReverseCursor>(txn, *this);
}

Status EphemeralForTestRecordStore::truncate(OperationContext* txn) {
    // Unlike other changes, TruncateChange mutates _data on construction to perform the
    // truncate
    txn->recoveryUnit()->registerChange(new TruncateChange(_data));
    return Status::OK();
}

void EphemeralForTestRecordStore::temp_cappedTruncateAfter(OperationContext* txn,
                                                           RecordId end,
                                                           bool inclusive) {
    Records::iterator it =
        inclusive ? _data->records.lower_bound(end) : _data->records.upper_bound(end);
    while (it != _data->records.end()) {
        txn->recoveryUnit()->registerChange(new RemoveChange(_data, it->first, it->second));
        _data->dataSize -= it->second.size;
        _data->records.erase(it++);
    }
}

Status EphemeralForTestRecordStore::validate(OperationContext* txn,
                                             ValidateCmdLevel level,
                                             ValidateAdaptor* adaptor,
                                             ValidateResults* results,
                                             BSONObjBuilder* output) {
    results->valid = true;
    if (level == kValidateFull) {
        for (Records::const_iterator it = _data->records.begin(); it != _data->records.end();
             ++it) {
            const EphemeralForTestRecord& rec = it->second;
            size_t dataSize;
            const Status status = adaptor->validate(it->first, rec.toRecordData(), &dataSize);
            if (!status.isOK()) {
                if (results->valid) {
                    // Only log once.
                    results->errors.push_back("detected one or more invalid documents (see logs)");
                }
                results->valid = false;
                log() << "Invalid object detected in " << _ns << ": " << status.reason();
            }
        }
    }

    output->appendNumber("nrecords", _data->records.size());

    return Status::OK();
}

void EphemeralForTestRecordStore::appendCustomStats(OperationContext* txn,
                                                    BSONObjBuilder* result,
                                                    double scale) const {
    result->appendBool("capped", _isCapped);
    if (_isCapped) {
        result->appendIntOrLL("max", _cappedMaxDocs);
        result->appendIntOrLL("maxSize", _cappedMaxSize / scale);
    }
}

Status EphemeralForTestRecordStore::touch(OperationContext* txn, BSONObjBuilder* output) const {
    if (output) {
        output->append("numRanges", 1);
        output->append("millis", 0);
    }
    return Status::OK();
}

void EphemeralForTestRecordStore::increaseStorageSize(OperationContext* txn,
                                                      int size,
                                                      bool enforceQuota) {
    // unclear what this would mean for this class. For now, just error if called.
    invariant(!"increaseStorageSize not yet implemented");
}

int64_t EphemeralForTestRecordStore::storageSize(OperationContext* txn,
                                                 BSONObjBuilder* extraInfo,
                                                 int infoLevel) const {
    // Note: not making use of extraInfo or infoLevel since we don't have extents
    const int64_t recordOverhead = numRecords(txn) * sizeof(EphemeralForTestRecord);
    return _data->dataSize + recordOverhead;
}

RecordId EphemeralForTestRecordStore::allocateLoc() {
    RecordId out = RecordId(_data->nextId++);
    invariant(out < RecordId::max());
    return out;
}

boost::optional<RecordId> EphemeralForTestRecordStore::oplogStartHack(
    OperationContext* txn, const RecordId& startingPosition) const {
    if (!_data->isOplog)
        return boost::none;

    const Records& records = _data->records;

    if (records.empty())
        return RecordId();

    Records::const_iterator it = records.lower_bound(startingPosition);
    if (it == records.end() || it->first > startingPosition)
        --it;

    return it->first;
}

}  // namespace mongo
