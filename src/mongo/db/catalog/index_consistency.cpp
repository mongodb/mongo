/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
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

#include <third_party/murmurhash3/MurmurHash3.h>

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_consistency.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/query/query_yield.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/util/elapsed_tracker.h"

namespace mongo {

namespace {
// The number of items we can scan before we must yield.
static const int kScanLimit = 1000;
}  // namespace

IndexConsistency::IndexConsistency(OperationContext* opCtx,
                                   Collection* collection,
                                   NamespaceString nss,
                                   RecordStore* recordStore,
                                   std::unique_ptr<Lock::CollectionLock> collLk,
                                   const bool background)
    : _opCtx(opCtx),
      _collection(collection),
      _nss(nss),
      _recordStore(recordStore),
      _collLk(std::move(collLk)),
      _isBackground(background),
      _tracker(opCtx->getServiceContext()->getFastClockSource(),
               internalQueryExecYieldIterations.load(),
               Milliseconds(internalQueryExecYieldPeriodMS.load())) {

    IndexCatalog* indexCatalog = _collection->getIndexCatalog();
    IndexCatalog::IndexIterator indexIterator = indexCatalog->getIndexIterator(_opCtx, false);

    int indexNumber = 0;
    while (indexIterator.more()) {

        const IndexDescriptor* descriptor = indexIterator.next();
        std::string indexNs = descriptor->indexNamespace();

        _indexNumber[descriptor->indexNamespace()] = indexNumber;

        IndexInfo indexInfo;

        indexInfo.isReady =
            _collection->getCatalogEntry()->isIndexReady(opCtx, descriptor->indexName());

        uint32_t indexNsHash;
        MurmurHash3_x86_32(indexNs.c_str(), indexNs.size(), 0, &indexNsHash);
        indexInfo.indexNsHash = indexNsHash;
        indexInfo.indexScanFinished = false;

        indexInfo.numKeys = 0;
        indexInfo.numLongKeys = 0;
        indexInfo.numRecords = 0;
        indexInfo.numExtraIndexKeys = 0;

        _indexesInfo[indexNumber] = indexInfo;

        indexNumber++;
    }
}

void IndexConsistency::addDocKey(const KeyString& ks, int indexNumber) {

    if (indexNumber < 0 || indexNumber >= static_cast<int>(_indexesInfo.size())) {
        return;
    }

    stdx::lock_guard<stdx::mutex> lock(_classMutex);
    _addDocKey_inlock(ks, indexNumber);
}

void IndexConsistency::removeDocKey(const KeyString& ks, int indexNumber) {

    if (indexNumber < 0 || indexNumber >= static_cast<int>(_indexesInfo.size())) {
        return;
    }

    stdx::lock_guard<stdx::mutex> lock(_classMutex);
    _removeDocKey_inlock(ks, indexNumber);
}

void IndexConsistency::addIndexKey(const KeyString& ks, int indexNumber) {

    if (indexNumber < 0 || indexNumber >= static_cast<int>(_indexesInfo.size())) {
        return;
    }

    stdx::lock_guard<stdx::mutex> lock(_classMutex);
    _addIndexKey_inlock(ks, indexNumber);
}

void IndexConsistency::removeIndexKey(const KeyString& ks, int indexNumber) {

    if (indexNumber < 0 || indexNumber >= static_cast<int>(_indexesInfo.size())) {
        return;
    }

    stdx::lock_guard<stdx::mutex> lock(_classMutex);
    _removeIndexKey_inlock(ks, indexNumber);
}

void IndexConsistency::addLongIndexKey(int indexNumber) {

    stdx::lock_guard<stdx::mutex> lock(_classMutex);
    if (indexNumber < 0 || indexNumber >= static_cast<int>(_indexesInfo.size())) {
        return;
    }

    _indexesInfo[indexNumber].numRecords++;
    _indexesInfo[indexNumber].numLongKeys++;
}

int64_t IndexConsistency::getNumKeys(int indexNumber) const {

    stdx::lock_guard<stdx::mutex> lock(_classMutex);
    if (indexNumber < 0 || indexNumber >= static_cast<int>(_indexesInfo.size())) {
        return 0;
    }

    return _indexesInfo.at(indexNumber).numKeys;
}

int64_t IndexConsistency::getNumLongKeys(int indexNumber) const {

    stdx::lock_guard<stdx::mutex> lock(_classMutex);
    if (indexNumber < 0 || indexNumber >= static_cast<int>(_indexesInfo.size())) {
        return 0;
    }

    return _indexesInfo.at(indexNumber).numLongKeys;
}

int64_t IndexConsistency::getNumRecords(int indexNumber) const {

    stdx::lock_guard<stdx::mutex> lock(_classMutex);
    if (indexNumber < 0 || indexNumber >= static_cast<int>(_indexesInfo.size())) {
        return 0;
    }

    return _indexesInfo.at(indexNumber).numRecords;
}

bool IndexConsistency::haveEntryMismatch() const {

    stdx::lock_guard<stdx::mutex> lock(_classMutex);
    for (auto iterator = _indexKeyCount.begin(); iterator != _indexKeyCount.end(); iterator++) {
        if (iterator->second != 0) {
            return true;
        }
    }

    return false;
}

int64_t IndexConsistency::getNumExtraIndexKeys(int indexNumber) const {

    stdx::lock_guard<stdx::mutex> lock(_classMutex);
    if (indexNumber < 0 || indexNumber >= static_cast<int>(_indexesInfo.size())) {
        return 0;
    }

    return _indexesInfo.at(indexNumber).numExtraIndexKeys;
}

void IndexConsistency::applyChange(const IndexDescriptor* descriptor,
                                   const boost::optional<IndexKeyEntry>& indexEntry,
                                   ValidationOperation operation) {

    stdx::lock_guard<stdx::mutex> lock(_classMutex);

    const std::string& indexNs = descriptor->indexNamespace();
    int indexNumber = getIndexNumber(indexNs);
    if (indexNumber == -1) {
        return;
    }

    // Ignore indexes that weren't ready before we started validation.
    if (!_indexesInfo.at(indexNumber).isReady) {
        return;
    }

    const auto& key = descriptor->keyPattern();
    const Ordering ord = Ordering::make(key);
    KeyString::Version version = KeyString::kLatestVersion;

    KeyString ks(version, indexEntry->key, ord, indexEntry->loc);

    if (_stage == ValidationStage::DOCUMENT) {
        _setYieldAtRecord_inlock(indexEntry->loc);
        if (_isBeforeLastProcessedRecordId_inlock(indexEntry->loc)) {
            if (operation == ValidationOperation::INSERT) {
                if (indexEntry->key.objsize() >=
                    static_cast<int64_t>(KeyString::TypeBits::kMaxKeyBytes)) {
                    // Index keys >= 1024 bytes are not indexed but are stored in the document key
                    // set.
                    _indexesInfo[indexNumber].numRecords++;
                    _indexesInfo[indexNumber].numLongKeys++;
                } else {
                    _addDocKey_inlock(ks, indexNumber);
                }
            } else if (operation == ValidationOperation::REMOVE) {
                if (indexEntry->key.objsize() >=
                    static_cast<int64_t>(KeyString::TypeBits::kMaxKeyBytes)) {
                    _indexesInfo[indexNumber].numRecords--;
                    _indexesInfo[indexNumber].numLongKeys--;
                } else {
                    _removeDocKey_inlock(ks, indexNumber);
                }
            }
        }
    } else if (_stage == ValidationStage::INDEX) {

        // Index entries with key sizes >= 1024 bytes are not indexed.
        if (indexEntry->key.objsize() >= static_cast<int64_t>(KeyString::TypeBits::kMaxKeyBytes)) {
            return;
        }

        if (_isIndexScanning_inlock(indexNumber)) {
            _setYieldAtIndexEntry_inlock(ks);
        }

        const bool wasIndexScanStarted =
            _isIndexFinished_inlock(indexNumber) || _isIndexScanning_inlock(indexNumber);
        const bool isUpcomingChangeToCurrentIndex =
            _isIndexScanning_inlock(indexNumber) && !_isBeforeLastProcessedIndexEntry_inlock(ks);

        if (!wasIndexScanStarted || isUpcomingChangeToCurrentIndex) {

            // We haven't started scanning this index namespace yet so everything
            // happens after the cursor, OR, we are scanning this index namespace,
            // and an event occured after our cursor
            if (operation == ValidationOperation::INSERT) {
                _removeIndexKey_inlock(ks, indexNumber);
                _indexesInfo.at(indexNumber).numExtraIndexKeys++;
            } else if (operation == ValidationOperation::REMOVE) {
                _addIndexKey_inlock(ks, indexNumber);
                _indexesInfo.at(indexNumber).numExtraIndexKeys--;
            }
        }
    }
}


void IndexConsistency::nextStage() {

    stdx::lock_guard<stdx::mutex> lock(_classMutex);
    if (_stage == ValidationStage::DOCUMENT) {
        _stage = ValidationStage::INDEX;
    } else if (_stage == ValidationStage::INDEX) {
        _stage = ValidationStage::NONE;
    }
}

ValidationStage IndexConsistency::getStage() const {

    stdx::lock_guard<stdx::mutex> lock(_classMutex);
    return _stage;
}

void IndexConsistency::setLastProcessedRecordId(RecordId recordId) {

    stdx::lock_guard<stdx::mutex> lock(_classMutex);
    if (!recordId.isNormal()) {
        _lastProcessedRecordId = boost::none;
    } else {
        _lastProcessedRecordId = recordId;
    }
}

void IndexConsistency::setLastProcessedIndexEntry(
    const IndexDescriptor& descriptor, const boost::optional<IndexKeyEntry>& indexEntry) {

    const auto& key = descriptor.keyPattern();
    const Ordering ord = Ordering::make(key);
    KeyString::Version version = KeyString::kLatestVersion;

    stdx::lock_guard<stdx::mutex> lock(_classMutex);
    if (!indexEntry) {
        _lastProcessedIndexEntry.reset();
    } else {
        _lastProcessedIndexEntry.reset(
            new KeyString(version, indexEntry->key, ord, indexEntry->loc));
    }
}

void IndexConsistency::notifyStartIndex(int indexNumber) {

    stdx::lock_guard<stdx::mutex> lock(_classMutex);
    if (indexNumber < 0 || indexNumber >= static_cast<int>(_indexesInfo.size())) {
        return;
    }

    _lastProcessedIndexEntry.reset(nullptr);
    _currentIndex = indexNumber;
}

void IndexConsistency::notifyDoneIndex(int indexNumber) {

    stdx::lock_guard<stdx::mutex> lock(_classMutex);
    if (indexNumber < 0 || indexNumber >= static_cast<int>(_indexesInfo.size())) {
        return;
    }

    _lastProcessedIndexEntry.reset(nullptr);
    _currentIndex = -1;
    _indexesInfo.at(indexNumber).indexScanFinished = true;
}

int IndexConsistency::getIndexNumber(const std::string& indexNs) {

    auto search = _indexNumber.find(indexNs);
    if (search != _indexNumber.end()) {
        return search->second;
    }

    return -1;
}

bool IndexConsistency::shouldGetNewSnapshot(const RecordId recordId) const {

    stdx::lock_guard<stdx::mutex> lock(_classMutex);
    if (!_yieldAtRecordId) {
        return false;
    }

    return _yieldAtRecordId <= recordId;
}

bool IndexConsistency::shouldGetNewSnapshot(const KeyString& keyString) const {

    stdx::lock_guard<stdx::mutex> lock(_classMutex);
    if (!_yieldAtIndexEntry) {
        return false;
    }

    return *_yieldAtIndexEntry <= keyString;
}

void IndexConsistency::relockCollectionWithMode(LockMode mode) {
    // Release the lock and grab the provided lock mode.
    _collLk.reset();
    _collLk.reset(new Lock::CollectionLock(_opCtx->lockState(), _nss.toString(), mode));
    invariant(_opCtx->lockState()->isCollectionLockedForMode(_nss.toString(), mode));

    // Check if the operation was killed.
    _opCtx->checkForInterrupt();

    // Ensure it is safe to continue.
    uassertStatusOK(_throwExceptionIfError());
}

bool IndexConsistency::scanLimitHit() {

    stdx::lock_guard<stdx::mutex> lock(_classMutex);

    // We have to yield every so many scans while doing background validation only.
    return _isBackground && _tracker.intervalHasElapsed();
}

void IndexConsistency::_addDocKey_inlock(const KeyString& ks, int indexNumber) {

    // Ignore indexes that weren't ready before we started validation.
    if (!_indexesInfo.at(indexNumber).isReady) {
        return;
    }

    const uint32_t hash = _hashKeyString(ks, indexNumber);
    _indexKeyCount[hash]++;
    _indexesInfo.at(indexNumber).numRecords++;
}

void IndexConsistency::_removeDocKey_inlock(const KeyString& ks, int indexNumber) {

    // Ignore indexes that weren't ready before we started validation.
    if (!_indexesInfo.at(indexNumber).isReady) {
        return;
    }

    const uint32_t hash = _hashKeyString(ks, indexNumber);
    _indexKeyCount[hash]--;
    _indexesInfo.at(indexNumber).numRecords--;
}

void IndexConsistency::_addIndexKey_inlock(const KeyString& ks, int indexNumber) {

    // Ignore indexes that weren't ready before we started validation.
    if (!_indexesInfo.at(indexNumber).isReady) {
        return;
    }

    const uint32_t hash = _hashKeyString(ks, indexNumber);
    _indexKeyCount[hash]--;
    _indexesInfo.at(indexNumber).numKeys++;
}

void IndexConsistency::_removeIndexKey_inlock(const KeyString& ks, int indexNumber) {

    // Ignore indexes that weren't ready before we started validation.
    if (!_indexesInfo.at(indexNumber).isReady) {
        return;
    }

    const uint32_t hash = _hashKeyString(ks, indexNumber);
    _indexKeyCount[hash]++;
    _indexesInfo.at(indexNumber).numKeys--;
}

bool IndexConsistency::_isIndexFinished_inlock(int indexNumber) const {

    return _indexesInfo.at(indexNumber).indexScanFinished;
}

bool IndexConsistency::_isIndexScanning_inlock(int indexNumber) const {

    return indexNumber == _currentIndex;
}

void IndexConsistency::_setYieldAtRecord_inlock(const RecordId recordId) {

    if (_isBeforeLastProcessedRecordId_inlock(recordId)) {
        return;
    }

    if (!_yieldAtRecordId || recordId <= _yieldAtRecordId) {
        _yieldAtRecordId = recordId;
    }
}

void IndexConsistency::_setYieldAtIndexEntry_inlock(const KeyString& keyString) {

    if (_isBeforeLastProcessedIndexEntry_inlock(keyString)) {
        return;
    }

    if (!_yieldAtIndexEntry || keyString <= *_yieldAtIndexEntry) {
        KeyString::Version version = KeyString::kLatestVersion;
        _yieldAtIndexEntry.reset(new KeyString(version));
        _yieldAtIndexEntry->resetFromBuffer(keyString.getBuffer(), keyString.getSize());
    }
}

bool IndexConsistency::_isBeforeLastProcessedRecordId_inlock(RecordId recordId) const {

    if (_lastProcessedRecordId && recordId <= _lastProcessedRecordId) {
        return true;
    }

    return false;
}

bool IndexConsistency::_isBeforeLastProcessedIndexEntry_inlock(const KeyString& keyString) const {

    if (_lastProcessedIndexEntry && keyString <= *_lastProcessedIndexEntry) {
        return true;
    }

    return false;
}

uint32_t IndexConsistency::_hashKeyString(const KeyString& ks, int indexNumber) const {

    uint32_t indexNsHash = _indexesInfo.at(indexNumber).indexNsHash;
    MurmurHash3_x86_32(
        ks.getTypeBits().getBuffer(), ks.getTypeBits().getSize(), indexNsHash, &indexNsHash);
    MurmurHash3_x86_32(ks.getBuffer(), ks.getSize(), indexNsHash, &indexNsHash);
    return indexNsHash % (1U << 22);
}

Status IndexConsistency::_throwExceptionIfError() {

    Database* database = dbHolder().get(_opCtx, _nss.db());

    // Ensure the database still exists.
    if (!database) {
        return Status(ErrorCodes::NamespaceNotFound,
                      "The database was dropped during background validation");
    }

    Collection* collection = database->getCollection(_opCtx, _nss);

    // Ensure the collection still exists.
    if (!collection) {
        return Status(ErrorCodes::NamespaceNotFound,
                      "The collection was dropped during background validation");
    }

    // Ensure no indexes were removed or added.
    IndexCatalog* indexCatalog = collection->getIndexCatalog();
    IndexCatalog::IndexIterator indexIterator = indexCatalog->getIndexIterator(_opCtx, false);
    int numRelevantIndexes = 0;

    while (indexIterator.more()) {
        const IndexDescriptor* descriptor = indexIterator.next();
        int indexNumber = getIndexNumber(descriptor->indexNamespace());
        if (indexNumber == -1) {
            // Allow the collection scan to finish to verify that all the records are valid BSON.
            if (_stage != ValidationStage::DOCUMENT) {
                // An index was added.
                return Status(ErrorCodes::IndexModified,
                              "An index was added during background validation");
            }
        } else {
            // Ignore indexes that weren't ready
            if (_indexesInfo.at(indexNumber).isReady) {
                numRelevantIndexes++;
            }
        }
    }

    if (numRelevantIndexes != static_cast<int>(_indexesInfo.size())) {
        // Allow the collection scan to finish to verify that all the records are valid BSON.
        if (_stage != ValidationStage::DOCUMENT) {
            // An index was dropped.
            return Status(ErrorCodes::IndexModified,
                          "An index was dropped during background validation");
        }
    }

    return Status::OK();
}
}  // namespace mongo
