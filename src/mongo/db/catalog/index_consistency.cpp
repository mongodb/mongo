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

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/index_consistency.h"

#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_repair.h"
#include "mongo/db/catalog/validate_gen.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/multi_key_path_tracker.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/logv2/log.h"
#include "mongo/util/string_map.h"
#include "mongo/util/testing_proctor.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {

namespace {

const size_t kNumHashBuckets = 1U << 16;

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
                                                       const KeyString::Value& ks) {
    return std::make_pair(indexInfo.indexName, std::string(ks.getBuffer(), ks.getSize()));
}

}  // namespace

IndexInfo::IndexInfo(const IndexDescriptor* descriptor, IndexAccessMethod* indexAccessMethod)
    : indexName(descriptor->indexName()),
      keyPattern(descriptor->keyPattern()),
      indexNameHash(hash(descriptor->indexName())),
      ord(Ordering::make(descriptor->keyPattern())),
      unique(descriptor->unique()),
      accessMethod(indexAccessMethod) {}

IndexEntryInfo::IndexEntryInfo(const IndexInfo& indexInfo,
                               RecordId entryRecordId,
                               BSONObj entryIdKey,
                               KeyString::Value entryKeyString)
    : indexName(indexInfo.indexName),
      keyPattern(indexInfo.keyPattern),
      ord(indexInfo.ord),
      recordId(std::move(entryRecordId)),
      idKey(entryIdKey.getOwned()),
      keyString(entryKeyString) {}

IndexConsistency::IndexConsistency(OperationContext* opCtx,
                                   CollectionValidation::ValidateState* validateState)
    : _validateState(validateState), _firstPhase(true) {
    _indexKeyBuckets.resize(kNumHashBuckets);

    for (const auto& index : _validateState->getIndexes()) {
        const IndexDescriptor* descriptor = index->descriptor();
        IndexAccessMethod* accessMethod = const_cast<IndexAccessMethod*>(index->accessMethod());
        _indexesInfo.emplace(descriptor->indexName(), IndexInfo(descriptor, accessMethod));
    }
}

void IndexConsistency::addMultikeyMetadataPath(const KeyString::Value& ks, IndexInfo* indexInfo) {
    auto hash = _hashKeyString(ks, indexInfo->indexNameHash);
    if (MONGO_unlikely(_validateState->extraLoggingForTest())) {
        LOGV2(6208500,
              "[validate](multikeyMetadataPath) Adding with the hash",
              "hash"_attr = hash,
              "keyString"_attr = ks.toString());
    }
    indexInfo->hashedMultikeyMetadataPaths.emplace(hash);
}

void IndexConsistency::removeMultikeyMetadataPath(const KeyString::Value& ks,
                                                  IndexInfo* indexInfo) {
    auto hash = _hashKeyString(ks, indexInfo->indexNameHash);
    if (MONGO_unlikely(_validateState->extraLoggingForTest())) {
        LOGV2(6208501,
              "[validate](multikeyMetadataPath) Removing with the hash",
              "hash"_attr = hash,
              "keyString"_attr = ks.toString());
    }
    indexInfo->hashedMultikeyMetadataPaths.erase(hash);
}

size_t IndexConsistency::getMultikeyMetadataPathCount(IndexInfo* indexInfo) {
    return indexInfo->hashedMultikeyMetadataPaths.size();
}

bool IndexConsistency::haveEntryMismatch() const {
    return std::any_of(_indexKeyBuckets.begin(),
                       _indexKeyBuckets.end(),
                       [](const IndexKeyBucket& bucket) -> bool { return bucket.indexKeyCount; });
}

void IndexConsistency::setSecondPhase() {
    invariant(_firstPhase);
    _firstPhase = false;
}

