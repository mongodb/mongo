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

#include <third_party/murmurhash3/MurmurHash3.h>

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_consistency.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_names.h"
#include "mongo/db/server_options.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/util/elapsed_tracker.h"

namespace mongo {

namespace {
// The number of items we can scan before we must yield.
static const int kScanLimit = 1000;

// TODO SERVER-36385: Completely remove the key size check in 4.4
bool largeKeyDisallowed() {
    return (serverGlobalParams.featureCompatibility.getVersion() ==
            ServerGlobalParams::FeatureCompatibility::Version::kFullyDowngradedTo40);
}
}  // namespace

IndexConsistency::IndexConsistency(OperationContext* opCtx,
                                   Collection* collection,
                                   NamespaceString nss,
                                   RecordStore* recordStore,
                                   const bool background)
    : _opCtx(opCtx),
      _collection(collection),
      _nss(nss),
      _recordStore(recordStore),
      _tracker(opCtx->getServiceContext()->getFastClockSource(),
               internalQueryExecYieldIterations.load(),
               Milliseconds(internalQueryExecYieldPeriodMS.load())) {

    IndexCatalog* indexCatalog = _collection->getIndexCatalog();
    std::unique_ptr<IndexCatalog::IndexIterator> indexIterator =
        indexCatalog->getIndexIterator(_opCtx, false);

    int indexNumber = 0;
    while (indexIterator->more()) {

        const IndexDescriptor* descriptor = indexIterator->next()->descriptor();
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

void IndexConsistency::addIndexKey(const KeyString& ks, int indexNumber) {

    if (indexNumber < 0 || indexNumber >= static_cast<int>(_indexesInfo.size())) {
        return;
    }

    stdx::lock_guard<stdx::mutex> lock(_classMutex);
    _addIndexKey_inlock(ks, indexNumber);
}

void IndexConsistency::addMultikeyMetadataPath(const KeyString& ks, int indexNumber) {
    if (indexNumber < 0) {
        return;
    }
    invariant(static_cast<size_t>(indexNumber) < _indexesInfo.size());

    stdx::lock_guard<stdx::mutex> lock(_classMutex);
    _indexesInfo[indexNumber].hashedMultikeyMetadataPaths.emplace(_hashKeyString(ks, indexNumber));
}

void IndexConsistency::removeMultikeyMetadataPath(const KeyString& ks, int indexNumber) {
    if (indexNumber < 0) {
        return;
    }
    invariant(static_cast<size_t>(indexNumber) < _indexesInfo.size());

    stdx::lock_guard<stdx::mutex> lock(_classMutex);
    _indexesInfo[indexNumber].hashedMultikeyMetadataPaths.erase(_hashKeyString(ks, indexNumber));
}

size_t IndexConsistency::getMultikeyMetadataPathCount(int indexNumber) {
    if (indexNumber < 0) {
        return 0;
    }
    invariant(static_cast<size_t>(indexNumber) < _indexesInfo.size());

    stdx::lock_guard<stdx::mutex> lock(_classMutex);
    return _indexesInfo[indexNumber].hashedMultikeyMetadataPaths.size();
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

int IndexConsistency::getIndexNumber(const std::string& indexNs) {

    auto search = _indexNumber.find(indexNs);
    if (search != _indexNumber.end()) {
        return search->second;
    }

    return -1;
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

void IndexConsistency::_addIndexKey_inlock(const KeyString& ks, int indexNumber) {

    // Ignore indexes that weren't ready before we started validation.
    if (!_indexesInfo.at(indexNumber).isReady) {
        return;
    }

    const uint32_t hash = _hashKeyString(ks, indexNumber);
    _indexKeyCount[hash]--;
    _indexesInfo.at(indexNumber).numKeys++;
}

uint32_t IndexConsistency::_hashKeyString(const KeyString& ks, int indexNumber) const {

    uint32_t indexNsHash = _indexesInfo.at(indexNumber).indexNsHash;
    MurmurHash3_x86_32(
        ks.getTypeBits().getBuffer(), ks.getTypeBits().getSize(), indexNsHash, &indexNsHash);
    MurmurHash3_x86_32(ks.getBuffer(), ks.getSize(), indexNsHash, &indexNsHash);
    return indexNsHash % (1U << 22);
}
}  // namespace mongo
