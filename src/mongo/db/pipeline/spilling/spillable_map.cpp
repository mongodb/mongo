// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/spilling/spillable_map.h"

#include "mongo/db/curop.h"
#include "mongo/db/pipeline/spilling/spill_table_batch_writer.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

void SpillableDocumentMapImpl::_add(Value id, Document document, size_t size) {
    auto [_, inserted] = _memMap.emplace(
        std::move(id),
        MemoryUsageTokenWith<Document>{MemoryUsageToken{size, &_memTracker}, std::move(document)});
    tassert(2398003, "duplicate keys are not supported in SpillableDocumentMapImpl", inserted);
    if (!_memTracker.withinMemoryLimit()) {
        spillToDisk();
    }
}

bool SpillableDocumentMapImpl::contains(const Value& id) const {
    if (_memMap.contains(id)) {
        return true;
    }
    if (_diskMap == nullptr || _diskMapSize == 0) {
        return false;
    }

    return _expCtx->getMongoProcessInterface()->checkRecordInSpillTable(
        _expCtx, *_diskMap, computeKey(id));
}

void SpillableDocumentMapImpl::clear() {
    _memMap.erase(_memMap.begin(), _memMap.end());
    if (_diskMap) {
        _expCtx->getMongoProcessInterface()->truncateSpillTable(_expCtx, *_diskMap);
        _diskMapSize = 0;
    }
}

void SpillableDocumentMapImpl::dispose() {
    _memMap.clear();
    if (_diskMap) {
        updateStorageSizeStat();
        _diskMap.reset();
        _diskMapSize = 0;
    }
}

void SpillableDocumentMapImpl::spillToDisk() {
    if (!hasInMemoryData()) {
        return;
    }

    if (_diskMap == nullptr) {
        initDiskMap();
    }

    SpillTableBatchWriter writer(_expCtx, *_diskMap);
    while (!_memMap.empty()) {
        auto it = _memMap.begin();
        writer.write(computeKey(it->first), it->second.value().toBson());
        _memMap.erase(it);
    }
    writer.flush();
    _diskMapSize += writer.writtenRecords();

    _stats.updateSpillingStats(1,
                               writer.writtenBytes(),
                               writer.writtenRecords(),
                               static_cast<uint64_t>(_diskMap->storageSize()));
    CurOp::get(_expCtx->getOperationContext())
        ->updateSpillStorageStats(_diskMap->computeOperationStatisticsSinceLastCall());
}

void SpillableDocumentMapImpl::initDiskMap() {
    _memTracker.assertCanSpill(_expCtx->getAllowDiskUse(), "SPILLABLE_DOCUMENT_MAP");

    _diskMap = _expCtx->getMongoProcessInterface()->createSpillTable(_expCtx, KeyFormat::String);
    _diskMapSize = 0;
}

RecordId SpillableDocumentMapImpl::computeKey(const Value& id) const {
    _builder.resetToEmpty();
    BSONObj obj = id.wrap("_");
    _builder.appendBSONElement(obj.firstElement());
    return RecordId{_builder.finishAndGetBuffer()};
}

void SpillableDocumentMapImpl::updateStorageSizeStat() {
    _stats.updateSpilledDataStorageSize(_diskMap->storageSize());
}

template <bool IsConst>
SpillableDocumentMapImpl::IteratorImpl<IsConst>::IteratorImpl(MapPointer map)
    : _map(map), _memIt(_map->_memMap.begin()) {
    if (_map->_diskMap != nullptr) {
        _diskIt = _map->_diskMap->getCursor(_map->_expCtx->getOperationContext());
        _diskItExhausted = false;
        saveDiskIt();

        if (memoryExhausted()) {
            readNextBatchFromDisk();
        }
    }
}

template <bool IsConst>
SpillableDocumentMapImpl::IteratorImpl<IsConst>::IteratorImpl(MapPointer map, const EndTag&)
    : _map(map), _memIt(_map->_memMap.end()) {}

template <bool IsConst>
bool SpillableDocumentMapImpl::IteratorImpl<IsConst>::operator==(const IteratorImpl& rhs) const {
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
                   rhs._diskDocuments.front().value().getField("_id")) == 0;
    }
    return true;
}

template <bool IsConst>
auto SpillableDocumentMapImpl::IteratorImpl<IsConst>::operator++() -> IteratorImpl& {
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
void SpillableDocumentMapImpl::IteratorImpl<IsConst>::releaseMemory() {
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
bool SpillableDocumentMapImpl::IteratorImpl<IsConst>::memoryExhausted() const {
    return _memIt == _map->_memMap.end();
}

template <bool IsConst>
bool SpillableDocumentMapImpl::IteratorImpl<IsConst>::diskExhausted() const {
    return _diskItExhausted && _diskDocuments.empty();
}

template <bool IsConst>
void SpillableDocumentMapImpl::IteratorImpl<IsConst>::readNextBatchFromDisk() {
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
auto SpillableDocumentMapImpl::IteratorImpl<IsConst>::getCurrentDocument() -> reference_type {
    if (!memoryExhausted()) {
        return _memIt->second.value();
    }
    if (!_diskDocuments.empty()) {
        return _diskDocuments.front().value();
    }
    tasserted(2398002, "dereferencing invalid SpillableDocumentMapImpl::IteratorImpl");
}

template <bool IsConst>
void SpillableDocumentMapImpl::IteratorImpl<IsConst>::restoreDiskIt() {
    _diskIt->reattachToOperationContext(_map->_expCtx->getOperationContext());
    bool restoreResult = _diskIt->restore();
    tassert(2398004, "Unable to restore disk cursor", restoreResult);
}

template <bool IsConst>
void SpillableDocumentMapImpl::IteratorImpl<IsConst>::saveDiskIt() {
    _diskIt->save();
    _diskIt->detachFromOperationContext();
}

template class SpillableDocumentMapImpl::IteratorImpl<true>;
template class SpillableDocumentMapImpl::IteratorImpl<false>;

void SpillableDocumentMap::add(Document document) {
    Value id = document.getField("_id");
    size_t size = document.getApproximateSize();
    _add(id, std::move(document), size);
}

void SpillableValueSet::add(Value value) {
    size_t size = value.getApproximateSize();
    _add(std::move(value), Document{}, size);
}


}  // namespace mongo