void IndexConsistency::repairMissingIndexEntries(OperationContext* opCtx,
                                                 ValidateResults* results) {
    invariant(_validateState->getIndexes().size() > 0);
    std::shared_ptr<const IndexCatalogEntry> index = _validateState->getIndexes().front();
    for (auto it = _missingIndexEntries.begin(); it != _missingIndexEntries.end();) {
        const KeyString::Value& ks = it->second.keyString;
        const KeyFormat keyFormat = _validateState->getCollection()->getRecordStore()->keyFormat();

        const std::string& indexName = it->first.first;
        if (indexName != index->descriptor()->indexName()) {
            // Assuming that _missingIndexEntries is sorted by indexName, this lookup should not
            // happen often.
            for (auto currIndex : _validateState->getIndexes()) {
                if (currIndex->descriptor()->indexName() == indexName) {
                    index = currIndex;
                    break;
                }
            }
        }

        int64_t numInserted = index_repair::repairMissingIndexEntry(opCtx,
                                                                    index,
                                                                    ks,
                                                                    keyFormat,
                                                                    _validateState->nss(),
                                                                    _validateState->getCollection(),
                                                                    results);
        getIndexInfo(indexName).numKeys += numInserted;
        it = _missingIndexEntries.erase(it);
    }

    if (results->numInsertedMissingIndexEntries > 0) {
        results->warnings.push_back(str::stream()
                                    << "Inserted " << results->numInsertedMissingIndexEntries
                                    << " missing index entries.");
    }
    if (results->numDocumentsMovedToLostAndFound > 0) {
        const NamespaceString lostAndFoundNss =
            NamespaceString(NamespaceString::kLocalDb,
                            "lost_and_found." + _validateState->getCollection()->uuid().toString());
        results->warnings.push_back(str::stream()
                                    << "Removed " << results->numDocumentsMovedToLostAndFound
                                    << " duplicate documents to resolve "
                                    << results->numDocumentsMovedToLostAndFound +
                                        results->numOutdatedMissingIndexEntry
                                    << " missing index entries. Removed documents can be found in '"
                                    << lostAndFoundNss.ns() << "'.");
    }
}

void IndexConsistency::addIndexEntryErrors(ValidateResults* results) {
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

    // Inform which indexes have inconsistencies and add the BSON objects of the inconsistent index
    // entries to the results vector.
    bool missingIndexEntrySizeLimitWarning = false;
    for (const auto& missingIndexEntry : _missingIndexEntries) {
        const IndexEntryInfo& entryInfo = missingIndexEntry.second;
        KeyString::Value ks = entryInfo.keyString;
        auto indexKey =
            KeyString::toBsonSafe(ks.getBuffer(), ks.getSize(), entryInfo.ord, ks.getTypeBits());
        const BSONObj entry = _generateInfo(entryInfo.indexName,
                                            entryInfo.keyPattern,
                                            entryInfo.recordId,
                                            indexKey,
                                            entryInfo.idKey);

        numMissingIndexEntriesSizeBytes += entry.objsize();
        if (numMissingIndexEntriesSizeBytes <= kErrorSizeBytes) {
            results->missingIndexEntries.push_back(entry);
        } else if (!missingIndexEntrySizeLimitWarning) {
            StringBuilder ss;
            ss << "Not all missing index entry inconsistencies are listed due to size limitations.";
            results->errors.push_back(ss.str());

            missingIndexEntrySizeLimitWarning = true;
        }

        std::string indexName = entry["indexName"].String();
        if (!results->indexResultsMap.at(indexName).valid) {
            continue;
        }

        StringBuilder ss;
        ss << "Index with name '" << indexName << "' has inconsistencies.";
        results->errors.push_back(ss.str());

        results->indexResultsMap.at(indexName).valid = false;
    }

    bool extraIndexEntrySizeLimitWarning = false;
    for (const auto& extraIndexEntry : _extraIndexEntries) {
        const SimpleBSONObjSet& entries = extraIndexEntry.second;
        for (const auto& entry : entries) {
            numExtraIndexEntriesSizeBytes += entry.objsize();
            if (numExtraIndexEntriesSizeBytes <= kErrorSizeBytes) {
                results->extraIndexEntries.push_back(entry);
            } else if (!extraIndexEntrySizeLimitWarning) {
                StringBuilder ss;
                ss << "Not all extra index entry inconsistencies are listed due to size "
                      "limitations.";
                results->errors.push_back(ss.str());

                extraIndexEntrySizeLimitWarning = true;
            }

            std::string indexName = entry["indexName"].String();
            if (!results->indexResultsMap.at(indexName).valid) {
                continue;
            }

            StringBuilder ss;
            ss << "Index with name '" << indexName << "' has inconsistencies.";
            results->errors.push_back(ss.str());

            results->indexResultsMap.at(indexName).valid = false;
        }
    }

    // Inform how many inconsistencies were detected.
    if (numMissingIndexEntryErrors > 0) {
        StringBuilder ss;
        ss << "Detected " << numMissingIndexEntryErrors << " missing index entries.";
        results->warnings.push_back(ss.str());
        results->valid = false;
    }

    if (numExtraIndexEntryErrors > 0) {
        StringBuilder ss;
        ss << "Detected " << numExtraIndexEntryErrors << " extra index entries.";
        results->warnings.push_back(ss.str());
        results->valid = false;
    }
}

