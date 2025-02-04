/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/pipeline/spilling/spillable_map.h"
#include "mongo/db/pipeline/spilling/record_store_batch_writer.h"
#include "mongo/db/query/util/spill_util.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

void SpillableDocumentMap::add(Document document) {
    size_t size = document.getApproximateSize();
    Value id = document.getField("_id");
    auto [_, inserted] = _memMap.emplace(
        std::move(id),
        MemoryUsageTokenWith<Document>{MemoryUsageToken{size, &_memTracker}, std::move(document)});
    tassert(2398003, "duplicate keys are not supported in SpillableDocumentMap", inserted);
    if (!_memTracker.withinMemoryLimit()) {
        spillToDisk();
    }
}

bool SpillableDocumentMap::contains(const Value& id) const {
    if (_memMap.contains(id)) {
        return true;
    }
    if (_diskMap == nullptr || _diskMapSize == 0) {
        return false;
    }

    return _expCtx->getMongoProcessInterface()->checkRecordInRecordStore(
        _expCtx, _diskMap->rs(), computeKey(id));
}

void SpillableDocumentMap::clear() {
    _memMap.erase(_memMap.begin(), _memMap.end());
    if (_diskMap) {
        _expCtx->getMongoProcessInterface()->truncateRecordStore(_expCtx, _diskMap->rs());
        _diskMapSize = 0;
    }
}

void SpillableDocumentMap::dispose() {
    _memMap.clear();
    if (_diskMap) {
        updateStorageSizeStat();
        _diskMap.reset();
        _diskMapSize = 0;
    }
}

void SpillableDocumentMap::spillToDisk() {
    uassertStatusOK(ensureSufficientDiskSpaceForSpilling(
        storageGlobalParams.dbpath, internalQuerySpillingMinAvailableDiskSpaceBytes.load()));

    if (_diskMap == nullptr) {
        initDiskMap();
    }

    RecordStoreBatchWriter writer(_expCtx, _diskMap->rs());
    while (!_memMap.empty()) {
        auto it = _memMap.begin();
        writer.write(computeKey(it->first), it->second.value().toBson());
        _memMap.erase(it);
    }
    writer.flush();
    _diskMapSize += writer.writtenRecords();

    _stats.updateSpillingStats(
        1,
        writer.writtenBytes(),
        writer.writtenRecords(),
        static_cast<uint64_t>(_diskMap->rs()->storageSize(
            *shard_role_details::getRecoveryUnit(_expCtx->getOperationContext()))));
}

void SpillableDocumentMap::initDiskMap() {
    uassert(ErrorCodes::QueryExceededMemoryLimitNoDiskUseAllowed,
            "Exceeded memory limit and can't spill to disk. Set allowDiskUse: true to allow "
            "spilling",
            _expCtx->getAllowDiskUse());

    _diskMap =
        _expCtx->getMongoProcessInterface()->createTemporaryRecordStore(_expCtx, KeyFormat::String);
    _diskMapSize = 0;
}

RecordId SpillableDocumentMap::computeKey(const Value& id) const {
    _builder.resetToEmpty();
    BSONObj obj = id.wrap("_");
    _builder.appendBSONElement(obj.firstElement());
    return RecordId{_builder.finishAndGetBuffer()};
}

void SpillableDocumentMap::updateStorageSizeStat() {
    _stats.updateSpilledDataStorageSize(_diskMap->rs()->storageSize(
        *shard_role_details::getRecoveryUnit(_expCtx->getOperationContext())));
}

template <bool IsConst>
SpillableDocumentMap::IteratorImpl<IsConst>::IteratorImpl(MapPointer map)
    : _map(map), _memIt(_map->_memMap.begin()) {
    if (_map->_diskMap != nullptr) {
        _diskIt = _map->_diskMap->rs()->getCursor(_map->_expCtx->getOperationContext());
        _diskItExhausted = false;
        saveDiskIt();

        if (memoryExhausted()) {
            readNextBatchFromDisk();
        }
    }
}

template <bool IsConst>
SpillableDocumentMap::IteratorImpl<IsConst>::IteratorImpl(MapPointer map, const EndTag&)
    : _map(map), _memIt(_map->_memMap.end()) {}

