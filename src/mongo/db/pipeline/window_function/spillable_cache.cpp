/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/pipeline/window_function/spillable_cache.h"

#include "mongo/db/record_id.h"
#include "mongo/db/storage/record_data.h"

namespace mongo {

bool SpillableCache::isIdInCache(int id) {
    tassert(5643005,
            str::stream() << "Requested expired document from SpillableCache. Expected range was "
                          << _nextFreedIndex << "-" << _nextIndex - 1 << " but got " << id,
            _nextFreedIndex <= id);
    return id < _nextIndex;
}

void SpillableCache::verifyInCache(int id) {
    tassert(5643004,
            str::stream() << "Requested document not in SpillableCache. Expected range was "
                          << _nextFreedIndex << "-" << _nextIndex - 1 << " but got " << id,
            isIdInCache(id));
}
void SpillableCache::addDocument(Document input) {
    _memTracker.update(input.getApproximateSize());
    _memCache.emplace_back(std::move(input));
    if (_memTracker.currentMemoryBytes() >=
            static_cast<long long>(_memTracker.base->_maxAllowedMemoryUsageBytes) &&
        _expCtx->allowDiskUse) {
        spillToDisk();
    }
    uassert(5643011,
            "Exceeded max memory. Set 'allowDiskUse: true' to spill to disk",
            _memTracker.currentMemoryBytes() <
                static_cast<long long>(_memTracker.base->_maxAllowedMemoryUsageBytes));
    ++_nextIndex;
}
Document SpillableCache::getDocumentById(int id) {
    verifyInCache(id);
    if (id < _diskWrittenIndex) {
        return readDocumentFromDiskById(id);
    }
    return readDocumentFromMemCacheById(id);
}
void SpillableCache::freeUpTo(int id) {
    for (int i = _nextFreedIndex; i <= id; ++i) {
        verifyInCache(i);
        // Deleting is expensive in WT. Only delete in memory documents, documents in the record
        // store will only be deleted when we drop the table.
        if (i >= _diskWrittenIndex) {
            tassert(5643010,
                    "Attempted to remove document from empty memCache in SpillableCache",
                    _memCache.size() > 0);
            _memTracker.update(-1 * _memCache.front().getApproximateSize());
            _memCache.pop_front();
        }
        ++_nextFreedIndex;
    }
}
void SpillableCache::clear() {
    if (_diskCache) {
        _expCtx->mongoProcessInterface->truncateRecordStore(_expCtx, _diskCache->rs());
    }
    _memCache.clear();
    _diskWrittenIndex = 0;
    _nextIndex = 0;
    _nextFreedIndex = 0;
    _memTracker.set(0);
}

void SpillableCache::writeBatchToDisk(std::vector<Record>& records) {
    // By passing a vector of null timestamps, these inserts are not timestamped individually, but
    // rather with the timestamp of the owning operation. We don't care about the timestamps.
    std::vector<Timestamp> timestamps(records.size());

    _expCtx->mongoProcessInterface->writeRecordsToRecordStore(
        _expCtx, _diskCache->rs(), &records, timestamps);
}
void SpillableCache::spillToDisk() {
    if (!_diskCache) {
        tassert(5643008,
                "Exceeded memory limit and can't spill to disk. Set allowDiskUse: true to allow "
                "spilling",
                _expCtx->allowDiskUse);
        tassert(5872800,
                "SpillableCache attempted to write to disk in an environment which is not prepared "
                "to do so",
                _expCtx->opCtx->getServiceContext());
        tassert(5872801,
                "SpillableCache attempted to write to disk in an environment without a storage "
                "engine configured",
                _expCtx->opCtx->getServiceContext()->getStorageEngine());
        _usedDisk = true;
        _diskCache = _expCtx->mongoProcessInterface->createTemporaryRecordStore(_expCtx);
    }
    // If we've freed things from cache before writing to disk, we need to update
    // '_diskWrittenIndex' to be the actual index of the document we're going to write.
    if (_diskWrittenIndex < _nextFreedIndex) {
        _diskWrittenIndex = _nextFreedIndex;
    }

    std::vector<Record> records;
    std::vector<BSONObj> ownedObjs;
    // Batch our writes to reduce pressure on the storage engine's cache.
    size_t batchSize = 0;
    for (auto& doc : _memCache) {
        auto bsonDoc = doc.toBson();
        size_t objSize = bsonDoc.objsize();
        if (records.size() == 1000 || batchSize + objSize > kMaxWriteSize) {
            writeBatchToDisk(records);
            records.clear();
            ownedObjs.clear();
            batchSize = 0;
        }
        ownedObjs.push_back(bsonDoc.getOwned());
        records.emplace_back(Record{RecordId(_diskWrittenIndex + 1),
                                    RecordData(ownedObjs.back().objdata(), objSize)});
        batchSize += objSize;
        ++_diskWrittenIndex;
    }
    _memCache.clear();
    _memTracker.set(0);
    if (records.size() == 0) {
        return;
    }
    // Write final batch.
    writeBatchToDisk(records);
}

Document SpillableCache::readDocumentFromDiskById(int desired) {
    tassert(5643006,
            str::stream() << "Attempted to read id " << desired
                          << "from disk in SpillableCache before writing",
            _diskCache && desired < _diskWrittenIndex);
    return _expCtx->mongoProcessInterface->readRecordFromRecordStore(
        _expCtx, _diskCache->rs(), RecordId(desired + 1));
}
Document SpillableCache::readDocumentFromMemCacheById(int desired) {
    // If we have only freed documents from disk, the index into '_memCache' is off by the number of
    // documents we've ever written to disk. If we have freed documents from the cache, the index
    // into '_memCache' is off by how many documents we've ever freed. In this case what we've
    // written to disk doesn't matter, since those no longer affect in memory indexes.
    auto lookupIndex = _diskWrittenIndex > _nextFreedIndex ? desired - _diskWrittenIndex
                                                           : desired - _nextFreedIndex;

    tassert(5643007,
            str::stream() << "Attempted to lookup " << lookupIndex << " but cache is only holding "
                          << _memCache.size(),
            lookupIndex < (int)_memCache.size());
    return _memCache[lookupIndex];
}

}  // namespace mongo