void IndexConsistency::addDocumentMultikeyPaths(IndexInfo* indexInfo,
                                                const MultikeyPaths& newPaths) {
    invariant(newPaths.size());
    if (indexInfo->docMultikeyPaths.size()) {
        MultikeyPathTracker::mergeMultikeyPaths(&indexInfo->docMultikeyPaths, newPaths);
    } else {
        // Instantiate the multikey paths. Also indicates that this index uses multikeyPaths.
        indexInfo->docMultikeyPaths = newPaths;
    }
}

void IndexConsistency::addDocKey(OperationContext* opCtx,
                                 const KeyString::Value& ks,
                                 IndexInfo* indexInfo,
                                 const RecordId& recordId) {
    auto rawHash = ks.hash(indexInfo->indexNameHash);
    auto hashLower = rawHash % kNumHashBuckets;
    auto hashUpper = (rawHash / kNumHashBuckets) % kNumHashBuckets;
    auto& lower = _indexKeyBuckets[hashLower];
    auto& upper = _indexKeyBuckets[hashUpper];

    if (_firstPhase) {
        // During the first phase of validation we only keep track of the count for the document
        // keys encountered.
        lower.indexKeyCount++;
        lower.bucketSizeBytes += ks.getSize();
        upper.indexKeyCount++;
        upper.bucketSizeBytes += ks.getSize();
        indexInfo->numRecords++;

        if (MONGO_unlikely(_validateState->extraLoggingForTest())) {
            LOGV2(4666602,
                  "[validate](record) Adding with hashes",
                  "hashUpper"_attr = hashUpper,
                  "hashLower"_attr = hashLower);
            const BSONObj& keyPatternBson = indexInfo->keyPattern;
            auto keyStringBson = KeyString::toBsonSafe(
                ks.getBuffer(), ks.getSize(), indexInfo->ord, ks.getTypeBits());
            KeyString::logKeyString(
                recordId, ks, keyPatternBson, keyStringBson, "[validate](record)");
        }
    } else if (lower.indexKeyCount || upper.indexKeyCount) {
        // Found a document key for a hash bucket that had mismatches.

        // Get the documents _id index key.
        auto record = _validateState->getSeekRecordStoreCursor()->seekExact(opCtx, recordId);
        invariant(record);

        BSONObj data = record->data.toBson();

        BSONObjBuilder idKeyBuilder;
        if (data.hasField("_id")) {
            idKeyBuilder.append(data["_id"]);
        }

        // Cannot have duplicate KeyStrings during the document scan phase for the same index.
        IndexKey key = _generateKeyForMap(*indexInfo, ks);
        invariant(_missingIndexEntries.count(key) == 0);
        _missingIndexEntries.insert(
            std::make_pair(key, IndexEntryInfo(*indexInfo, recordId, idKeyBuilder.obj(), ks)));

        // Prints the collection document's metadata.
        _validateState->getCollection()->getRecordStore()->printRecordMetadata(opCtx, recordId);
    }
}

