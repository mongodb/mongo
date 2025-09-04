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

#include "mongo/base/error_codes.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/storage/damage_vector.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/logv2/log.h"
#include "mongo/util/str.h"

#include <cstring>
#include <iterator>
#include <memory>
#include <numeric>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/shared_array.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {

using std::shared_ptr;

// Works for both removes and updates
class EphemeralForTestRecordStore::RemoveChange : public RecoveryUnit::Change {
public:
    RemoveChange(Data* data, RecordId loc, const EphemeralForTestRecord& rec)
        : _data(data), _loc(loc), _rec(rec) {}

    void commit(OperationContext* opCtx, boost::optional<Timestamp>) noexcept override {}
    void rollback(OperationContext* opCtx) noexcept override {
        stdx::lock_guard<stdx::recursive_mutex> lock(_data->recordsMutex);

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
    TruncateChange(OperationContext* opCtx, Data* data, const RecordId& begin, const RecordId& end)
        : _opCtx(opCtx), _data(data), _dataSize(0) {
        stdx::lock_guard<stdx::recursive_mutex> lock(_data->recordsMutex);

        for (auto it = _data->records.begin(); it != _data->records.end();) {
            if (it->first >= begin && it->first <= end) {
                _deletedRecords.try_emplace(it->first, it->second);
                _dataSize += it->first.memUsage() + it->second.size;
                it = _data->records.erase(it);
            } else {
                it++;
            }
        }
        _data->dataSize -= _dataSize;
    }

    void commit(OperationContext* opCtx, boost::optional<Timestamp>) noexcept override {}
    void rollback(OperationContext* opCtx) noexcept override {
        using std::swap;

        stdx::lock_guard<stdx::recursive_mutex> lock(_data->recordsMutex);
        _data->records.merge(std::move(_deletedRecords));
        _data->dataSize += _dataSize;
    }

private:
    OperationContext* _opCtx;
    Records _deletedRecords;
    Data* const _data;
    int64_t _dataSize;
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

    boost::optional<Record> seek(const RecordId& start, BoundInclusion boundInclusion) override {
        // not implemented
        return {};
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

    bool restore(RecoveryUnit& ru, bool tolerateCappedRepositioning) final {
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

    boost::optional<Record> seek(const RecordId& start, BoundInclusion boundInclusion) override {
        // not implemented
        return {};
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

    bool restore(RecoveryUnit& ru, bool tolerateCappedRepositioning) final {
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

std::variant<EphemeralForTestIntegerKeyedContainer, EphemeralForTestStringKeyedContainer>
EphemeralForTestRecordStore::_makeContainer() {
    auto container = EphemeralForTestIntegerKeyedContainer();
    return container;
}

EphemeralForTestRecordStore::EphemeralForTestRecordStore(boost::optional<UUID> uuid,
                                                         StringData identName,
                                                         std::shared_ptr<void>* dataInOut,
                                                         bool isCapped,
                                                         bool isOplog)
    : RecordStoreBase(uuid, identName),
      _container(_makeContainer()),
      _isCapped(isCapped),
      _data(*dataInOut ? static_cast<Data*>(dataInOut->get()) : new Data(isOplog)) {
    // NOTE : The static_cast here assumes that `dataInOut`, which is a void pointer. As of now,
    // DevNullKVEngine constructs a EphemeralForTestRecordStore by passing `_catalogInfo` to this
    // method.
    if (!*dataInOut) {
        dataInOut->reset(_data);  // takes ownership
    }
}

const char* EphemeralForTestRecordStore::name() const {
    return "EphemeralForTest";
}

const EphemeralForTestRecordStore::EphemeralForTestRecord* EphemeralForTestRecordStore::recordFor(
    WithLock, const RecordId& loc) const {
    Records::const_iterator it = _data->records.find(loc);
    if (it == _data->records.end()) {
        LOGV2_ERROR(23720,
                    "EphemeralForTestRecordStore::recordFor cannot find record",
                    "uuid"_attr = uuid(),
                    "loc"_attr = loc);
    }
    invariant(it != _data->records.end());
    return &it->second;
}

EphemeralForTestRecordStore::EphemeralForTestRecord* EphemeralForTestRecordStore::recordFor(
    WithLock, const RecordId& loc) {
    Records::iterator it = _data->records.find(loc);
    if (it == _data->records.end()) {
        LOGV2_ERROR(23721,
                    "EphemeralForTestRecordStore::recordFor cannot find record",
                    "uuid"_attr = uuid(),
                    "loc"_attr = loc);
    }
    invariant(it != _data->records.end());
    return &it->second;
}

void EphemeralForTestRecordStore::_deleteRecord(OperationContext* opCtx,
                                                RecoveryUnit& ru,
                                                const RecordId& loc) {
    stdx::lock_guard<stdx::recursive_mutex> lock(_data->recordsMutex);

    EphemeralForTestRecord* rec = recordFor(lock, loc);
    ru.registerChange(std::make_unique<RemoveChange>(_data, loc, *rec));
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

Status EphemeralForTestRecordStore::_insertRecords(OperationContext* opCtx,
                                                   RecoveryUnit& ru,
                                                   std::vector<Record>* inOutRecords,
                                                   const std::vector<Timestamp>& timestamps) {
    const auto insertSingleFn = [this, opCtx, &ru](Record* record) {
        stdx::lock_guard<stdx::recursive_mutex> lock(_data->recordsMutex);
        EphemeralForTestRecord rec(record->data.size());
        memcpy(rec.data.get(), record->data.data(), record->data.size());

        if (record->id.isNull()) {
            if (_data->isOplog) {
                StatusWith<RecordId> status =
                    extractAndCheckLocForOplog(lock, record->data.data(), record->data.size());
                if (!status.isOK())
                    return status.getStatus();
                record->id = std::move(status.getValue());
            } else {
                record->id = allocateLoc(lock);
            }
        }
        _data->dataSize += record->data.size();
        _data->records[record->id] = rec;

        ru.onRollback([this, loc = record->id](OperationContext*) {
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

Status EphemeralForTestRecordStore::_updateRecord(
    OperationContext* opCtx, RecoveryUnit& ru, const RecordId& loc, const char* data, int len) {
    stdx::lock_guard<stdx::recursive_mutex> lock(_data->recordsMutex);
    EphemeralForTestRecord* oldRecord = recordFor(lock, loc);
    int oldLen = oldRecord->size;

    EphemeralForTestRecord newRecord(len);
    memcpy(newRecord.data.get(), data, len);

    ru.registerChange(std::make_unique<RemoveChange>(_data, loc, *oldRecord));
    _data->dataSize += len - oldLen;
    *oldRecord = newRecord;
    return Status::OK();
}

bool EphemeralForTestRecordStore::updateWithDamagesSupported() const {
    return true;
}

StatusWith<RecordData> EphemeralForTestRecordStore::_updateWithDamages(
    OperationContext* opCtx,
    RecoveryUnit& ru,
    const RecordId& loc,
    const RecordData& oldRec,
    const char* damageSource,
    const DamageVector& damages) {

    stdx::lock_guard<stdx::recursive_mutex> lock(_data->recordsMutex);

    EphemeralForTestRecord* oldRecord = recordFor(lock, loc);
    const int len = std::accumulate(
        damages.begin(), damages.end(), oldRecord->size, [](int bytes, const auto& damage) {
            return bytes + damage.sourceSize - damage.targetSize;
        });

    EphemeralForTestRecord newRecord(len);

    ru.registerChange(std::make_unique<RemoveChange>(_data, loc, *oldRecord));

    char* root = newRecord.data.get();
    char* old = oldRecord->data.get();
    DamageVector::const_iterator where = damages.begin();
    const DamageVector::const_iterator end = damages.end();
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
    OperationContext* opCtx, RecoveryUnit& ru, bool forward) const {
    if (forward)
        return std::make_unique<Cursor>(opCtx, *this);
    return std::make_unique<ReverseCursor>(opCtx, *this);
}

std::unique_ptr<RecordCursor> EphemeralForTestRecordStore::getRandomCursor(OperationContext*,
                                                                           RecoveryUnit&) const {
    return {};
}

Status EphemeralForTestRecordStore::_truncate(OperationContext* opCtx, RecoveryUnit& ru) {
    // Unlike other changes, TruncateChange mutates _data on construction to perform the
    // truncate
    ru.registerChange(
        std::make_unique<TruncateChange>(opCtx, _data, RecordId::minLong(), RecordId::maxLong()));
    return Status::OK();
}

Status EphemeralForTestRecordStore::_rangeTruncate(OperationContext* opCtx,
                                                   RecoveryUnit& ru,
                                                   const RecordId& minRecordId,
                                                   const RecordId& maxRecordId,
                                                   int64_t hintDataSizeDiff,
                                                   int64_t hintNumRecordsDiff) {
    // Unlike other changes, TruncateChange mutates _data on construction to perform the
    // truncate.
    ru.registerChange(std::make_unique<TruncateChange>(opCtx, _data, minRecordId, maxRecordId));
    return Status::OK();
}

bool EphemeralForTestRecordStore::compactSupported() const {
    return false;
}

StatusWith<int64_t> EphemeralForTestRecordStore::_compact(OperationContext*,
                                                          RecoveryUnit&,
                                                          const CompactOptions&) {
    return Status::OK();
}

void EphemeralForTestRecordStore::validate(RecoveryUnit&,
                                           const CollectionValidation::ValidationOptions&,
                                           ValidateResults*) {}

void EphemeralForTestRecordStore::reserveRecordIds(OperationContext* opCtx,
                                                   RecoveryUnit& ru,
                                                   std::vector<RecordId>* out,
                                                   size_t nRecords) {
    stdx::lock_guard<stdx::recursive_mutex> lock(_data->recordsMutex);
    for (size_t i = 0; i < nRecords; i++) {
        out->push_back(allocateLoc(lock));
    }
}

int64_t EphemeralForTestRecordStore::storageSize(RecoveryUnit&,
                                                 BSONObjBuilder* extraInfo,
                                                 int infoLevel) const {
    // Note: not making use of extraInfo or infoLevel since we don't have extents
    const int64_t recordOverhead = numRecords() * sizeof(EphemeralForTestRecord);
    return _data->dataSize + recordOverhead;
}

int64_t EphemeralForTestRecordStore::freeStorageSize(RecoveryUnit&) const {
    return 0;
}

RecordId EphemeralForTestRecordStore::allocateLoc(WithLock) {
    RecordId out = RecordId(_data->nextId++);
    invariant(out.isValid());
    return out;
}

RecordStore::RecordStoreContainer EphemeralForTestRecordStore::getContainer() {
    return std::visit([](auto& v) -> RecordStore::RecordStoreContainer { return v; }, _container);
}

}  // namespace mongo