template <bool IsConst>
bool SpillableDocumentMap::IteratorImpl<IsConst>::operator==(const IteratorImpl& rhs) const {
    if (this->memoryExhausted() != rhs.memoryExhausted()) {
        return false;
    }
    if (!this->memoryExhausted()) {
        return this->_memIt == rhs._memIt;
    }
    if (this->diskExhausted() != rhs.diskExhausted()) {
        return false;
    }
    if (!this->diskExhausted()) {
        return ValueComparator::kInstance.compare(
            this->_diskDocuments.front().value().getField("_id"),
            rhs._diskDocuments.front().value().getField("_id"));
    }
    return true;
}

template <bool IsConst>
auto SpillableDocumentMap::IteratorImpl<IsConst>::operator++() -> IteratorImpl& {
    if (!memoryExhausted()) {
        _memIt++;
        if (memoryExhausted()) {
            readNextBatchFromDisk();
        }
    } else if (!diskExhausted()) {
        _diskDocuments.pop_front();
        if (_diskDocuments.empty()) {
            if (_diskItExhausted) {
                _diskIt.reset();
            } else {
                readNextBatchFromDisk();
            }
        }
    }
    return *this;
}

template <bool IsConst>
void SpillableDocumentMap::IteratorImpl<IsConst>::spill() {
    if (!memoryExhausted()) {
        return;
    }

    // When reading from disk, we need to have at least one document in the buffer to be able to
    // deference the iterator.
    if (_diskDocuments.size() <= 1) {
        return;
    }

    RecordId frontRecordId = _map->computeKey(_diskDocuments.front().value().getField("_id"));
    _diskDocuments.clear();

    restoreDiskIt();
    ON_BLOCK_EXIT([&]() { saveDiskIt(); });

    boost::optional<Record> bson = _diskIt->seekExact(frontRecordId);
    tassert(2398005, "Previously present RecordId not found", bson.has_value());
    _diskItExhausted = false;

    Document doc{bson->data.releaseToBson()};
    _diskDocuments.emplace_back(MemoryUsageToken{doc.getApproximateSize(), &_map->_memTracker},
                                doc.getOwned());
}

template <bool IsConst>
bool SpillableDocumentMap::IteratorImpl<IsConst>::memoryExhausted() const {
    return _memIt == _map->_memMap.end();
}

template <bool IsConst>
bool SpillableDocumentMap::IteratorImpl<IsConst>::diskExhausted() const {
    return _diskItExhausted && _diskDocuments.empty();
}

template <bool IsConst>
void SpillableDocumentMap::IteratorImpl<IsConst>::readNextBatchFromDisk() {
    if (_diskItExhausted || !_diskDocuments.empty()) {
        return;
    }

    restoreDiskIt();
    ON_BLOCK_EXIT([&]() { saveDiskIt(); });

    // Always read at least one document
    do {
        auto bson = _diskIt->next();
        if (!bson) {
            _diskItExhausted = true;
            return;
        }
        Document doc{bson->data.releaseToBson()};
        _diskDocuments.emplace_back(MemoryUsageToken{doc.getApproximateSize(), &_map->_memTracker},
                                    doc.getOwned());
    } while (!_diskItExhausted && _map->_memTracker.withinMemoryLimit());
}

template <bool IsConst>
auto SpillableDocumentMap::IteratorImpl<IsConst>::getCurrentDocument() -> reference_type {
    if (!memoryExhausted()) {
        return _memIt->second.value();
    }
    if (!_diskDocuments.empty()) {
        return _diskDocuments.front().value();
    }
    tasserted(2398002, "dereferencing invalid SpillableDocumentMap::IteratorImpl");
}

template <bool IsConst>
void SpillableDocumentMap::IteratorImpl<IsConst>::restoreDiskIt() {
    _diskIt->reattachToOperationContext(_map->_expCtx->getOperationContext());
    bool restoreResult = _diskIt->restore();
    tassert(2398004, "Unable to restore disk cursor", restoreResult);
}

template <bool IsConst>
void SpillableDocumentMap::IteratorImpl<IsConst>::saveDiskIt() {
    _diskIt->save();
    _diskIt->detachFromOperationContext();
}

template class SpillableDocumentMap::IteratorImpl<true>;
template class SpillableDocumentMap::IteratorImpl<false>;

}  // namespace mongo