void IndexConsistency::addIndexKey(OperationContext* opCtx,
                                   const KeyString::Value& ks,
                                   IndexInfo* indexInfo,
                                   const RecordId& recordId,
                                   ValidateResults* results) {
    auto rawHash = ks.hash(indexInfo->indexNameHash);
    auto hashLower = rawHash % kNumHashBuckets;
    auto hashUpper = (rawHash / kNumHashBuckets) % kNumHashBuckets;
    auto& lower = _indexKeyBuckets[hashLower];
    auto& upper = _indexKeyBuckets[hashUpper];

    if (_firstPhase) {
        // During the first phase of validation we only keep track of the count for the index entry
        // keys encountered.
        lower.indexKeyCount--;
        lower.bucketSizeBytes += ks.getSize();
        upper.indexKeyCount--;
        upper.bucketSizeBytes += ks.getSize();
        indexInfo->numKeys++;

        if (MONGO_unlikely(_validateState->extraLoggingForTest())) {
            LOGV2(4666603,
                  "[validate](index) Adding with hashes",
                  "hashUpper"_attr = hashUpper,
                  "hashLower"_attr = hashLower);
            const BSONObj& keyPatternBson = indexInfo->keyPattern;
            auto keyStringBson = KeyString::toBsonSafe(
                ks.getBuffer(), ks.getSize(), indexInfo->ord, ks.getTypeBits());
            KeyString::logKeyString(
                recordId, ks, keyPatternBson, keyStringBson, "[validate](index)");
        }
    } else if (lower.indexKeyCount || upper.indexKeyCount) {
        // Found an index key for a bucket that has inconsistencies.
        // If there is a corresponding document key for the index entry key, we remove the key from
        // the '_missingIndexEntries' map. However if there was no document key for the index entry
        // key, we add the key to the '_extraIndexEntries' map.
        auto indexKey =
            KeyString::toBsonSafe(ks.getBuffer(), ks.getSize(), indexInfo->ord, ks.getTypeBits());
        BSONObj info = _generateInfo(
            indexInfo->indexName, indexInfo->keyPattern, recordId, indexKey, BSONObj());

        IndexKey key = _generateKeyForMap(*indexInfo, ks);
        if (_missingIndexEntries.count(key) == 0) {
            if (_validateState->fixErrors()) {
                // Removing extra index entries.
                InsertDeleteOptions options;
                options.dupsAllowed = !indexInfo->unique;
                int64_t numDeleted = 0;
                writeConflictRetry(
                    opCtx, "removingExtraIndexEntries", _validateState->nss().ns(), [&] {
                        WriteUnitOfWork wunit(opCtx);
                        Status status = indexInfo->accessMethod->asSortedData()->removeKeys(
                            opCtx, {ks}, options, &numDeleted);
                        wunit.commit();
                    });
                auto& indexResults = results->indexResultsMap[indexInfo->indexName];
                indexResults.keysTraversed -= numDeleted;
                results->numRemovedExtraIndexEntries += numDeleted;
                results->repaired = true;
                indexInfo->numKeys--;
                return;
            }

            // We may have multiple extra index entries for a given KeyString.
            auto search = _extraIndexEntries.find(key);
            if (search == _extraIndexEntries.end()) {
                SimpleBSONObjSet infoSet = {info};
                if (TestingProctor::instance().isEnabled() &&
                    _validateState->nss() == NamespaceString("local", "replset.initialSyncId")) {
                    invariant(false,
                              "Validation failed with an extra index key on "
                              "`local.replset.initialSyncId`.");
                }
                _extraIndexEntries.insert(std::make_pair(key, infoSet));

                // Prints the collection document's metadata.
                _validateState->getCollection()->getRecordStore()->printRecordMetadata(opCtx,
                                                                                       recordId);
                return;
            }
            search->second.insert(info);
        } else {
            _missingIndexEntries.erase(key);
        }
    }
}

