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

#include <algorithm>
#include <third_party/murmurhash3/MurmurHash3.h>

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_consistency.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_names.h"
#include "mongo/db/server_options.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/util/elapsed_tracker.h"
#include "mongo/util/string_map.h"

namespace mongo {

namespace {

// TODO SERVER-36385: Completely remove the key size check in 4.4
bool largeKeyDisallowed() {
    return (serverGlobalParams.featureCompatibility.getVersion() ==
            ServerGlobalParams::FeatureCompatibility::Version::kFullyDowngradedTo40);
}

// The number of items we can scan before we must yield.
static const int kScanLimit = 1000;
static const size_t kNumHashBuckets = 1U << 16;

StringSet::hasher hash;

/**
 * Returns a key for the '_extraIndexEntries' and '_missingIndexEntries' maps. The key is a pair
 * of index name and the index key represented in KeyString form.
 * Using the index name is required as the index keys are passed in as KeyStrings which do not
 * contain field names.
 *
 * If we had the following document: { a: 1, b: 1 } with two indexes on keys "a" and "b", then
 * the KeyStrings for the index keys of the document would be identical as the field name in the
 * KeyString is not present. The BSON representation of this would look like: { : 1 } for both.
 * To distinguish these as different index keys, return a pair of index name and index key.
 */
std::pair<std::string, std::string> _generateKeyForMap(const IndexInfo& indexInfo,
                                                       const KeyString& ks) {
    return std::make_pair(indexInfo.descriptor->indexName(),
                          std::string(ks.getBuffer(), ks.getSize()));
}

}  // namespace

IndexInfo::IndexInfo(const IndexDescriptor* descriptor)
    : descriptor(descriptor),
      indexNameHash(hash(descriptor->indexName())),
      ord(Ordering::make(descriptor->keyPattern())) {}

IndexConsistency::IndexConsistency(OperationContext* opCtx,
                                   Collection* collection,
                                   NamespaceString nss,
                                   RecordStore* recordStore,
                                   bool background)
    : _opCtx(opCtx),
      _collection(collection),
      _nss(nss),
      _recordStore(recordStore),
      _tracker(opCtx->getServiceContext()->getFastClockSource(),
               internalQueryExecYieldIterations.load(),
               Milliseconds(internalQueryExecYieldPeriodMS.load())),
      _firstPhase(true) {
    _indexKeyCount.resize(kNumHashBuckets);

    IndexCatalog* indexCatalog = _collection->getIndexCatalog();
    std::unique_ptr<IndexCatalog::IndexIterator> indexIterator =
        indexCatalog->getIndexIterator(_opCtx, false);

    while (indexIterator->more()) {
        const IndexDescriptor* descriptor = indexIterator->next()->descriptor();
        if (DurableCatalog::get(opCtx)->isIndexReady(opCtx, nss, descriptor->indexName()))
            _indexesInfo.emplace(descriptor->indexName(), IndexInfo(descriptor));
    }
}

void IndexConsistency::addMultikeyMetadataPath(const KeyString& ks, IndexInfo* indexInfo) {
    indexInfo->hashedMultikeyMetadataPaths.emplace(_hashKeyString(ks, indexInfo->indexNameHash));
}

void IndexConsistency::removeMultikeyMetadataPath(const KeyString& ks, IndexInfo* indexInfo) {
    indexInfo->hashedMultikeyMetadataPaths.erase(_hashKeyString(ks, indexInfo->indexNameHash));
}

size_t IndexConsistency::getMultikeyMetadataPathCount(IndexInfo* indexInfo) {
    return indexInfo->hashedMultikeyMetadataPaths.size();
}

void IndexConsistency::addLongIndexKey(IndexInfo* indexInfo) {
    indexInfo->numRecords++;
    indexInfo->numLongKeys++;
}

bool IndexConsistency::haveEntryMismatch() const {
    return std::any_of(
        _indexKeyCount.begin(), _indexKeyCount.end(), [](int count) -> bool { return count; });
}

void IndexConsistency::setSecondPhase() {
    invariant(_firstPhase);
    _firstPhase = false;
}

void IndexConsistency::addIndexEntryErrors(ValidateResultsMap* indexNsResultsMap,
                                           ValidateResults* results) {
    invariant(!_firstPhase);

    // We'll report up to 1MB for extra index entry errors and missing index entry errors.
    const int kErrorSizeBytes = 1 * 1024 * 1024;
    long numMissingIndexEntriesSizeBytes = 0;
    long numExtraIndexEntriesSizeBytes = 0;

    int numMissingIndexEntryErrors = _missingIndexEntries.size();
    int numExtraIndexEntryErrors = 0;
    for (const auto& item : _extraIndexEntries) {
        numExtraIndexEntryErrors += item.second.size();
    }

    // Sort missing index entries by size so we can process in order of increasing size and return
    // as many as possible within memory limits.
    using MissingIt = decltype(_missingIndexEntries)::const_iterator;
    std::vector<MissingIt> missingIndexEntriesBySize;
    missingIndexEntriesBySize.reserve(_missingIndexEntries.size());
    for (auto it = _missingIndexEntries.begin(); it != _missingIndexEntries.end(); ++it) {
        missingIndexEntriesBySize.push_back(it);
    }
    std::sort(missingIndexEntriesBySize.begin(),
              missingIndexEntriesBySize.end(),
              [](const MissingIt& a, const MissingIt& b) {
                  return a->second.objsize() < b->second.objsize();
              });

    // Inform which indexes have inconsistencies and add the BSON objects of the inconsistent index
    // entries to the results vector.
    bool missingIndexEntrySizeLimitWarning = false;
    bool first = true;
    for (const auto& it : missingIndexEntriesBySize) {
        const auto& entry = it->second;

        numMissingIndexEntriesSizeBytes += entry.objsize();
        if (first || numMissingIndexEntriesSizeBytes <= kErrorSizeBytes) {
            results->missingIndexEntries.push_back(entry);
            first = false;
        } else if (!missingIndexEntrySizeLimitWarning) {
            StringBuilder ss;
            ss << "Not all missing index entry inconsistencies are listed due to size limitations.";
            results->errors.push_back(ss.str());

            missingIndexEntrySizeLimitWarning = true;
        }

        std::string indexName = entry["indexName"].String();
        if (!indexNsResultsMap->at(indexName).valid) {
            continue;
        }

        StringBuilder ss;
        ss << "Index with name '" << indexName << "' has inconsistencies.";
        results->errors.push_back(ss.str());

        indexNsResultsMap->at(indexName).valid = false;
    }

    // Sort extra index entries by size so we can process in order of increasing size and return as
    // many as possible within memory limits.
    using ExtraIt = SimpleBSONObjSet::const_iterator;
    std::vector<ExtraIt> extraIndexEntriesBySize;
    // Since the extra entries are stored in a map of sets, we have to iterate the entries in the
    // map and sum the size of the sets in order to get the total number. Given that we can have at
    // most 64 indexes per collection, and the total number of entries could potentially be in the
    // millions, we expect that iterating the map will be much less costly than the additional
    // allocations and copies that could result from not calling 'reserve' on the vector.
    size_t totalExtraIndexEntriesCount =
        std::accumulate(_extraIndexEntries.begin(),
                        _extraIndexEntries.end(),
                        0,
                        [](size_t total, const std::pair<IndexKey, SimpleBSONObjSet>& set) {
                            return total + set.second.size();
                        });
    extraIndexEntriesBySize.reserve(totalExtraIndexEntriesCount);
    for (const auto& extraIndexEntry : _extraIndexEntries) {
        const SimpleBSONObjSet& entries = extraIndexEntry.second;
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            extraIndexEntriesBySize.push_back(it);
        }
    }
    std::sort(extraIndexEntriesBySize.begin(),
              extraIndexEntriesBySize.end(),
              [](const ExtraIt& a, const ExtraIt& b) { return a->objsize() < b->objsize(); });

