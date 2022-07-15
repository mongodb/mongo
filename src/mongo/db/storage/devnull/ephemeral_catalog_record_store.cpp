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


#include "mongo/db/storage/devnull/ephemeral_catalog_record_store.h"

#include <memory>

#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/logv2/log.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {

using std::shared_ptr;

// Works for both removes and updates
class EphemeralForTestRecordStore::RemoveChange : public RecoveryUnit::Change {
public:
    RemoveChange(OperationContext* opCtx,
                 Data* data,
                 RecordId loc,
                 const EphemeralForTestRecord& rec)
        : _opCtx(opCtx), _data(data), _loc(loc), _rec(rec) {}

    virtual void commit(boost::optional<Timestamp>) {}
    virtual void rollback() {
        stdx::lock_guard<stdx::recursive_mutex> lock(_data->recordsMutex);

        Records::iterator it = _data->records.find(_loc);
        if (it != _data->records.end()) {
            _data->dataSize -= it->second.size;
        }

        _data->dataSize += _rec.size;
        _data->records[_loc] = _rec;
    }

private:
    OperationContext* _opCtx;
    Data* const _data;
    const RecordId _loc;
    const EphemeralForTestRecord _rec;
};

class EphemeralForTestRecordStore::TruncateChange : public RecoveryUnit::Change {
public:
    TruncateChange(OperationContext* opCtx, Data* data) : _opCtx(opCtx), _data(data), _dataSize(0) {
        using std::swap;

        stdx::lock_guard<stdx::recursive_mutex> lock(_data->recordsMutex);
        swap(_dataSize, _data->dataSize);
        swap(_records, _data->records);
    }

    virtual void commit(boost::optional<Timestamp>) {}
    virtual void rollback() {
        using std::swap;

        stdx::lock_guard<stdx::recursive_mutex> lock(_data->recordsMutex);
        swap(_dataSize, _data->dataSize);
        swap(_records, _data->records);
    }

private:
    OperationContext* _opCtx;
    Data* const _data;
    int64_t _dataSize;
    Records _records;
};

class EphemeralForTestRecordStore::Cursor final : public SeekableRecordCursor {
public:
    Cursor(OperationContext* opCtx, const EphemeralForTestRecordStore& rs)
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

    boost::optional<Record> seekNear(const RecordId& id) final {
        // not implemented
        return boost::none;
    }

    void save() final {
        if (!_needFirstSeek && !_lastMoveWasRestore)
            _savedId = _it == _records.end() ? RecordId() : _it->first;
    }

    void saveUnpositioned() final {
        _savedId = RecordId();
    }

