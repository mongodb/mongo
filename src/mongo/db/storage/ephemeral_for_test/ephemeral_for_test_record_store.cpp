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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/storage/ephemeral_for_test/ephemeral_for_test_record_store.h"

#include <cstring>
#include <memory>
#include <utility>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/storage/ephemeral_for_test/ephemeral_for_test_radix_store.h"
#include "mongo/db/storage/ephemeral_for_test/ephemeral_for_test_recovery_unit.h"
#include "mongo/db/storage/ephemeral_for_test/ephemeral_for_test_visibility_manager.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/logv2/log.h"
#include "mongo/util/hex.h"

namespace mongo {
namespace ephemeral_for_test {
namespace {
Ordering allAscending = Ordering::make(BSONObj());
auto const version = KeyString::Version::kLatestVersion;
BSONObj const sample = BSON(""
                            << "s"
                            << "" << (int64_t)0);

std::string createKey(StringData ident, RecordId recordId) {
    KeyString::Builder ks(version, BSON("" << ident), allAscending, recordId);
    return std::string(ks.getBuffer(), ks.getSize());
}

std::string createPrefix(StringData ident) {
    KeyString::Builder ks(
        version, BSON("" << ident), allAscending, KeyString::Discriminator::kExclusiveBefore);
    return std::string(ks.getBuffer(), ks.getSize());
}

std::string createPostfix(StringData ident) {
    KeyString::Builder ks(
        version, BSON("" << ident), allAscending, KeyString::Discriminator::kExclusiveAfter);
    return std::string(ks.getBuffer(), ks.getSize());
}

RecordId extractRecordId(const std::string& keyStr, KeyFormat keyFormat) {
    if (KeyFormat::Long == keyFormat) {
        return KeyString::decodeRecordIdLongAtEnd(keyStr.c_str(), keyStr.size());
    } else {
        invariant(KeyFormat::String == keyFormat);
        return KeyString::decodeRecordIdStrAtEnd(keyStr.c_str(), keyStr.size());
    }
}
}  // namespace

RecordStore::RecordStore(StringData ns,
                         StringData ident,
                         KeyFormat keyFormat,
                         bool isCapped,
                         CappedCallback* cappedCallback,
                         VisibilityManager* visibilityManager)
    : mongo::RecordStore(ns, ident),
      _keyFormat(keyFormat),
      _isCapped(isCapped),
      _ident(getIdent().data(), getIdent().size()),
      _prefix(createPrefix(_ident)),
      _postfix(createPostfix(_ident)),
      _cappedCallback(cappedCallback),
      _isOplog(NamespaceString::oplog(ns)),
      _visibilityManager(visibilityManager) {}

const char* RecordStore::name() const {
    return kEngineName;
}

long long RecordStore::dataSize(OperationContext* opCtx) const {
    return _dataSize.load();
}

long long RecordStore::numRecords(OperationContext* opCtx) const {
    return static_cast<long long>(_numRecords.load());
}

void RecordStore::setCappedCallback(CappedCallback* cb) {
    stdx::lock_guard<Latch> cappedCallbackLock(_cappedCallbackMutex);
    _cappedCallback = cb;
}

int64_t RecordStore::storageSize(OperationContext* opCtx,
                                 BSONObjBuilder* extraInfo,
                                 int infoLevel) const {
    return dataSize(opCtx);
}

bool RecordStore::findRecord(OperationContext* opCtx, const RecordId& loc, RecordData* rd) const {
    StringStore* workingCopy(RecoveryUnit::get(opCtx)->getHead());
    auto it = workingCopy->find(createKey(_ident, loc));
    if (it == workingCopy->end()) {
        return false;
    }
    *rd = RecordData(it->second.c_str(), it->second.length()).getOwned();
    return true;
}

void RecordStore::deleteRecord(OperationContext* opCtx, const RecordId& dl) {
    if (KeyFormat::Long == _keyFormat) {
        _initHighestIdIfNeeded(opCtx);
    }
    auto ru = RecoveryUnit::get(opCtx);
    StringStore* workingCopy(ru->getHead());
    SizeAdjuster adjuster(opCtx, this);
    invariant(workingCopy->erase(createKey(_ident, dl)));
    ru->makeDirty();
}

Status RecordStore::insertRecords(OperationContext* opCtx,
                                  std::vector<Record>* inOutRecords,
                                  const std::vector<Timestamp>& timestamps) {
    auto ru = RecoveryUnit::get(opCtx);
    StringStore* workingCopy(ru->getHead());
    {
        SizeAdjuster adjuster(opCtx, this);
        for (auto& record : *inOutRecords) {
            auto thisRecordId = record.id;
            if (_isOplog) {
                StatusWith<RecordId> status =
                    record_id_helpers::extractKeyOptime(record.data.data(), record.data.size());
                if (!status.isOK())
                    return status.getStatus();
                thisRecordId = status.getValue();
                _visibilityManager->addUncommittedRecord(opCtx, this, thisRecordId);
            } else {
                // If the record had an id already, keep it.
                if (KeyFormat::Long == _keyFormat && thisRecordId.isNull()) {
                    thisRecordId = RecordId(_nextRecordId(opCtx));
                }
            }
            auto key = createKey(_ident, thisRecordId);
            auto it = workingCopy->find(key);
            if (keyFormat() == KeyFormat::String && it != workingCopy->end()) {
                // RecordStores with the string KeyFormat implicitly expect enforcement of _id
                // uniqueness. Generate a useful error message that is consistent with duplicate key
                // error messages on indexes.
                BSONObj obj = record_id_helpers::toBSONAs(thisRecordId, "");
                return buildDupKeyErrorStatus(obj,
                                              NamespaceString(ns()),
                                              "" /* indexName */,
                                              BSON("_id" << 1),
                                              BSONObj() /* collation */);
            }

            workingCopy->insert(
                StringStore::value_type{key, std::string(record.data.data(), record.data.size())});
            record.id = RecordId(thisRecordId);
        }
    }
    ru->makeDirty();
    return Status::OK();
}

Status RecordStore::updateRecord(OperationContext* opCtx,
                                 const RecordId& oldLocation,
                                 const char* data,
                                 int len) {
    StringStore* workingCopy(RecoveryUnit::get(opCtx)->getHead());
    SizeAdjuster adjuster(opCtx, this);
    {
        std::string key = createKey(_ident, oldLocation);
        StringStore::const_iterator it = workingCopy->find(key);
        invariant(it != workingCopy->end());
        workingCopy->update(StringStore::value_type{key, std::string(data, len)});
    }
    RecoveryUnit::get(opCtx)->makeDirty();

    return Status::OK();
}

bool RecordStore::updateWithDamagesSupported() const {
    // TODO: enable updateWithDamages after writable pointers are complete.
    return false;
}

StatusWith<RecordData> RecordStore::updateWithDamages(OperationContext* opCtx,
                                                      const RecordId& loc,
                                                      const RecordData& oldRec,
                                                      const char* damageSource,
                                                      const mutablebson::DamageVector& damages) {
    return RecordData();
}

std::unique_ptr<SeekableRecordCursor> RecordStore::getCursor(OperationContext* opCtx,
                                                             bool forward) const {
    if (forward)
        return std::make_unique<Cursor>(opCtx, *this, _visibilityManager);
    return std::make_unique<ReverseCursor>(opCtx, *this, _visibilityManager);
}

Status RecordStore::truncate(OperationContext* opCtx) {
    SizeAdjuster adjuster(opCtx, this);
    StatusWith<int64_t> s = truncateWithoutUpdatingCount(
        checked_cast<ephemeral_for_test::RecoveryUnit*>(opCtx->recoveryUnit()));
    if (!s.isOK())
        return s.getStatus();

    return Status::OK();
}

StatusWith<int64_t> RecordStore::truncateWithoutUpdatingCount(mongo::RecoveryUnit* ru) {
    auto bRu = checked_cast<ephemeral_for_test::RecoveryUnit*>(ru);
    StringStore* workingCopy(bRu->getHead());
    StringStore::const_iterator end = workingCopy->upper_bound(_postfix);
    std::vector<std::string> toDelete;

    for (auto it = workingCopy->lower_bound(_prefix); it != end; ++it) {
        toDelete.push_back(it->first);
    }

    if (toDelete.empty())
        return 0;

    for (const auto& key : toDelete)
        workingCopy->erase(key);

    bRu->makeDirty();

    return static_cast<int64_t>(toDelete.size());
}

void RecordStore::cappedTruncateAfter(OperationContext* opCtx, RecordId end, bool inclusive) {
    auto ru = RecoveryUnit::get(opCtx);
    StringStore* workingCopy(ru->getHead());
    WriteUnitOfWork wuow(opCtx);
    const auto recordKey = createKey(_ident, end);
    auto recordIt =
        inclusive ? workingCopy->lower_bound(recordKey) : workingCopy->upper_bound(recordKey);
    auto endIt = workingCopy->upper_bound(_postfix);

    while (recordIt != endIt) {
        stdx::lock_guard<Latch> cappedCallbackLock(_cappedCallbackMutex);
        if (_cappedCallback) {
            // Documents are guaranteed to have a RecordId at the end of the KeyString, unlike
            // unique indexes.
            RecordId rid = extractRecordId(recordIt->first, _keyFormat);
            RecordData rd = RecordData(recordIt->second.c_str(), recordIt->second.length());
            uassertStatusOK(_cappedCallback->aboutToDeleteCapped(opCtx, rid, rd));
        }
        // Important to scope adjuster until after capped callback, as that changes indexes and
        // would result in those changes being reflected in RecordStore count/size.
        SizeAdjuster adjuster(opCtx, this);

        // Don't need to increment the iterator because the iterator gets revalidated and placed on
        // the next item after the erase.
        workingCopy->erase(recordIt->first);

        // Tree modifications are bound to happen here so we need to reposition our end cursor.
        endIt.repositionIfChanged();
        ru->makeDirty();
    }

    wuow.commit();
}

void RecordStore::updateStatsAfterRepair(OperationContext* opCtx,
                                         long long numRecords,
                                         long long dataSize) {
    // SERVER-38883 This storage engine should instead be able to invariant that stats are correct.
    _numRecords.store(numRecords);
    _dataSize.store(dataSize);
}

void RecordStore::waitForAllEarlierOplogWritesToBeVisible(OperationContext* opCtx) const {
    _visibilityManager->waitForAllEarlierOplogWritesToBeVisible(opCtx);
}

Status RecordStore::oplogDiskLocRegister(OperationContext* opCtx,
                                         const Timestamp& opTime,
                                         bool orderedCommit) {
    if (!orderedCommit) {
        return opCtx->recoveryUnit()->setTimestamp(opTime);
    }

    return Status::OK();
}

void RecordStore::_initHighestIdIfNeeded(OperationContext* opCtx) {
    // In the normal case, this will already be initialized, so use a weak load. Since this value
    // will only change from 0 to a positive integer, the only risk is reading an outdated value, 0,
    // and having to take the mutex.
    if (_highestRecordId.loadRelaxed() > 0) {
        return;
    }

    // Only one thread needs to do this.
    stdx::lock_guard<Latch> lk(_initHighestIdMutex);
    if (_highestRecordId.load() > 0) {
        return;
    }

    // Need to start at 1 so we are always higher than RecordId::minLong()
    int64_t nextId = 1;

    // Find the largest RecordId currently in use.
    std::unique_ptr<SeekableRecordCursor> cursor = getCursor(opCtx, /*forward=*/false);
    if (auto record = cursor->next()) {
        nextId = record->id.getLong() + 1;
    }

    _highestRecordId.store(nextId);
};

int64_t RecordStore::_nextRecordId(OperationContext* opCtx) {
    _initHighestIdIfNeeded(opCtx);
    return _highestRecordId.fetchAndAdd(1);
}

RecordStore::Cursor::Cursor(OperationContext* opCtx,
                            const RecordStore& rs,
                            VisibilityManager* visibilityManager)
    : opCtx(opCtx), _rs(rs), _visibilityManager(visibilityManager) {
    if (_rs._isOplog) {
        _oplogVisibility = _visibilityManager->getAllCommittedRecord();
    }
}

boost::optional<Record> RecordStore::Cursor::next() {
    // Capped iterators die on invalidation rather than advancing.
    if (_rs._isCapped && _lastMoveWasRestore) {
        return boost::none;
    }

    _savedPosition = boost::none;
    StringStore* workingCopy(RecoveryUnit::get(opCtx)->getHead());
    if (_needFirstSeek) {
        _needFirstSeek = false;
        it = workingCopy->lower_bound(_rs._prefix);
    } else if (it != workingCopy->end() && !_lastMoveWasRestore) {
        ++it;
    }
    _lastMoveWasRestore = false;
    if (it != workingCopy->end() && inPrefix(it->first)) {
        _savedPosition = it->first;
        Record nextRecord;
        nextRecord.id = RecordId(extractRecordId(it->first, _rs._keyFormat));
        nextRecord.data = RecordData(it->second.c_str(), it->second.length());

        if (_rs._isOplog && nextRecord.id > _oplogVisibility) {
            return boost::none;
        }

        return nextRecord;
    }
    return boost::none;
}

boost::optional<Record> RecordStore::Cursor::seekExact(const RecordId& id) {
    _savedPosition = boost::none;
    _lastMoveWasRestore = false;
    StringStore* workingCopy(RecoveryUnit::get(opCtx)->getHead());
    std::string key = createKey(_rs._ident, id);
    it = workingCopy->find(key);

    if (it == workingCopy->end() || !inPrefix(it->first))
        return boost::none;

    if (_rs._isOplog && id > _oplogVisibility) {
        return boost::none;
    }

    _needFirstSeek = false;
    _savedPosition = it->first;
    return Record{id, RecordData(it->second.c_str(), it->second.length())};
}

boost::optional<Record> RecordStore::Cursor::seekNear(const RecordId& id) {
    _savedPosition = boost::none;
    _lastMoveWasRestore = false;

    RecordId search = id;
    if (_rs._isOplog && id > _oplogVisibility) {
        search = RecordId(_oplogVisibility);
    }

    auto numRecords = _rs.numRecords(opCtx);
    if (numRecords == 0)
        return boost::none;

    StringStore* workingCopy{RecoveryUnit::get(opCtx)->getHead()};
    std::string key = createKey(_rs._ident, search);
    // We may land higher and that is fine per the API contract.
    it = workingCopy->lower_bound(key);

    // If we're at the end of this record store, we didn't find anything >= id. Position on the
    // immediately previous record, which must exist.
    if (it == workingCopy->end() || !inPrefix(it->first)) {
        // The reverse iterator constructor positions on the next record automatically.
        StringStore::const_reverse_iterator revIt(it);
        invariant(revIt != workingCopy->rend());
        it = workingCopy->lower_bound(revIt->first);
        invariant(it != workingCopy->end());
        invariant(inPrefix(it->first));
    }

    // If we landed one higher, then per the API contract, we need to return the previous record.
    RecordId rid = extractRecordId(it->first, _rs._keyFormat);
    if (rid > search) {
        StringStore::const_reverse_iterator revIt(it);
        // The reverse iterator constructor positions on the next record automatically.
        if (revIt != workingCopy->rend() && inPrefix(revIt->first)) {
            it = workingCopy->lower_bound(revIt->first);
            rid = RecordId(extractRecordId(it->first, _rs._keyFormat));
        }
        // Otherwise, we hit the beginning of this record store, then there is only one record and
        // we should return that.
    }

    // For forward cursors on the oplog, the oplog visible timestamp is treated as the end of the
    // record store. So if we are positioned past this point, then there are no visible records.
    if (_rs._isOplog && rid > _oplogVisibility) {
        return boost::none;
    }

    _needFirstSeek = false;
    _savedPosition = it->first;
    return Record{rid, RecordData(it->second.c_str(), it->second.length())};
}

// Positions are saved as we go.
void RecordStore::Cursor::save() {}
void RecordStore::Cursor::saveUnpositioned() {
    _savedPosition = boost::none;
}

bool RecordStore::Cursor::restore() {
    if (!_savedPosition)
        return true;

    // Get oplog visibility before forking working tree to guarantee that nothing gets committed
    // after we've forked that would update oplog visibility
    if (_rs._isOplog) {
        _oplogVisibility = _visibilityManager->getAllCommittedRecord();
    }

    StringStore* workingCopy(RecoveryUnit::get(opCtx)->getHead());
    it = workingCopy->lower_bound(_savedPosition.value());
    _lastMoveWasRestore = it == workingCopy->end() || it->first != _savedPosition.value();

    // Capped iterators die on invalidation rather than advancing.
    return !(_rs._isCapped && _lastMoveWasRestore);
}

void RecordStore::Cursor::detachFromOperationContext() {
    invariant(opCtx != nullptr);
    opCtx = nullptr;
}

void RecordStore::Cursor::reattachToOperationContext(OperationContext* opCtx) {
    invariant(opCtx != nullptr);
    this->opCtx = opCtx;
}

bool RecordStore::Cursor::inPrefix(const std::string& key_string) {
    return (key_string > _rs._prefix) && (key_string < _rs._postfix);
}

RecordStore::ReverseCursor::ReverseCursor(OperationContext* opCtx,
                                          const RecordStore& rs,
                                          VisibilityManager* visibilityManager)
    : opCtx(opCtx), _rs(rs), _visibilityManager(visibilityManager) {
    _savedPosition = boost::none;
}

boost::optional<Record> RecordStore::ReverseCursor::next() {
    // Capped iterators die on invalidation rather than advancing.
    if (_rs._isCapped && _lastMoveWasRestore) {
        return boost::none;
    }

    _savedPosition = boost::none;
    StringStore* workingCopy(RecoveryUnit::get(opCtx)->getHead());
    if (_needFirstSeek) {
        _needFirstSeek = false;
        it = StringStore::const_reverse_iterator(workingCopy->upper_bound(_rs._postfix));
    } else if (it != workingCopy->rend() && !_lastMoveWasRestore) {
        ++it;
    }
    _lastMoveWasRestore = false;

    if (it != workingCopy->rend() && inPrefix(it->first)) {
        _savedPosition = it->first;
        Record nextRecord;
        nextRecord.id = RecordId(extractRecordId(it->first, _rs._keyFormat));
        nextRecord.data = RecordData(it->second.c_str(), it->second.length());

        return nextRecord;
    }
    return boost::none;
}

boost::optional<Record> RecordStore::ReverseCursor::seekExact(const RecordId& id) {
    _needFirstSeek = false;
    _savedPosition = boost::none;
    StringStore* workingCopy(RecoveryUnit::get(opCtx)->getHead());
    std::string key = createKey(_rs._ident, id);
    StringStore::const_iterator canFind = workingCopy->find(key);
    if (canFind == workingCopy->end() || !inPrefix(canFind->first)) {
        it = workingCopy->rend();
        return boost::none;
    }

    it = StringStore::const_reverse_iterator(++canFind);  // reverse iterator returns item 1 before
    _savedPosition = it->first;
    return Record{id, RecordData(it->second.c_str(), it->second.length())};
}

boost::optional<Record> RecordStore::ReverseCursor::seekNear(const RecordId& id) {
    _savedPosition = boost::none;
    _lastMoveWasRestore = false;

    auto numRecords = _rs.numRecords(opCtx);
    if (numRecords == 0)
        return boost::none;

    StringStore* workingCopy{RecoveryUnit::get(opCtx)->getHead()};
    std::string key = createKey(_rs._ident, id);
    it = StringStore::const_reverse_iterator(workingCopy->upper_bound(key));

    // Since there is at least 1 record, if we hit the beginning we need to return the only record.
    if (it == workingCopy->rend() || !inPrefix(it->first)) {
        // This lands on the next key.
        auto fwdIt = workingCopy->upper_bound(key);
        // reverse iterator increments one item before
        it = StringStore::const_reverse_iterator(++fwdIt);
        invariant(it != workingCopy->end());
        invariant(inPrefix(it->first));
    }

    // If we landed lower, then per the API contract, we need to return the previous record.
    RecordId rid = extractRecordId(it->first, _rs._keyFormat);
    if (rid < id) {
        // This lands on the next key.
        auto fwdIt = workingCopy->upper_bound(key);
        if (fwdIt != workingCopy->end() && inPrefix(fwdIt->first)) {
            it = StringStore::const_reverse_iterator(++fwdIt);
        }
        // Otherwise, we hit the beginning of this record store, then there is only one record and
        // we should return that.
    }

    rid = RecordId(extractRecordId(it->first, _rs._keyFormat));
    _needFirstSeek = false;
    _savedPosition = it->first;
    return Record{rid, RecordData(it->second.c_str(), it->second.length())};
}

void RecordStore::ReverseCursor::save() {}
void RecordStore::ReverseCursor::saveUnpositioned() {
    _savedPosition = boost::none;
}

bool RecordStore::ReverseCursor::restore() {
    if (!_savedPosition)
        return true;

    StringStore* workingCopy(RecoveryUnit::get(opCtx)->getHead());
    it = StringStore::const_reverse_iterator(workingCopy->upper_bound(_savedPosition.value()));
    _lastMoveWasRestore = (it == workingCopy->rend() || it->first != _savedPosition.value());

    // Capped iterators die on invalidation rather than advancing.
    return !(_rs._isCapped && _lastMoveWasRestore);
}

void RecordStore::ReverseCursor::detachFromOperationContext() {
    invariant(opCtx != nullptr);
    opCtx = nullptr;
}

void RecordStore::ReverseCursor::reattachToOperationContext(OperationContext* opCtx) {
    invariant(opCtx != nullptr);
    this->opCtx = opCtx;
}

bool RecordStore::ReverseCursor::inPrefix(const std::string& key_string) {
    return (key_string > _rs._prefix) && (key_string < _rs._postfix);
}

RecordStore::SizeAdjuster::SizeAdjuster(OperationContext* opCtx, RecordStore* rs)
    : _opCtx(opCtx),
      _rs(rs),
      _workingCopy(ephemeral_for_test::RecoveryUnit::get(opCtx)->getHead()),
      _origNumRecords(_workingCopy->size()),
      _origDataSize(_workingCopy->dataSize()) {}

RecordStore::SizeAdjuster::~SizeAdjuster() {
    // SERVER-48981 This implementation of fastcount results in inaccurate values. This storage
    // engine emits write conflict exceptions at commit-time leading to the fastcount to be
    // inaccurate until the rollback happens.
    // If proper local isolation is implemented, SERVER-38883 can also be fulfulled for this storage
    // engine where we can invariant for correct fastcount in updateStatsAfterRepair()
    int64_t deltaNumRecords = _workingCopy->size() - _origNumRecords;
    int64_t deltaDataSize = _workingCopy->dataSize() - _origDataSize;
    _rs->_numRecords.fetchAndAdd(deltaNumRecords);
    _rs->_dataSize.fetchAndAdd(deltaDataSize);
    RecoveryUnit::get(_opCtx)->onRollback([rs = _rs, deltaNumRecords, deltaDataSize]() {
        rs->_numRecords.fetchAndSubtract(deltaNumRecords);
        rs->_dataSize.fetchAndSubtract(deltaDataSize);
    });
}

}  // namespace ephemeral_for_test
}  // namespace mongo