    bool extraIndexEntrySizeLimitWarning = false;
    for (const auto& entry : extraIndexEntriesBySize) {
        numExtraIndexEntriesSizeBytes += entry->objsize();
        if (first || numExtraIndexEntriesSizeBytes <= kErrorSizeBytes) {
            results->extraIndexEntries.push_back(*entry);
            first = false;
        } else if (!extraIndexEntrySizeLimitWarning) {
            StringBuilder ss;
            ss << "Not all extra index entry inconsistencies are listed due to size "
                  "limitations.";
            results->errors.push_back(ss.str());

            extraIndexEntrySizeLimitWarning = true;
        }

        std::string indexName = (*entry)["indexName"].String();
        if (!indexNsResultsMap->at(indexName).valid) {
            continue;
        }

        StringBuilder ss;
        ss << "Index with name '" << indexName << "' has inconsistencies.";
        results->errors.push_back(ss.str());

        indexNsResultsMap->at(indexName).valid = false;
    }

    // Inform how many inconsistencies were detected.
    if (numMissingIndexEntryErrors > 0) {
        StringBuilder ss;
        ss << "Detected " << numMissingIndexEntryErrors << " missing index entries.";
        results->warnings.push_back(ss.str());
    }

    if (numExtraIndexEntryErrors > 0) {
        StringBuilder ss;
        ss << "Detected " << numExtraIndexEntryErrors << " extra index entries.";
        results->warnings.push_back(ss.str());
    }

