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

#include "mongo/db/pipeline/spilling/spillable_deque.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/pipeline/spilling/spill_table_batch_writer.h"
#include "mongo/db/query/util/spill_util.h"
#include "mongo/db/record_id.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/util/assert_util.h"

#include <utility>

namespace mongo {

bool SpillableDeque::isIdInCache(int id) {
    tassert(5643005,
            str::stream() << "Requested expired document from SpillableDeque. Expected range was "
                          << _nextFreedIndex << "-" << _nextIndex - 1 << " but got " << id,
            _nextFreedIndex <= id);
    return id < _nextIndex;
}

void SpillableDeque::verifyInCache(int id) {
    tassert(5643004,
            str::stream() << "Requested document not in SpillableDeque. Expected range was "
                          << _nextFreedIndex << "-" << _nextIndex - 1 << " but got " << id,
            isIdInCache(id));
}
void SpillableDeque::addDocument(Document input) {
    _memCache.emplace_back(MemoryUsageToken{input.getApproximateSize(), &_memTracker},
                           std::move(input));
    if (!_memTracker.withinMemoryLimit() && _expCtx->getAllowDiskUse()) {
        spillToDisk();
    }
    uassert(5643011,
            str::stream() << "Exceeded max memory. Current memory: "
                          << _memTracker.inUseTrackedMemoryBytes() << " bytes. Max allowed memory: "
                          << _memTracker.maxAllowedMemoryUsageBytes()
                          << " bytes. Set 'allowDiskUse: true' to spill to disk",
            _memTracker.withinMemoryLimit());
    ++_nextIndex;
}
Document SpillableDeque::getDocumentById(int id) {
    verifyInCache(id);
    if (id < _diskWrittenIndex) {
        return readDocumentFromDiskById(id);
    }
    return readDocumentFromMemCacheById(id);
}
void SpillableDeque::freeUpTo(int id) {
    for (int i = _nextFreedIndex; i <= id; ++i) {
        verifyInCache(i);
        // Deleting is expensive in WT. Only delete in memory documents, documents in the record
        // store will only be deleted when we drop the table.
        if (i >= _diskWrittenIndex) {
            tassert(5643010,
                    "Attempted to remove document from empty memCache in SpillableDeque",
                    _memCache.size() > 0);
            _memCache.pop_front();
        }
        ++_nextFreedIndex;
    }
}
void SpillableDeque::clear() {
    if (_diskCache) {
        _expCtx->getMongoProcessInterface()->truncateSpillTable(_expCtx, *_diskCache);
    }
    _memCache.clear();
    _diskWrittenIndex = 0;
    _nextIndex = 0;
    _nextFreedIndex = 0;
}

void SpillableDeque::spillToDisk() {
    if (!_diskCache) {
        uassert(ErrorCodes::QueryExceededMemoryLimitNoDiskUseAllowed,
                "Exceeded memory limit and can't spill to disk. Set allowDiskUse: true to allow "
                "spilling",
                _expCtx->getAllowDiskUse());
        tassert(5872800,
                "SpillableDeque attempted to write to disk in an environment which is not prepared "
                "to do so",
                _expCtx->getOperationContext()->getServiceContext());
        tassert(5872801,
                "SpillableDeque attempted to write to disk in an environment without a storage "
                "engine configured",
                _expCtx->getOperationContext()->getServiceContext()->getStorageEngine());
        _diskCache =
            _expCtx->getMongoProcessInterface()->createSpillTable(_expCtx, KeyFormat::Long);
    }

    // Ensure there is sufficient disk space for spilling
    uassertStatusOK(ensureSufficientDiskSpaceForSpilling(
        storageGlobalParams.dbpath, internalQuerySpillingMinAvailableDiskSpaceBytes.load()));

    // If we've freed things from cache before writing to disk, we need to update
    // '_diskWrittenIndex' to be the actual index of the document we're going to write.
    if (_diskWrittenIndex < _nextFreedIndex) {
        _diskWrittenIndex = _nextFreedIndex;
    }

    SpillTableBatchWriter writer{_expCtx, *_diskCache};
    for (auto& memoryTokenWithDoc : _memCache) {
        RecordId recordId{++_diskWrittenIndex};
        auto bsonDoc = memoryTokenWithDoc.value().toBsonWithMetaData();
        writer.write(recordId, std::move(bsonDoc));
    }
    _memCache.clear();
    // Write final batch.
    writer.flush();

    _stats.updateSpillingStats(
        1,
        writer.writtenBytes(),
        writer.writtenRecords(),
        static_cast<uint64_t>(_diskCache->storageSize(
            *shard_role_details::getRecoveryUnit(_expCtx->getOperationContext()))));
    CurOp::get(_expCtx->getOperationContext())
        ->updateSpillStorageStats(_diskCache->computeOperationStatisticsSinceLastCall());
}

void SpillableDeque::updateStorageSizeStat() {
    _stats.updateSpilledDataStorageSize(_diskCache->storageSize(
        *shard_role_details::getRecoveryUnit(_expCtx->getOperationContext())));
}

Document SpillableDeque::readDocumentFromDiskById(int desired) {
    tassert(5643006,
            str::stream() << "Attempted to read id " << desired
                          << "from disk in SpillableDeque before writing",
            _diskCache && desired < _diskWrittenIndex);
    return _expCtx->getMongoProcessInterface()->readRecordFromSpillTable(
        _expCtx, *_diskCache, RecordId(desired + 1));
}
Document SpillableDeque::readDocumentFromMemCacheById(int desired) {
    // If we have only freed documents from disk, the index into '_memCache' is off by the number of
    // documents we've ever written to disk. If we have freed documents from the cache, the index
    // into '_memCache' is off by how many documents we've ever freed. In this case what we've
    // written to disk doesn't matter, since those no longer affect in memory indexes.
    size_t lookupIndex = _diskWrittenIndex > _nextFreedIndex ? desired - _diskWrittenIndex
                                                             : desired - _nextFreedIndex;

    tassert(5643007,
            str::stream() << "Attempted to lookup " << lookupIndex << " but cache is only holding "
                          << _memCache.size(),
            lookupIndex < _memCache.size());
    return _memCache[lookupIndex].value();
}

}  // namespace mongo
