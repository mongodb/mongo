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

#include <cstring>
#include <memory>
#include <utility>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/biggie/biggie_record_store.h"
#include "mongo/db/storage/biggie/biggie_recovery_unit.h"
#include "mongo/db/storage/biggie/biggie_visibility_manager.h"
#include "mongo/db/storage/biggie/store.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/db/storage/oplog_hack.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/hex.h"
#include "mongo/util/log.h"

namespace mongo {
namespace biggie {
namespace {
Ordering allAscending = Ordering::make(BSONObj());
auto const version = KeyString::Version::V1;
BSONObj const sample = BSON(""
                            << "s"
                            << ""
                            << (int64_t)0);

std::string createKey(StringData ident, int64_t recordId) {
    KeyString ks(version, BSON("" << ident << "" << recordId), allAscending);
    return std::string(ks.getBuffer(), ks.getSize());
}

RecordId extractRecordId(const std::string& keyStr) {
    KeyString ks(version, sample, allAscending);
    ks.resetFromBuffer(keyStr.c_str(), keyStr.size());
    BSONObj obj = KeyString::toBson(keyStr.c_str(), keyStr.size(), allAscending, ks.getTypeBits());
    auto it = BSONObjIterator(obj);
    ++it;
    return RecordId((*it).Long());
}
}  // namespace

RecordStore::RecordStore(StringData ns,
                         StringData ident,
                         bool isCapped,
                         int64_t cappedMaxSize,
                         int64_t cappedMaxDocs,
                         CappedCallback* cappedCallback,
                         VisibilityManager* visibilityManager)
    : mongo::RecordStore(ns),
      _isCapped(isCapped),
      _cappedMaxSize(cappedMaxSize),
      _cappedMaxDocs(cappedMaxDocs),
      _identStr(ident.rawData(), ident.size()),
      _ident(_identStr.data(), _identStr.size()),
      _prefix(createKey(_ident, std::numeric_limits<int64_t>::min())),
      _postfix(createKey(_ident, std::numeric_limits<int64_t>::max())),
      _cappedCallback(cappedCallback),
      _isOplog(NamespaceString::oplog(ns)),
      _visibilityManager(visibilityManager) {
    if (_isCapped) {
        invariant(_cappedMaxSize > 0);
        invariant(_cappedMaxDocs == -1 || _cappedMaxDocs > 0);
    } else {
        invariant(_cappedMaxSize == -1);
        invariant(_cappedMaxDocs == -1);
    }
}

const char* RecordStore::name() const {
    return "biggie";
}

const std::string& RecordStore::getIdent() const {
    return _identStr;
}

long long RecordStore::dataSize(OperationContext* opCtx) const {
    return _dataSize.load();
}

long long RecordStore::numRecords(OperationContext* opCtx) const {
    return static_cast<long long>(_numRecords.load());
}

bool RecordStore::isCapped() const {
    return _isCapped;
}

void RecordStore::setCappedCallback(CappedCallback* cb) {
    stdx::lock_guard<stdx::mutex> cappedCallbackLock(_cappedCallbackMutex);
    _cappedCallback = cb;
}

int64_t RecordStore::storageSize(OperationContext* opCtx,
                                 BSONObjBuilder* extraInfo,
                                 int infoLevel) const {
    return dataSize(opCtx);
}

bool RecordStore::findRecord(OperationContext* opCtx, const RecordId& loc, RecordData* rd) const {
    StringStore* workingCopy(RecoveryUnit::get(opCtx)->getHead());
    auto it = workingCopy->find(createKey(_ident, loc.repr()));
    if (it == workingCopy->end()) {
        return false;
    }
    *rd = RecordData(it->second.c_str(), it->second.length());
    return true;
}

void RecordStore::deleteRecord(OperationContext* opCtx, const RecordId& dl) {
    auto ru = RecoveryUnit::get(opCtx);
    StringStore* workingCopy(ru->getHead());
    SizeAdjuster adjuster(opCtx, this);
    invariant(workingCopy->erase(createKey(_ident, dl.repr())));
    ru->makeDirty();
}

Status RecordStore::insertRecords(OperationContext* opCtx,
                                  std::vector<Record>* inOutRecords,
                                  const std::vector<Timestamp>& timestamps) {
    int64_t totalSize = 0;
    for (auto& record : *inOutRecords)
        totalSize += record.data.size();

    // Caller will retry one element at a time.
    if (_isCapped && totalSize > _cappedMaxSize)
        return Status(ErrorCodes::BadValue, "object to insert exceeds cappedMaxSize");

    auto ru = RecoveryUnit::get(opCtx);
    StringStore* workingCopy(ru->getHead());
    {
        SizeAdjuster adjuster(opCtx, this);
        for (auto& record : *inOutRecords) {
            int64_t thisRecordId = 0;
            if (_isOplog) {
                StatusWith<RecordId> status =
                    oploghack::extractKey(record.data.data(), record.data.size());
                if (!status.isOK())
                    return status.getStatus();
                thisRecordId = status.getValue().repr();
                _visibilityManager->addUncommittedRecord(opCtx, this, RecordId(thisRecordId));
            } else {
                thisRecordId = _nextRecordId();
            }
            workingCopy->insert(
                StringStore::value_type{createKey(_ident, thisRecordId),
                                        std::string(record.data.data(), record.data.size())});
            record.id = RecordId(thisRecordId);
        }
    }
    ru->makeDirty();
    _cappedDeleteAsNeeded(opCtx, workingCopy);
    return Status::OK();
}

Status RecordStore::insertRecordsWithDocWriter(OperationContext* opCtx,
                                               const DocWriter* const* docs,
                                               const Timestamp*,
                                               size_t nDocs,
                                               RecordId* idsOut) {
    int64_t totalSize = 0;
    for (size_t i = 0; i < nDocs; i++)
        totalSize += docs[i]->documentSize();

    // Caller will retry one element at a time.
    if (_isCapped && totalSize > _cappedMaxSize)
        return Status(ErrorCodes::BadValue, "object to insert exceeds cappedMaxSize");

    auto ru = RecoveryUnit::get(opCtx);
    StringStore* workingCopy(ru->getHead());
    {
        SizeAdjuster adjuster(opCtx, this);
        for (size_t i = 0; i < nDocs; i++) {
            const size_t len = docs[i]->documentSize();

            std::string buf(len, '\0');
            docs[i]->writeDocument(&buf[0]);

            int64_t thisRecordId = 0;
            if (_isOplog) {
                StatusWith<RecordId> status = oploghack::extractKey(buf.data(), len);
                if (!status.isOK())
                    return status.getStatus();
                thisRecordId = status.getValue().repr();
                _visibilityManager->addUncommittedRecord(opCtx, this, RecordId(thisRecordId));
            } else {
                thisRecordId = _nextRecordId();
            }
            std::string key = createKey(_ident, thisRecordId);

            StringStore::value_type vt{key, buf};
            workingCopy->insert(std::move(vt));
            if (idsOut)
                idsOut[i] = RecordId(thisRecordId);
            ru->makeDirty();
        }
    }

    _cappedDeleteAsNeeded(opCtx, workingCopy);
    return Status::OK();
}

Status RecordStore::updateRecord(OperationContext* opCtx,
                                 const RecordId& oldLocation,
                                 const char* data,
                                 int len) {
    StringStore* workingCopy(RecoveryUnit::get(opCtx)->getHead());
    SizeAdjuster(opCtx, this);
    {
        std::string key = createKey(_ident, oldLocation.repr());
        StringStore::const_iterator it = workingCopy->find(key);
        invariant(it != workingCopy->end());
        workingCopy->update(StringStore::value_type{key, std::string(data, len)});
    }
    _cappedDeleteAsNeeded(opCtx, workingCopy);
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
    StatusWith<int64_t> s = truncateWithoutUpdatingCount(opCtx);
    if (!s.isOK())
        return s.getStatus();

    return Status::OK();
}

StatusWith<int64_t> RecordStore::truncateWithoutUpdatingCount(OperationContext* opCtx) {
    auto ru = RecoveryUnit::get(opCtx);
    StringStore* workingCopy(ru->getHead());
    StringStore::const_iterator end = workingCopy->upper_bound(_postfix);
    std::vector<std::string> toDelete;

    for (auto it = workingCopy->lower_bound(_prefix); it != end; ++it) {
        toDelete.push_back(it->first);
    }

    if (toDelete.empty())
        return 0;

    for (const auto& key : toDelete)
        workingCopy->erase(key);

    ru->makeDirty();

    return static_cast<int64_t>(toDelete.size());
}

void RecordStore::cappedTruncateAfter(OperationContext* opCtx, RecordId end, bool inclusive) {
    auto ru = RecoveryUnit::get(opCtx);
    StringStore* workingCopy(ru->getHead());
    WriteUnitOfWork wuow(opCtx);
    const auto recordKey = createKey(_ident, end.repr());
    auto recordIt =
        inclusive ? workingCopy->lower_bound(recordKey) : workingCopy->upper_bound(recordKey);
    auto endIt = workingCopy->upper_bound(_postfix);

    while (recordIt != endIt) {
        stdx::lock_guard<stdx::mutex> cappedCallbackLock(_cappedCallbackMutex);
        if (_cappedCallback) {
            // Documents are guaranteed to have a RecordId at the end of the KeyString, unlike
            // unique indexes.
            RecordId rid = extractRecordId(recordIt->first);
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

void RecordStore::appendCustomStats(OperationContext* opCtx,
                                    BSONObjBuilder* result,
                                    double scale) const {
    result->appendBool("capped", _isCapped);
    if (_isCapped) {
        result->appendIntOrLL("max", _cappedMaxDocs);
        result->appendIntOrLL("maxSize", _cappedMaxSize / scale);
    }
}

Status RecordStore::touch(OperationContext* opCtx, BSONObjBuilder* output) const {
    return Status::OK();  // All data is already in 'cache'.
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

boost::optional<RecordId> RecordStore::oplogStartHack(OperationContext* opCtx,
                                                      const RecordId& startingPosition) const {
    if (!_isOplog)
        return boost::none;

    if (numRecords(opCtx) == 0)
        return RecordId();

    StringStore* workingCopy{RecoveryUnit::get(opCtx)->getHead()};

    std::string key = createKey(_ident, startingPosition.repr());
    StringStore::const_reverse_iterator it(workingCopy->upper_bound(key));

    if (it == workingCopy->rend())
        return RecordId();

    RecordId rid = RecordId(extractRecordId(it->first));
    if (rid > startingPosition)
        return RecordId();

    return rid;
}

bool RecordStore::_cappedAndNeedDelete(OperationContext* opCtx, StringStore* workingCopy) {
    if (!_isCapped)
        return false;

    if (dataSize(opCtx) > _cappedMaxSize)
        return true;

    if ((_cappedMaxDocs != -1) && numRecords(opCtx) > _cappedMaxDocs)
        return true;
    return false;
}

void RecordStore::_cappedDeleteAsNeeded(OperationContext* opCtx, StringStore* workingCopy) {
    if (!_isCapped)
        return;

    // Create the lowest key for this identifier and use lower_bound() to get to the first one.
    auto recordIt = workingCopy->lower_bound(_prefix);

    // Ensure only one thread at a time can do deletes, otherwise they'll conflict.
    stdx::lock_guard<stdx::mutex> cappedDeleterLock(_cappedDeleterMutex);

    while (_cappedAndNeedDelete(opCtx, workingCopy)) {

        stdx::lock_guard<stdx::mutex> cappedCallbackLock(_cappedCallbackMutex);
        RecordId rid = RecordId(extractRecordId(recordIt->first));

        if (_isOplog && _visibilityManager->isFirstHidden(rid)) {
            // We have a record that hasn't been committed yet, so we shouldn't truncate anymore
            // until it gets committed.
            return;
        }

        if (_cappedCallback) {
            RecordData rd = RecordData(recordIt->second.c_str(), recordIt->second.length());
            uassertStatusOK(_cappedCallback->aboutToDeleteCapped(opCtx, rid, rd));
        }

        SizeAdjuster adjuster(opCtx, this);
        invariant(numRecords(opCtx) > 0, str::stream() << numRecords(opCtx));

        // Don't need to increment the iterator because the iterator gets revalidated and placed on
        // the next item after the erase.
        workingCopy->erase(recordIt->first);
        auto ru = RecoveryUnit::get(opCtx);
        ru->makeDirty();
    }
}

RecordStore::Cursor::Cursor(OperationContext* opCtx,
                            const RecordStore& rs,
                            VisibilityManager* visibilityManager)
    : opCtx(opCtx), _visibilityManager(visibilityManager) {
    _ident = rs._ident;
    _prefix = rs._prefix;
    _postfix = rs._postfix;
    _isCapped = rs._isCapped;
    _isOplog = rs._isOplog;
}

boost::optional<Record> RecordStore::Cursor::next() {
    _savedPosition = boost::none;
    StringStore* workingCopy(RecoveryUnit::get(opCtx)->getHead());
    if (_needFirstSeek) {
        _needFirstSeek = false;
        it = workingCopy->lower_bound(_prefix);
    } else if (it != workingCopy->end() && !_lastMoveWasRestore) {
        ++it;
    }
    _lastMoveWasRestore = false;
    if (it != workingCopy->end() && inPrefix(it->first)) {
        _savedPosition = it->first;
        Record nextRecord;
        nextRecord.id = RecordId(extractRecordId(it->first));
        nextRecord.data = RecordData(it->second.c_str(), it->second.length());

        if (_isOplog && nextRecord.id > _visibilityManager->getAllCommittedRecord())
            return boost::none;
        return nextRecord;
    }
    return boost::none;
}

boost::optional<Record> RecordStore::Cursor::seekExact(const RecordId& id) {
    _savedPosition = boost::none;
    _lastMoveWasRestore = false;
    StringStore* workingCopy(RecoveryUnit::get(opCtx)->getHead());
    std::string key = createKey(_ident, id.repr());
    it = workingCopy->find(key);

    if (it == workingCopy->end() || !inPrefix(it->first))
        return boost::none;

    if (_isOplog && id > _visibilityManager->getAllCommittedRecord())
        return boost::none;

    _needFirstSeek = false;
    _savedPosition = it->first;
    return Record{id, RecordData(it->second.c_str(), it->second.length())};
}

// Positions are saved as we go.
void RecordStore::Cursor::save() {}
void RecordStore::Cursor::saveUnpositioned() {}

bool RecordStore::Cursor::restore() {
    StringStore* workingCopy(RecoveryUnit::get(opCtx)->getHead());
    it = (_savedPosition) ? workingCopy->lower_bound(_savedPosition.value()) : workingCopy->end();
    _lastMoveWasRestore = it == workingCopy->end() || it->first != _savedPosition.value();

    // Capped iterators die on invalidation rather than advancing.
    return !(_isCapped && _lastMoveWasRestore);
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
    return (key_string > _prefix) && (key_string < _postfix);
}

RecordStore::ReverseCursor::ReverseCursor(OperationContext* opCtx,
                                          const RecordStore& rs,
                                          VisibilityManager* visibilityManager)
    : opCtx(opCtx), _visibilityManager(visibilityManager) {
    _savedPosition = boost::none;
    _ident = rs._ident;
    _prefix = rs._prefix;
    _postfix = rs._postfix;
    _isCapped = rs._isCapped;
    _isOplog = rs._isOplog;
}

boost::optional<Record> RecordStore::ReverseCursor::next() {
    _savedPosition = boost::none;
    StringStore* workingCopy(RecoveryUnit::get(opCtx)->getHead());
    if (_needFirstSeek) {
        _needFirstSeek = false;
        it = StringStore::const_reverse_iterator(workingCopy->upper_bound(_postfix));
    } else if (it != workingCopy->rend() && !_lastMoveWasRestore) {
        ++it;
    }
    _lastMoveWasRestore = false;

    if (it != workingCopy->rend() && inPrefix(it->first)) {
        _savedPosition = it->first;
        Record nextRecord;
        nextRecord.id = RecordId(extractRecordId(it->first));
        nextRecord.data = RecordData(it->second.c_str(), it->second.length());

        if (_isOplog && nextRecord.id > _visibilityManager->getAllCommittedRecord())
            return boost::none;
        return nextRecord;
    }
    return boost::none;
}

boost::optional<Record> RecordStore::ReverseCursor::seekExact(const RecordId& id) {
    _needFirstSeek = false;
    _savedPosition = boost::none;
    StringStore* workingCopy(RecoveryUnit::get(opCtx)->getHead());
    std::string key = createKey(_ident, id.repr());
    StringStore::const_iterator canFind = workingCopy->find(key);
    if (canFind == workingCopy->end() || !inPrefix(canFind->first)) {
        it = workingCopy->rend();
        return boost::none;
    }

    if (_isOplog && id > _visibilityManager->getAllCommittedRecord())
        return boost::none;

    it = StringStore::const_reverse_iterator(++canFind);  // reverse iterator returns item 1 before
    _savedPosition = it->first;
    return Record{id, RecordData(it->second.c_str(), it->second.length())};
}

void RecordStore::ReverseCursor::save() {}
void RecordStore::ReverseCursor::saveUnpositioned() {}

bool RecordStore::ReverseCursor::restore() {
    StringStore* workingCopy(RecoveryUnit::get(opCtx)->getHead());
    it = _savedPosition
        ? StringStore::const_reverse_iterator(workingCopy->upper_bound(_savedPosition.value()))
        : workingCopy->rend();
    _lastMoveWasRestore = (it == workingCopy->rend() || it->first != _savedPosition.value());

    // Capped iterators die on invalidation rather than advancing.
    return !(_isCapped && _lastMoveWasRestore);
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
    return (key_string > _prefix) && (key_string < _postfix);
}

RecordStore::SizeAdjuster::SizeAdjuster(OperationContext* opCtx, RecordStore* rs)
    : _opCtx(opCtx),
      _rs(rs),
      _workingCopy(biggie::RecoveryUnit::get(opCtx)->getHead()),
      _origNumRecords(_workingCopy->size()),
      _origDataSize(_workingCopy->dataSize()) {}

RecordStore::SizeAdjuster::~SizeAdjuster() {
    int64_t deltaNumRecords = _workingCopy->size() - _origNumRecords;
    int64_t deltaDataSize = _workingCopy->dataSize() - _origDataSize;
    _rs->_numRecords.fetchAndAdd(deltaNumRecords);
    _rs->_dataSize.fetchAndAdd(deltaDataSize);
    RecoveryUnit::get(_opCtx)->onRollback([ rs = _rs, deltaNumRecords, deltaDataSize ]() {
        invariant(rs->_numRecords.load() >= deltaNumRecords);
        rs->_numRecords.fetchAndSubtract(deltaNumRecords);
        rs->_dataSize.fetchAndSubtract(deltaDataSize);
    });
}

}  // namespace biggie
}  // namespace mongo
