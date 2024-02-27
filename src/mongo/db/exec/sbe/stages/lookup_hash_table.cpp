/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/stages/lookup_hash_table.h"

#include "mongo/db/curop.h"
#include "mongo/db/exec/sbe/size_estimator.h"

namespace mongo::sbe {
////////////////////////////////////////////////////////////////////////////////////////////////////
// Class LookupHashTableIter
////////////////////////////////////////////////////////////////////////////////////////////////////

void LookupHashTableIter::initSearchArray() {
    invariant(_outerKeyIsArray);
    HashTableType::const_iterator hashTableMatchIter;

    value::ArrayEnumerator enumerator(_outerKeyTag, _outerKeyVal);
    while (!enumerator.atEnd()) {
        auto [tagElemView, valElemView] = enumerator.getViewOfValue();
        _iterProbeKey.reset(0, false, tagElemView, valElemView);
        hashTableMatchIter = _hashTable._memoryHt->find(_iterProbeKey);
        if (hashTableMatchIter != _hashTable._memoryHt->end()) {
            _hashTableMatchSet.insert(hashTableMatchIter->second.begin(),
                                      hashTableMatchIter->second.end());
        } else if (_hashTable._recordStoreHt) {
            // The key wasn't in memory. Check the '_hashTable._recordStoreHt' disk spill.
            auto [_, tagElemCollView, valElemCollView] =
                _hashTable.normalizeStringIfCollator(tagElemView, valElemView);
            boost::optional<std::vector<size_t>> indicesFromRS =
                _hashTable.readIndicesFromRecordStore(
                    _hashTable._recordStoreHt.get(), tagElemCollView, valElemCollView);
            if (indicesFromRS) {
                _hashTableMatchSet.insert(indicesFromRS->begin(), indicesFromRS->end());
            }
        }
        enumerator.advance();
    }  // while
    _hashTableMatchSetIter = _hashTableMatchSet.begin();
    _hashTableSearched = true;
}  // LookupHashTableIter::initSearchArray

void LookupHashTableIter::initSearchScalar() {
    invariant(!_outerKeyIsArray);
    HashTableType::const_iterator hashTableMatchIter;

    _iterProbeKey.reset(0, false, _outerKeyTag, _outerKeyVal);
    hashTableMatchIter = _hashTable._memoryHt->find(_iterProbeKey);
    if (hashTableMatchIter != _hashTable._memoryHt->end()) {
        _hashTableMatchVector = hashTableMatchIter->second;
        _hashTableMatchVectorIdx = 0;
    } else if (_hashTable._recordStoreHt) {
        // The key wasn't in memory. Check the '_hashTable._recordStoreHt' disk spill.
        auto [_, tagKeyCollView, valKeyCollView] =
            _hashTable.normalizeStringIfCollator(_outerKeyTag, _outerKeyVal);
        boost::optional<std::vector<size_t>> indicesFromRS = _hashTable.readIndicesFromRecordStore(
            _hashTable._recordStoreHt.get(), tagKeyCollView, valKeyCollView);
        if (indicesFromRS) {
            _hashTableMatchVector = std::move(indicesFromRS.get());
            _hashTableMatchVectorIdx = 0;
        }
    }
    _hashTableSearched = true;
}  // LookupHashTableIter::initSearchScalar

size_t LookupHashTableIter::getNextMatchingIndex() {
    // Iterator over matches of an individual outer key value in '_hashTable->_memoryHt'.
    if (_outerKeyIsArray) {
        // Outer key is an array. '_outerKeyTag', '_outerKeyVal' contain the key value.
        if (MONGO_unlikely(!_hashTableSearched)) {
            // This is the first time we are looking for this outer key. Build a sorted set of all
            // inner matches, if any, for all entries in this outer key array.
            initSearchArray();
        }

        // Return the next match, if any.
        if (_hashTableMatchSetIter != _hashTableMatchSet.end()) {
            return *(_hashTableMatchSetIter++);
        } else {
            return kNoMatchingIndex;
        }
    } else {
        // Outer key is a scalar. '_outerKeyTag', '_outerKeyVal' contain the key value.
        if (MONGO_unlikely(!_hashTableSearched)) {
            // This is the first time we are looking for this outer scalar key. Find its vector of
            // inner matches, if any.
            initSearchScalar();
        }

        // Return the next match, if any.
        if (_hashTableMatchVectorIdx < _hashTableMatchVector.size()) {
            return _hashTableMatchVector[_hashTableMatchVectorIdx++];
        } else {
            return kNoMatchingIndex;
        }
    }  // else outer key is a scalar
}  // LookupHashTableIter::getNextMatchingValue

void LookupHashTableIter::reset(const value::TypeTags& outerKeyTag,
                                const value::Value& outerKeyVal) {
    clear();
    _outerKeyTag = outerKeyTag;
    _outerKeyVal = outerKeyVal;
    if (value::isArray(_outerKeyTag)) {
        _outerKeyIsArray = true;
    } else {
        _outerKeyIsArray = false;
    }
}  // LookupHashTableIter::reset

////////////////////////////////////////////////////////////////////////////////////////////////////
// Class LookupHashTable
////////////////////////////////////////////////////////////////////////////////////////////////////

std::tuple<bool, value::TypeTags, value::Value> LookupHashTable::normalizeStringIfCollator(
    value::TypeTags tag, value::Value val) const {
    if (value::isString(tag) && _collator) {
        auto [tagColl, valColl] = value::makeNewString(
            _collator->getComparisonKey(value::getStringView(tag, val)).getKeyData());
        return {true, tagColl, valColl};
    }
    return {false, tag, val};
}

boost::optional<std::vector<size_t>> LookupHashTable::readIndicesFromRecordStore(
    SpillingStore* rs, value::TypeTags tagKey, value::Value valKey) {
    _htProbeKey.reset(0, false, tagKey, valKey);

    auto [rid, _] = serializeKeyForRecordStore(_htProbeKey);
    RecordData record;
    if (rs->findRecord(_opCtx, rid, &record)) {
        // 'BufBuilder' writes numbers in little endian format, so must read them using the same.
        auto valueReader = BufReader(record.data(), record.size());
        auto nRecords = valueReader.read<LittleEndian<size_t>>();
        std::vector<size_t> result(nRecords);
        for (size_t i = 0; i < nRecords; ++i) {
            auto idx = valueReader.read<LittleEndian<size_t>>();
            result[i] = idx;
        }
        return result;
    }
    return boost::none;
}

void LookupHashTable::addHashTableEntry(value::SlotAccessor* keyAccessor, size_t valueIndex) {
    // Adds a new key-value entry. Will attempt to move or copy from key accessor when needed.
    // array case each elem in array we put each element into ht.
    auto [tagKeyView, valKeyView] = keyAccessor->getViewOfValue();
    _htProbeKey.reset(0, false, tagKeyView, valKeyView);

    // Check to see if key is already in memory. If not, we will emplace a new key or spill to disk.
    auto htIt = _memoryHt->find(_htProbeKey);
    if (htIt == _memoryHt->end()) {
        // If the key and one 'size_t' index fit into the '_memoryHt' without reaching the memory
        // limit and we haven't spilled yet emplace into '_memoryHt'. Otherwise, we will always
        // spill the key to the record store. The additional guard !hasSpilledHtToDisk() ensures
        // that a key that is evicted from '_memoryHt' never ends in '_memoryHt' again.
        const long long newMemUsage = _computedTotalMemUsage +
            size_estimator::estimate(tagKeyView, valKeyView) + sizeof(size_t);

        value::MaterializedRow key{1};
        if (!hasSpilledHtToDisk() && newMemUsage <= _memoryUseInBytesBeforeSpill) {
            // We have to insert an owned key, attempt a move, but force copy if necessary when we
            // haven't spilled to the '_recordStore' yet.
            auto [tagKey, valKey] = keyAccessor->getCopyOfValue();
            key.reset(0, true, tagKey, valKey);

            auto [it, inserted] = _memoryHt->try_emplace(std::move(key));
            invariant(inserted);
            htIt = it;
            htIt->second.push_back(valueIndex);
            _computedTotalMemUsage = newMemUsage;
        } else {
            // Write record to rs.
            if (!hasSpilledHtToDisk()) {
                makeTemporaryRecordStore();
            }

            auto val = std::vector<size_t>{valueIndex};
            auto [tagKey, valKey] = keyAccessor->getViewOfValue();
            spillIndicesToRecordStore(_recordStoreHt.get(), tagKey, valKey, val);
        }
    } else {
        // The key is already present in '_memoryHt' so the memory will only grow by one size_t. If
        // we reach the memory limit, the key/value in '_memoryHt' will be evicted from memory and
        // spilled to
        // '_recordStoreHt' along with the new index.
        const long long newMemUsage = _computedTotalMemUsage + sizeof(size_t);
        if (newMemUsage <= _memoryUseInBytesBeforeSpill) {
            htIt->second.push_back(valueIndex);
            _computedTotalMemUsage = newMemUsage;
        } else {
            if (!hasSpilledHtToDisk()) {
                makeTemporaryRecordStore();
            }

            value::MaterializedRow key{1};
            key.reset(0, true, tagKeyView, valKeyView);
            _computedTotalMemUsage -= size_estimator::estimate(tagKeyView, valKeyView);

            // Evict the hash table value.
            _computedTotalMemUsage -= htIt->second.size() * sizeof(size_t);
            htIt->second.push_back(valueIndex);
            spillIndicesToRecordStore(_recordStoreHt.get(), tagKeyView, valKeyView, htIt->second);
            _memoryHt->erase(htIt);
        }
    }
}  // LookupHashTable::addHashTableEntry

void LookupHashTable::spillBufferedValueToDisk(SpillingStore* rs,
                                               size_t bufferIdx,
                                               const value::MaterializedRow& val) {
    CurOp::get(_opCtx)->debug().hashLookupSpillToDisk += 1;

    RecordId rid = getValueRecordId(bufferIdx);

    BufBuilder buf;
    val.serializeForSorter(buf);

    rs->upsertToRecordStore(_opCtx, rid, buf, false);

    _hashLookupStats.spilledBuffRecords++;
    // Add size of record ID + size of buffer.
    _hashLookupStats.spilledBuffBytesOverAllRecords += sizeof(size_t) + buf.len();
    return;
}

size_t LookupHashTable::bufferValueOrSpill(value::MaterializedRow& value) {
    const long long newMemUsage = _computedTotalMemUsage + size_estimator::estimate(value);
    if (!hasSpilledBufToDisk() && newMemUsage <= _memoryUseInBytesBeforeSpill) {
        _buffer.emplace_back(std::move(value));
        _computedTotalMemUsage = newMemUsage;
    } else {
        if (!hasSpilledBufToDisk()) {
            makeTemporaryRecordStore();
        }
        spillBufferedValueToDisk(_recordStoreBuf.get(), _valueId, value);
    }
    return _valueId++;
}

void LookupHashTable::writeIndicesToRecordStore(SpillingStore* rs,
                                                value::TypeTags tagKey,
                                                value::Value valKey,
                                                const std::vector<size_t>& value,
                                                bool update) {
    BufBuilder buf;
    buf.appendNum(value.size());  // number of indices
    for (auto& idx : value) {
        buf.appendNum(static_cast<size_t>(idx));
    }

    value::MaterializedRow key{1};
    key.reset(0, false, tagKey, valKey);
    auto [rid, typeBits] = serializeKeyForRecordStore(key);

    rs->upsertToRecordStore(_opCtx, rid, buf, typeBits, update);
    if (!update) {
        _hashLookupStats.spilledHtRecords++;
        // Add the size of key (which comprises of the memory usage for the key + its type bits),
        // as well as the size of one integer to store the length of indices vector in the value.
        _hashLookupStats.spilledHtBytesOverAllRecords +=
            rid.memUsage() + typeBits.getSize() + sizeof(size_t);
    }
    // Add the size of indices vector used in the hash-table value to the accounting.
    _hashLookupStats.spilledHtBytesOverAllRecords += value.size() * sizeof(size_t);
}

void LookupHashTable::spillIndicesToRecordStore(SpillingStore* rs,
                                                value::TypeTags tagKey,
                                                value::Value valKey,
                                                const std::vector<size_t>& value) {
    CurOp::get(_opCtx)->debug().hashLookupSpillToDisk += 1;

    auto [owned, tagKeyColl, valKeyColl] = normalizeStringIfCollator(tagKey, valKey);
    _htProbeKey.reset(0, owned, tagKeyColl, valKeyColl);

    auto valFromRs = readIndicesFromRecordStore(rs, tagKeyColl, valKeyColl);

    auto update = false;
    if (valFromRs) {
        valFromRs->insert(valFromRs->end(), value.begin(), value.end());
        update = true;
        // As we're updating these records, we'd remove the old size from the accounting. The new
        // size is added back to the accounting in the call to 'writeIndicesToRecordStore' below.
        _hashLookupStats.spilledHtBytesOverAllRecords -= value.size();
    } else {
        valFromRs = value;
    }

    writeIndicesToRecordStore(rs, tagKeyColl, valKeyColl, *valFromRs, update);
}

void LookupHashTable::makeTemporaryRecordStore() {
    tassert(8229808,
            "HashLookupUnwindStage attempted to write to disk in an environment which is not "
            "prepared to do so",
            _opCtx->getServiceContext());
    tassert(8229809,
            "No storage engine so HashLookupUnwindStage cannot spill to disk",
            _opCtx->getServiceContext()->getStorageEngine());
    assertIgnorePrepareConflictsBehavior(_opCtx);

    _recordStoreBuf = std::make_unique<SpillingStore>(_opCtx, KeyFormat::Long);
    _recordStoreHt = std::make_unique<SpillingStore>(_opCtx, KeyFormat::String);
    _hashLookupStats.usedDisk = true;
}

std::pair<RecordId, key_string::TypeBits> LookupHashTable::serializeKeyForRecordStore(
    const value::MaterializedRow& key) const {
    key_string::Builder kb{key_string::Version::kLatestVersion};
    return encodeKeyString(kb, key);
}

boost::optional<std::pair<value::TypeTags, value::Value>> LookupHashTable::getValueAtIndex(
    size_t index) {
    if (index < _buffer.size()) {
        // Document is in memory buffer, always in column 0.
        return _buffer[index].getViewOfValue(0);
    } else if (_recordStoreBuf) {
        // Document is in disk buffer, always in column 0. The MaterializedRow object constructed
        // from this must be copied to a place that does not go out of scope when this method
        // returns, for which we use '_bufValueRecordStore'.
        _bufValueRecordStore =
            _recordStoreBuf->readFromRecordStore(_opCtx, LookupHashTable::getValueRecordId(index));
        if (_bufValueRecordStore) {
            return _bufValueRecordStore->getViewOfValue(0);
        }
    }
    tasserted(8229807,
              str::stream() << "index " << index
                            << " not found in hash table store."
                               " _buffer.size(): "
                            << _buffer.size() << ", _recordStoreBuf "
                            << (_recordStoreBuf ? "exists." : "does not exist."));
    return boost::none;
}  // HashLookupUnwindStage::getValueAtIndex

void LookupHashTable::reset(bool fromClose) {
    _memoryUseInBytesBeforeSpill = internalQuerySBELookupApproxMemoryUseInBytesBeforeSpill.load();
    _memoryHt = boost::none;
    if (_recordStoreHt) {
        _recordStoreHt.reset(nullptr);
    }
    if (_recordStoreBuf) {
        _recordStoreBuf.reset(nullptr);
    }

    // Erase but don't change its reference, as 'HashLookupStage::_outInnerBufferProjectAccessor'
    // contain a reference to this buffer.
    _buffer.clear();
    if (fromClose) {
        _buffer.shrink_to_fit();
    }

    _valueId = 0;
    htIter.clear();
}

void LookupHashTable::doSaveState(bool relinquishCursor) {
    if (_recordStoreHt) {
        _recordStoreHt->saveState();
    }
    if (_recordStoreBuf) {
        _recordStoreBuf->saveState();
    }
}

void LookupHashTable::doRestoreState(bool relinquishCursor) {
    if (_recordStoreHt) {
        _recordStoreHt->restoreState();
    }
    if (_recordStoreBuf) {
        _recordStoreBuf->restoreState();
    }
}
}  // namespace mongo::sbe