    bool restore(bool tolerateCappedRepositioning) final {
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
    void reattachToOperationContext(OperationContext* opCtx) final {}
    void setSaveStorageCursorOnDetachFromOperationContext(bool) override {}

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
    ReverseCursor(OperationContext* opCtx, const EphemeralForTestRecordStore& rs)
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

    boost::optional<Record> seekNear(const RecordId& id) final {
        // not implemented
        return boost::none;
    }

    void save() final {
        if (!_needFirstSeek && !_lastMoveWasRestore)
            _savedId = _it == _records.rend() ? RecordId() : _it->first;
    }

    void saveUnpositioned() final {
        _savedId = RecordId();
    }

    bool restore(bool tolerateCappedRepositioning) final {
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
    void reattachToOperationContext(OperationContext* opCtx) final {}
    void setSaveStorageCursorOnDetachFromOperationContext(bool) override {}

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
                                                         StringData identName,
                                                         std::shared_ptr<void>* dataInOut,
                                                         bool isCapped,
                                                         CappedCallback* cappedCallback)
    : RecordStore(ns, identName),
      _isCapped(isCapped),
      _cappedCallback(cappedCallback),
      _data(*dataInOut ? static_cast<Data*>(dataInOut->get())
                       : new Data(ns, NamespaceString::oplog(ns))) {
    if (!*dataInOut) {
        dataInOut->reset(_data);  // takes ownership
    }
}

const char* EphemeralForTestRecordStore::name() const {
    return "EphemeralForTest";
}

RecordData EphemeralForTestRecordStore::dataFor(OperationContext* opCtx,
                                                const RecordId& loc) const {
    stdx::lock_guard<stdx::recursive_mutex> lock(_data->recordsMutex);
    return recordFor(lock, loc)->toRecordData();
}

const EphemeralForTestRecordStore::EphemeralForTestRecord* EphemeralForTestRecordStore::recordFor(
    WithLock, const RecordId& loc) const {
    Records::const_iterator it = _data->records.find(loc);
    if (it == _data->records.end()) {
        LOGV2_ERROR(
            23720,
            "EphemeralForTestRecordStore::recordFor cannot find record for {namespace}:{loc}",
            logAttrs(NamespaceString(ns())),
            "loc"_attr = loc);
    }
    invariant(it != _data->records.end());
    return &it->second;
}

EphemeralForTestRecordStore::EphemeralForTestRecord* EphemeralForTestRecordStore::recordFor(
    WithLock, const RecordId& loc) {
    Records::iterator it = _data->records.find(loc);
    if (it == _data->records.end()) {
        LOGV2_ERROR(
            23721,
            "EphemeralForTestRecordStore::recordFor cannot find record for {namespace}:{loc}",
            logAttrs(NamespaceString(ns())),
            "loc"_attr = loc);
    }
    invariant(it != _data->records.end());
    return &it->second;
}

bool EphemeralForTestRecordStore::findRecord(OperationContext* opCtx,
                                             const RecordId& loc,
                                             RecordData* rd) const {
    stdx::lock_guard<stdx::recursive_mutex> lock(_data->recordsMutex);
    Records::const_iterator it = _data->records.find(loc);
    if (it == _data->records.end()) {
        return false;
    }
    *rd = it->second.toRecordData();
    return true;
}

void EphemeralForTestRecordStore::doDeleteRecord(OperationContext* opCtx, const RecordId& loc) {
    stdx::lock_guard<stdx::recursive_mutex> lock(_data->recordsMutex);

    deleteRecord(lock, opCtx, loc);
}

void EphemeralForTestRecordStore::deleteRecord(WithLock lk,
                                               OperationContext* opCtx,
                                               const RecordId& loc) {
    EphemeralForTestRecord* rec = recordFor(lk, loc);
    opCtx->recoveryUnit()->registerChange(std::make_unique<RemoveChange>(opCtx, _data, loc, *rec));
    _data->dataSize -= rec->size;
    invariant(_data->records.erase(loc) == 1);
}

StatusWith<RecordId> EphemeralForTestRecordStore::extractAndCheckLocForOplog(WithLock,
                                                                             const char* data,
                                                                             int len) const {
    StatusWith<RecordId> status = record_id_helpers::extractKeyOptime(data, len);
    if (!status.isOK())
        return status;

    if (!_data->records.empty() && status.getValue() <= _data->records.rbegin()->first) {

        return StatusWith<RecordId>(ErrorCodes::BadValue,
                                    str::stream() << "attempted out-of-order oplog insert of "
                                                  << status.getValue() << " (oplog last insert was "
                                                  << _data->records.rbegin()->first << " )");
    }
    return status;
}

Status EphemeralForTestRecordStore::doInsertRecords(OperationContext* opCtx,
                                                    std::vector<Record>* inOutRecords,
                                                    const std::vector<Timestamp>& timestamps) {
    const auto insertSingleFn = [this, opCtx](Record* record) {
        stdx::lock_guard<stdx::recursive_mutex> lock(_data->recordsMutex);
        EphemeralForTestRecord rec(record->data.size());
        memcpy(rec.data.get(), record->data.data(), record->data.size());

        RecordId loc;
        if (_data->isOplog) {
            StatusWith<RecordId> status =
                extractAndCheckLocForOplog(lock, record->data.data(), record->data.size());
            if (!status.isOK())
                return status.getStatus();
            loc = std::move(status.getValue());
        } else {
            loc = allocateLoc(lock);
        }

        _data->dataSize += record->data.size();
        _data->records[loc] = rec;
        record->id = loc;

        opCtx->recoveryUnit()->onRollback([this, loc = std::move(loc)]() {
            stdx::lock_guard<stdx::recursive_mutex> lock(_data->recordsMutex);

            Records::iterator it = _data->records.find(loc);
            if (it != _data->records.end()) {
                _data->dataSize -= it->second.size;
                _data->records.erase(it);
            }
        });
        return Status::OK();
    };

    for (auto& record : *inOutRecords) {
        auto status = insertSingleFn(&record);
        if (!status.isOK())
            return status;
    }

    return Status::OK();
}

Status EphemeralForTestRecordStore::doUpdateRecord(OperationContext* opCtx,
                                                   const RecordId& loc,
                                                   const char* data,
                                                   int len) {
    stdx::lock_guard<stdx::recursive_mutex> lock(_data->recordsMutex);
    EphemeralForTestRecord* oldRecord = recordFor(lock, loc);
    int oldLen = oldRecord->size;

    EphemeralForTestRecord newRecord(len);
    memcpy(newRecord.data.get(), data, len);

    opCtx->recoveryUnit()->registerChange(
        std::make_unique<RemoveChange>(opCtx, _data, loc, *oldRecord));
    _data->dataSize += len - oldLen;
    *oldRecord = newRecord;
    return Status::OK();
}

bool EphemeralForTestRecordStore::updateWithDamagesSupported() const {
    return true;
}

StatusWith<RecordData> EphemeralForTestRecordStore::doUpdateWithDamages(
    OperationContext* opCtx,
    const RecordId& loc,
    const RecordData& oldRec,
    const char* damageSource,
    const mutablebson::DamageVector& damages) {

    stdx::lock_guard<stdx::recursive_mutex> lock(_data->recordsMutex);

    EphemeralForTestRecord* oldRecord = recordFor(lock, loc);
    const int len = std::accumulate(
        damages.begin(), damages.end(), oldRecord->size, [](int bytes, const auto& damage) {
            return bytes + damage.sourceSize - damage.targetSize;
        });

    EphemeralForTestRecord newRecord(len);

    opCtx->recoveryUnit()->registerChange(
        std::make_unique<RemoveChange>(opCtx, _data, loc, *oldRecord));

    char* root = newRecord.data.get();
    char* old = oldRecord->data.get();
    mutablebson::DamageVector::const_iterator where = damages.begin();
    const mutablebson::DamageVector::const_iterator end = damages.end();
    // Since the 'targetOffset' is referring to the location in the new record, we need to subtract
    // the accumulated change of size by the damages to get the offset in the old record.
    int diffSize = 0;
    int curSize = 0;
    int oldOffset = 0;
    for (; where != end; ++where) {
        // First copies all bytes before the damage from the old record.
        // Bytes between the current location in the oldRecord and the start of the damage.
        auto oldSize = (where->targetOffset - diffSize) - oldOffset;
        std::memcpy(root + curSize, old + oldOffset, oldSize);

        // Then copies from the damage source according to the damage event info.
        const char* sourcePtr = damageSource + where->sourceOffset;
        char* targetPtr = root + where->targetOffset;
        std::memcpy(targetPtr, sourcePtr, where->sourceSize);

        // Moves after the current damaged area in the old record. Updates the size difference due
        // to the current damage. Increases the current size in the new record.
        oldOffset = where->targetOffset - diffSize + where->targetSize;
        diffSize += where->sourceSize - where->targetSize;
        curSize += oldSize + where->sourceSize;
    }
    // Copies the rest of the old record.
    std::memcpy(root + curSize, old + oldOffset, oldRecord->size - oldOffset);

    *oldRecord = newRecord;

    return newRecord.toRecordData();
}

std::unique_ptr<SeekableRecordCursor> EphemeralForTestRecordStore::getCursor(
    OperationContext* opCtx, bool forward) const {
    if (forward)
        return std::make_unique<Cursor>(opCtx, *this);
    return std::make_unique<ReverseCursor>(opCtx, *this);
}

Status EphemeralForTestRecordStore::doTruncate(OperationContext* opCtx) {
    // Unlike other changes, TruncateChange mutates _data on construction to perform the
    // truncate
    opCtx->recoveryUnit()->registerChange(std::make_unique<TruncateChange>(opCtx, _data));
    return Status::OK();
}

void EphemeralForTestRecordStore::doCappedTruncateAfter(OperationContext* opCtx,
                                                        const RecordId& end,
                                                        bool inclusive) {
    stdx::lock_guard<stdx::recursive_mutex> lock(_data->recordsMutex);
    Records::iterator it =
        inclusive ? _data->records.lower_bound(end) : _data->records.upper_bound(end);
    while (it != _data->records.end()) {
        auto& id = it->first;
        EphemeralForTestRecord record = it->second;

        if (_cappedCallback) {
            uassertStatusOK(_cappedCallback->aboutToDeleteCapped(opCtx, id, record.toRecordData()));
        }

        opCtx->recoveryUnit()->registerChange(
            std::make_unique<RemoveChange>(opCtx, _data, id, record));
        _data->dataSize -= record.size;
        _data->records.erase(it++);
    }
}

int64_t EphemeralForTestRecordStore::storageSize(OperationContext* opCtx,
                                                 BSONObjBuilder* extraInfo,
                                                 int infoLevel) const {
    // Note: not making use of extraInfo or infoLevel since we don't have extents
    const int64_t recordOverhead = numRecords(opCtx) * sizeof(EphemeralForTestRecord);
    return _data->dataSize + recordOverhead;
}

RecordId EphemeralForTestRecordStore::allocateLoc(WithLock) {
    RecordId out = RecordId(_data->nextId++);
    invariant(out.isValid());
    return out;
}
}  // namespace mongo