    results->valid = false;
}

void IndexConsistency::addDocKey(const KeyString& ks,
                                 IndexInfo* indexInfo,
                                 RecordId recordId,
                                 const BSONObj& indexKey) {
    const uint32_t hash = _hashKeyString(ks, indexInfo->indexNameHash);

    if (_firstPhase) {
        // During the first phase of validation we only keep track of the count for the document
        // keys encountered.
        _indexKeyCount[hash]++;
        indexInfo->numRecords++;
    } else if (_indexKeyCount[hash]) {
        // Found a document key for a hash bucket that had mismatches.

        // Get the documents _id index key.
        auto cursor = _recordStore->getCursor(_opCtx);
        auto record = cursor->seekExact(recordId);
        invariant(record);

        BSONObj data = record->data.toBson();
        boost::optional<BSONElement> idKey = boost::none;
        if (data.hasField("_id")) {
            idKey = data["_id"];
        }

        BSONObj info = _generateInfo(*indexInfo, recordId, indexKey, idKey);

        // Cannot have duplicate KeyStrings during the document scan phase for the same
        // index.
        IndexKey key = _generateKeyForMap(*indexInfo, ks);
        invariant(_missingIndexEntries.count(key) == 0);
        _missingIndexEntries.insert(std::make_pair(key, info));
    }
}

void IndexConsistency::addIndexKey(const KeyString& ks,
                                   IndexInfo* indexInfo,
                                   RecordId recordId,
                                   const BSONObj& indexKey) {
    const uint32_t hash = _hashKeyString(ks, indexInfo->indexNameHash);

    if (_firstPhase) {
        // During the first phase of validation we only keep track of the count for the index entry
        // keys encountered.
        _indexKeyCount[hash]--;
        indexInfo->numKeys++;
    } else if (_indexKeyCount[hash]) {
        // Found an index key for a bucket that has inconsistencies.
        // If there is a corresponding document key for the index entry key, we remove the key from
        // the '_missingIndexEntries' map. However if there was no document key for the index entry
        // key, we add the key to the '_extraIndexEntries' map.
        IndexKey key = _generateKeyForMap(*indexInfo, ks);
        BSONObj info = _generateInfo(*indexInfo, recordId, indexKey, boost::none);
        if (_missingIndexEntries.count(key) == 0) {
            // We may have multiple extra index entries for a given KeyString.
            auto search = _extraIndexEntries.find(key);
            if (search == _extraIndexEntries.end()) {
                SimpleBSONObjSet infoSet = {info};
                _extraIndexEntries.insert(std::make_pair(key, infoSet));
                return;
            }

            search->second.insert(info);
        } else {
            _missingIndexEntries.erase(key);
        }
    }
}

BSONObj IndexConsistency::_generateInfo(const IndexInfo& indexInfo,
                                        RecordId recordId,
                                        const BSONObj& indexKey,
                                        boost::optional<BSONElement> idKey) {
    const std::string& indexName = indexInfo.descriptor->indexName();
    const BSONObj& keyPattern = indexInfo.descriptor->keyPattern();

    // We need to rehydrate the indexKey for improved readability.
    // {"": ObjectId(...)} -> {"_id": ObjectId(...)}
    auto keysIt = keyPattern.begin();
    auto valuesIt = indexKey.begin();

    BSONObjBuilder b;
    while (keysIt != keyPattern.end()) {
        // keysIt and valuesIt must have the same number of elements.
        invariant(valuesIt != indexKey.end());
        b.appendAs(*valuesIt, keysIt->fieldName());
        keysIt++;
        valuesIt++;
    }

    BSONObj rehydratedKey = b.done();

    if (idKey) {
        return BSON("indexName" << indexName << "recordId" << recordId.repr() << "idKey" << *idKey
                                << "indexKey" << rehydratedKey);
    } else {
        return BSON("indexName" << indexName << "recordId" << recordId.repr() << "indexKey"
                                << rehydratedKey);
    }
}

uint32_t IndexConsistency::_hashKeyString(const KeyString& ks, uint32_t indexNameHash) const {
    MurmurHash3_x86_32(
        ks.getTypeBits().getBuffer(), ks.getTypeBits().getSize(), indexNameHash, &indexNameHash);
    MurmurHash3_x86_32(ks.getBuffer(), ks.getSize(), indexNameHash, &indexNameHash);
    return indexNameHash % kNumHashBuckets;
}
}  // namespace mongo