bool IndexConsistency::limitMemoryUsageForSecondPhase(ValidateResults* result) {
    invariant(!_firstPhase);

    const uint32_t maxMemoryUsageBytes = maxValidateMemoryUsageMB.load() * 1024 * 1024;
    const uint64_t totalMemoryNeededBytes =
        std::accumulate(_indexKeyBuckets.begin(),
                        _indexKeyBuckets.end(),
                        0,
                        [](uint64_t bytes, const IndexKeyBucket& bucket) {
                            return bucket.indexKeyCount ? bytes + bucket.bucketSizeBytes : bytes;
                        });

    // Allows twice the "maxValidateMemoryUsageMB" because each KeyString has two hashes stored.
    if (totalMemoryNeededBytes <= maxMemoryUsageBytes * 2) {
        // The amount of memory we need is under the limit, so no need to do anything else.
        return true;
    }

    bool hasNonZeroBucket = false;
    uint64_t memoryUsedSoFarBytes = 0;
    uint32_t smallestBucketBytes = std::numeric_limits<uint32_t>::max();
    // Zero out any nonzero buckets that would put us over maxMemoryUsageBytes.
    std::for_each(_indexKeyBuckets.begin(), _indexKeyBuckets.end(), [&](IndexKeyBucket& bucket) {
        if (bucket.indexKeyCount == 0) {
            return;
        }

        smallestBucketBytes = std::min(smallestBucketBytes, bucket.bucketSizeBytes);
        if (bucket.bucketSizeBytes + memoryUsedSoFarBytes > maxMemoryUsageBytes) {
            // Including this bucket would put us over the memory limit, so zero this bucket. We
            // don't want to keep any entry that will exceed the memory limit in the second phase so
            // we don't double the 'maxMemoryUsageBytes' here.
            bucket.indexKeyCount = 0;
            return;
        }
        memoryUsedSoFarBytes += bucket.bucketSizeBytes;
        hasNonZeroBucket = true;
    });

    StringBuilder memoryLimitMessage;
    memoryLimitMessage << "Memory limit for validation is currently set to "
                       << maxValidateMemoryUsageMB.load()
                       << "MB and can be configured via the 'maxValidateMemoryUsageMB' parameter.";

    if (!hasNonZeroBucket) {
        const uint32_t minMemoryNeededMB = (smallestBucketBytes / (1024 * 1024)) + 1;
        StringBuilder ss;
        ss << "Unable to report index entry inconsistencies due to memory limitations. Need at "
              "least "
           << minMemoryNeededMB << "MB to report at least one index entry inconsistency. "
           << memoryLimitMessage.str();
        result->errors.push_back(ss.str());
        result->valid = false;

        return false;
    }

    StringBuilder ss;
    ss << "Not all index entry inconsistencies are reported due to memory limitations. "
       << memoryLimitMessage.str();
    result->errors.push_back(ss.str());
    result->valid = false;

    return true;
}

BSONObj IndexConsistency::_generateInfo(const std::string& indexName,
                                        const BSONObj& keyPattern,
                                        const RecordId& recordId,
                                        const BSONObj& indexKey,
                                        const BSONObj& idKey) {

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

    BSONObjBuilder infoBuilder;
    infoBuilder.append("indexName", indexName);
    recordId.serializeToken("recordId", &infoBuilder);

    if (!idKey.isEmpty()) {
        infoBuilder.append("idKey", idKey);
    }

    infoBuilder.append("indexKey", rehydratedKey);

    return infoBuilder.obj();
}

uint32_t IndexConsistency::_hashKeyString(const KeyString::Value& ks,
                                          uint32_t indexNameHash) const {
    return ks.hash(indexNameHash);
}
}  // namespace mongo
