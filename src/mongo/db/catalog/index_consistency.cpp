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
#include <cstdint>
#include <fmt/format.h>
#include <memory>
#include <mutex>
#include <new>
#include <numeric>
#include <type_traits>

#include <absl/container/node_hash_map.h>
// IWYU pragma: no_include "boost/container/detail/flat_tree.hpp"
#include <boost/container/flat_set.hpp>
#include <boost/container/small_vector.hpp>
#include <boost/container/vector.hpp>
// IWYU pragma: no_include "boost/intrusive/detail/algorithm.hpp"
// IWYU pragma: no_include "boost/intrusive/detail/iterator.hpp"
// IWYU pragma: no_include "boost/move/algo/detail/set_difference.hpp"
#include <boost/move/algo/move.hpp>
// IWYU pragma: no_include "boost/move/detail/iterator_to_raw_pointer.hpp"
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_consistency.h"
#include "mongo/db/catalog/index_repair.h"
#include "mongo/db/catalog/validate_gen.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_names.h"
#include "mongo/db/multi_key_path_tracker.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/storage/execution_context.h"
#include "mongo/db/storage/index_entry_comparison.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/shared_buffer_fragment.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"
#include "mongo/util/testing_proctor.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {

const long long IndexConsistency::kInterruptIntervalNumRecords = 4096;
const size_t IndexConsistency::kNumHashBuckets = 1U << 16;

namespace {

MONGO_FAIL_POINT_DEFINE(crashOnMultikeyValidateFailure);
MONGO_FAIL_POINT_DEFINE(failIndexKeyOrdering);

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
                                                       const key_string::Value& ks) {
    return std::make_pair(indexInfo.indexName, std::string(ks.getBuffer(), ks.getSize()));
}

BSONObj _rehydrateKey(const BSONObj& keyPattern, const BSONObj& indexKey) {
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

    return b.obj();
}


}  // namespace

IndexInfo::IndexInfo(const IndexDescriptor* descriptor)
    : indexName(descriptor->indexName()),
      keyPattern(descriptor->keyPattern()),
      indexNameHash(hash(descriptor->indexName())),
      ord(Ordering::make(descriptor->keyPattern())),
      unique(descriptor->unique()),
      accessMethod(descriptor->getEntry()->accessMethod()) {}

IndexEntryInfo::IndexEntryInfo(const IndexInfo& indexInfo,
                               RecordId entryRecordId,
                               BSONObj entryIdKey,
                               key_string::Value entryKeyString)
    : indexName(indexInfo.indexName),
      keyPattern(indexInfo.keyPattern),
      ord(indexInfo.ord),
      recordId(std::move(entryRecordId)),
      idKey(entryIdKey.getOwned()),
      keyString(entryKeyString) {}

IndexConsistency::IndexConsistency(OperationContext* opCtx,
                                   CollectionValidation::ValidateState* validateState,
                                   const size_t numHashBuckets)
    : _validateState(validateState), _firstPhase(true) {
    _indexKeyBuckets.resize(numHashBuckets);
}

void IndexConsistency::setSecondPhase() {
    invariant(_firstPhase);
    _firstPhase = false;
}

KeyStringIndexConsistency::KeyStringIndexConsistency(
    OperationContext* opCtx,
    CollectionValidation::ValidateState* validateState,
    const size_t numHashBuckets)
    : IndexConsistency(opCtx, validateState, numHashBuckets) {
    for (const auto& indexIdent : _validateState->getIndexIdents()) {
        const IndexDescriptor* descriptor =
            validateState->getCollection()->getIndexCatalog()->findIndexByIdent(opCtx, indexIdent);
        _indexesInfo.emplace(descriptor->indexName(), IndexInfo(descriptor));
    }
}

void KeyStringIndexConsistency::addMultikeyMetadataPath(const key_string::Value& ks,
                                                        IndexInfo* indexInfo) {
    auto hash = _hashKeyString(ks, indexInfo->indexNameHash);
    if (MONGO_unlikely(_validateState->logDiagnostics())) {
        LOGV2(6208500,
              "[validate](multikeyMetadataPath) Adding with the hash",
              "hash"_attr = hash,
              "keyString"_attr = ks.toString());
    }
    indexInfo->hashedMultikeyMetadataPaths.emplace(hash);
}

void KeyStringIndexConsistency::removeMultikeyMetadataPath(const key_string::Value& ks,
                                                           IndexInfo* indexInfo) {
    auto hash = _hashKeyString(ks, indexInfo->indexNameHash);
    if (MONGO_unlikely(_validateState->logDiagnostics())) {
        LOGV2(6208501,
              "[validate](multikeyMetadataPath) Removing with the hash",
              "hash"_attr = hash,
              "keyString"_attr = ks.toString());
    }
    indexInfo->hashedMultikeyMetadataPaths.erase(hash);
}

size_t KeyStringIndexConsistency::getMultikeyMetadataPathCount(IndexInfo* indexInfo) {
    return indexInfo->hashedMultikeyMetadataPaths.size();
}

bool KeyStringIndexConsistency::haveEntryMismatch() const {
    bool haveMismatch =
        std::any_of(_indexKeyBuckets.begin(),
                    _indexKeyBuckets.end(),
                    [](const IndexKeyBucket& bucket) -> bool { return bucket.indexKeyCount; });

    if (haveMismatch && _validateState->logDiagnostics()) {
        for (size_t i = 0; i < _indexKeyBuckets.size(); i++) {
            if (_indexKeyBuckets[i].indexKeyCount == 0) {
                continue;
            }

            LOGV2(7404500,
                  "[validate](bucket entry mismatch)",
                  "hash"_attr = i,
                  "indexKeyCount"_attr = _indexKeyBuckets[i].indexKeyCount,
                  "bucketBytesSize"_attr = _indexKeyBuckets[i].bucketSizeBytes);
        }
    }

    return haveMismatch;
}

void KeyStringIndexConsistency::repairIndexEntries(OperationContext* opCtx,
                                                   ValidateResults* results) {
    invariant(_validateState->getIndexIdents().size() > 0);
    for (auto it = _missingIndexEntries.begin(); it != _missingIndexEntries.end();) {
        const key_string::Value& ks = it->second.keyString;
        const KeyFormat keyFormat = _validateState->getCollection()->getRecordStore()->keyFormat();

        const std::string& indexName = it->first.first;
        const IndexDescriptor* descriptor =
            _validateState->getCollection()->getIndexCatalog()->findIndexByName(opCtx, indexName);
        const IndexCatalogEntry* entry = descriptor->getEntry();
        int64_t numInserted = index_repair::repairMissingIndexEntry(opCtx,
                                                                    entry,
                                                                    ks,
                                                                    keyFormat,
                                                                    _validateState->nss(),
                                                                    _validateState->getCollection(),
                                                                    results);
        getIndexInfo(indexName).numKeys += numInserted;
        it = _missingIndexEntries.erase(it);
    }

    if (results->getNumInsertedMissingIndexEntries() > 0) {
        results->addWarning(str::stream()
                            << "Inserted " << results->getNumInsertedMissingIndexEntries()
                            << " missing index entries.");
    }
    if (results->getNumDocumentsMovedToLostAndFound() > 0) {
        const NamespaceString lostAndFoundNss = NamespaceString::makeLocalCollection(
            "lost_and_found." + _validateState->getCollection()->uuid().toString());
        results->addWarning(str::stream()
                            << "Removed " << results->getNumDocumentsMovedToLostAndFound()
                            << " duplicate documents to resolve "
                            << results->getNumDocumentsMovedToLostAndFound() +
                                results->getNumOutdatedMissingIndexEntry()
                            << " missing index entries. Removed documents can be found in '"
                            << lostAndFoundNss.toStringForErrorMsg() << "'.");
    }
}

void KeyStringIndexConsistency::addIndexEntryErrors(OperationContext* opCtx,
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
                  return a->second.keyString.getSize() < b->second.keyString.getSize();
              });

    // Inform which indexes have inconsistencies and add the BSON objects of the inconsistent index
    // entries to the results vector.
    bool missingIndexEntrySizeLimitWarning = false;
    bool first = true;
    for (const auto& missingIndexEntry : missingIndexEntriesBySize) {
        const IndexEntryInfo& entryInfo = missingIndexEntry->second;
        key_string::Value ks = entryInfo.keyString;
        auto indexKey =
            key_string::toBsonSafe(ks.getBuffer(), ks.getSize(), entryInfo.ord, ks.getTypeBits());
        const BSONObj entry = _generateInfo(entryInfo.indexName,
                                            entryInfo.keyPattern,
                                            entryInfo.recordId,
                                            indexKey,
                                            entryInfo.idKey);

        numMissingIndexEntriesSizeBytes += entry.objsize();
        if (first || numMissingIndexEntriesSizeBytes <= kErrorSizeBytes) {
            results->addMissingIndexEntry(entry);
            first = false;
        } else if (!missingIndexEntrySizeLimitWarning) {
            StringBuilder ss;
            ss << "Not all missing index entry inconsistencies are listed due to size limitations.";
            results->addError(ss.str(), false);

            missingIndexEntrySizeLimitWarning = true;
        }

        _printMetadata(opCtx, results, entryInfo);

        std::string indexName = entry["indexName"].String();
        if (!results->getIndexResultsMap().at(indexName).isValid()) {
            continue;
        }

        StringBuilder ss;
        ss << "Index with name '" << indexName << "' has inconsistencies.";
        results->getIndexResultsMap().at(indexName).addError(ss.str());
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
            results->addExtraIndexEntry(*entry);
            first = false;
        } else if (!extraIndexEntrySizeLimitWarning) {
            StringBuilder ss;
            ss << "Not all extra index entry inconsistencies are listed due to size "
                  "limitations.";
            results->addError(ss.str(), false);

            extraIndexEntrySizeLimitWarning = true;
        }

        std::string indexName = (*entry)["indexName"].String();
        if (!results->getIndexResultsMap().at(indexName).isValid()) {
            continue;
        }

        StringBuilder ss;
        ss << "Index with name '" << indexName << "' has inconsistencies.";
        results->getIndexResultsMap().at(indexName).addError(ss.str());
    }

    // Inform how many inconsistencies were detected.
    if (numMissingIndexEntryErrors > 0) {
        StringBuilder ss;
        ss << "Detected " << numMissingIndexEntryErrors << " missing index entries.";
        results->addWarning(ss.str());
    }

    if (numExtraIndexEntryErrors > 0) {
        StringBuilder ss;
        ss << "Detected " << numExtraIndexEntryErrors << " extra index entries.";
        results->addWarning(ss.str());
    }
}

void KeyStringIndexConsistency::addDocumentMultikeyPaths(IndexInfo* indexInfo,
                                                         const MultikeyPaths& newPaths) {
    invariant(newPaths.size());
    if (indexInfo->docMultikeyPaths.size()) {
        MultikeyPathTracker::mergeMultikeyPaths(&indexInfo->docMultikeyPaths, newPaths);
    } else {
        // Instantiate the multikey paths. Also indicates that this index uses multikeyPaths.
        indexInfo->docMultikeyPaths = newPaths;
    }
}

void KeyStringIndexConsistency::addDocKey(OperationContext* opCtx,
                                          const key_string::Value& ks,
                                          IndexInfo* indexInfo,
                                          const RecordId& recordId,
                                          ValidateResults* results) {
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

        if (MONGO_unlikely(_validateState->logDiagnostics())) {
            LOGV2(4666602,
                  "[validate](record) Adding with hashes",
                  "hashUpper"_attr = hashUpper,
                  "hashLower"_attr = hashLower);
            const BSONObj& keyPatternBson = indexInfo->keyPattern;
            auto keyStringBson = key_string::toBsonSafe(
                ks.getBuffer(), ks.getSize(), indexInfo->ord, ks.getTypeBits());
            key_string::logKeyString(
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
    }
}

void KeyStringIndexConsistency::addIndexKey(OperationContext* opCtx,
                                            const IndexCatalogEntry* entry,
                                            const key_string::Value& ks,
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

        if (MONGO_unlikely(_validateState->logDiagnostics())) {
            LOGV2(4666603,
                  "[validate](index) Adding with hashes",
                  "hashUpper"_attr = hashUpper,
                  "hashLower"_attr = hashLower);
            const BSONObj& keyPatternBson = indexInfo->keyPattern;
            auto keyStringBson = key_string::toBsonSafe(
                ks.getBuffer(), ks.getSize(), indexInfo->ord, ks.getTypeBits());
            key_string::logKeyString(
                recordId, ks, keyPatternBson, keyStringBson, "[validate](index)");
        }
    } else if (lower.indexKeyCount || upper.indexKeyCount) {
        // Found an index key for a bucket that has inconsistencies.
        // If there is a corresponding document key for the index entry key, we remove the key from
        // the '_missingIndexEntries' map. However if there was no document key for the index entry
        // key, we add the key to the '_extraIndexEntries' map.
        auto indexKey =
            key_string::toBsonSafe(ks.getBuffer(), ks.getSize(), indexInfo->ord, ks.getTypeBits());
        BSONObj info = _generateInfo(
            indexInfo->indexName, indexInfo->keyPattern, recordId, indexKey, BSONObj());

        IndexKey key = _generateKeyForMap(*indexInfo, ks);
        if (_missingIndexEntries.count(key) == 0) {
            if (_validateState->fixErrors()) {
                // Removing extra index entries.
                InsertDeleteOptions options;
                options.dupsAllowed = !indexInfo->unique;
                int64_t numDeleted = 0;
                writeConflictRetry(opCtx, "removingExtraIndexEntries", _validateState->nss(), [&] {
                    WriteUnitOfWork wunit(opCtx);
                    Status status = indexInfo->accessMethod->asSortedData()->removeKeys(
                        opCtx, entry, {ks}, options, &numDeleted);
                    wunit.commit();
                });
                auto& indexResults = results->getIndexValidateResult(indexInfo->indexName);
                indexResults.addKeysTraversed(-numDeleted);
                results->addNumRemovedExtraIndexEntries(numDeleted);
                results->setRepaired(true);
                indexInfo->numKeys--;
                _extraIndexEntries.erase(key);
                return;
            }

            // We may have multiple extra index entries for a given KeyString.
            auto search = _extraIndexEntries.find(key);
            if (search == _extraIndexEntries.end()) {
                SimpleBSONObjSet infoSet = {info};
                _extraIndexEntries.insert(std::make_pair(key, infoSet));

                // Prints the collection document's and index entry's metadata.
                _validateState->getCollection()->getRecordStore()->printRecordMetadata(
                    opCtx, recordId, results->getRecordTimestampsPtr());
                indexInfo->accessMethod->asSortedData()
                    ->getSortedDataInterface()
                    ->printIndexEntryMetadata(opCtx, ks);
                return;
            }
            search->second.insert(info);
        } else {
            _missingIndexEntries.erase(key);
        }
    }
}

bool KeyStringIndexConsistency::limitMemoryUsageForSecondPhase(ValidateResults* result) {
    invariant(!_firstPhase);

    const uint64_t maxMemoryUsageBytes =
        static_cast<uint64_t>(maxValidateMemoryUsageMB.load()) * 1024 * 1024;
    const uint64_t totalMemoryNeededBytes =
        std::accumulate(_indexKeyBuckets.begin(),
                        _indexKeyBuckets.end(),
                        0ull,
                        [](uint64_t bytes, const IndexKeyBucket& bucket) {
                            return bucket.indexKeyCount ? bytes + bucket.bucketSizeBytes : bytes;
                        });

    // Allows twice the "maxValidateMemoryUsageMB" because each KeyString has two hashes stored.
    if (totalMemoryNeededBytes <= maxMemoryUsageBytes * 2) {
        // The amount of memory we need is under the limit, so no need to do anything else.
        return true;
    }

    // At this point we know we'll exceed the memory limit, and will pare back some of the buckets.
    // First we'll see what the smallest bucket is, and if that's over the limit by itself, then
    // we can zero out all the other buckets. Otherwise we'll keep as many buckets as we can.

    auto smallestBucketWithAnInconsistency = std::min_element(
        _indexKeyBuckets.begin(),
        _indexKeyBuckets.end(),
        [](const IndexKeyBucket& lhs, const IndexKeyBucket& rhs) {
            if (lhs.indexKeyCount != 0) {
                return rhs.indexKeyCount == 0 || lhs.bucketSizeBytes < rhs.bucketSizeBytes;
            }
            return false;
        });
    invariant(smallestBucketWithAnInconsistency->indexKeyCount != 0);

    if (smallestBucketWithAnInconsistency->bucketSizeBytes > maxMemoryUsageBytes) {
        // We're going to just keep the smallest bucket, and zero everything else.
        std::for_each(
            _indexKeyBuckets.begin(), _indexKeyBuckets.end(), [&](IndexKeyBucket& bucket) {
                if (&bucket == &(*smallestBucketWithAnInconsistency)) {
                    // We keep the smallest bucket.
                    return;
                }

                bucket.indexKeyCount = 0;
            });
    } else {
        // We're going to scan through the buckets and keep as many as we can.
        std::uint32_t memoryUsedSoFarBytes = 0;
        std::for_each(
            _indexKeyBuckets.begin(), _indexKeyBuckets.end(), [&](IndexKeyBucket& bucket) {
                if (bucket.indexKeyCount == 0) {
                    return;
                }

                if (bucket.bucketSizeBytes + memoryUsedSoFarBytes > maxMemoryUsageBytes) {
                    // Including this bucket would put us over the memory limit, so zero this
                    // bucket. We don't want to keep any entry that will exceed the memory limit in
                    // the second phase so we don't double the 'maxMemoryUsageBytes' here.
                    bucket.indexKeyCount = 0;
                    return;
                }
                memoryUsedSoFarBytes += bucket.bucketSizeBytes;
            });
    }

    StringBuilder ss;
    ss << "Not all index entry inconsistencies are reported due to memory limitations. Memory "
          "limit for validation is currently set to "
       << maxValidateMemoryUsageMB.load()
       << "MB and can be configured via the 'maxValidateMemoryUsageMB' parameter.";
    result->addError(ss.str());

    LOGV2(8943500,
          "Not all index entry inconsistencies are reported due to memory limitations; the memory "
          "limit for validation can be configured via the 'maxValidateMemoryUsageMB' server "
          "parameter",
          "maxValidateMemoryUsageMB"_attr = maxValidateMemoryUsageMB.load(),
          "totalMemoryNeededBytes"_attr = totalMemoryNeededBytes);

    return true;
}

void KeyStringIndexConsistency::validateIndexKeyCount(OperationContext* opCtx,
                                                      const IndexCatalogEntry* index,
                                                      long long* numRecords,
                                                      IndexValidateResults& results) {
    // Fetch the total number of index entries we previously found traversing the index.
    const IndexDescriptor* desc = index->descriptor();
    const std::string indexName = desc->indexName();
    IndexInfo* indexInfo = &this->getIndexInfo(indexName);
    const auto numTotalKeys = indexInfo->numKeys;

    // Update numRecords by subtracting number of records removed from record store in repair mode
    // when validating index consistency
    (*numRecords) -= results.getKeysRemovedFromRecordStore();

    if (desc->isIdIndex() && numTotalKeys != (*numRecords)) {
        const std::string msg = str::stream()
            << "number of _id index entries (" << numTotalKeys
            << ") does not match the number of documents in the index (" << (*numRecords) << ")";
        results.addError(msg);
    }

    // Hashed indexes may never be multikey.
    if (desc->getAccessMethodName() == IndexNames::HASHED &&
        index->isMultikey(opCtx, _validateState->getCollection())) {
        results.addError(str::stream()
                         << "Hashed index is incorrectly marked multikey: " << desc->indexName());
    }

    // Confirm that the number of index entries is not greater than the number of documents in the
    // collection. This check is only valid for indexes that are not multikey (indexed arrays
    // produce an index key per array entry) and not $** indexes which can produce index keys for
    // multiple paths within a single document.
    if (results.isValid() && !index->isMultikey(opCtx, _validateState->getCollection()) &&
        desc->getIndexType() != IndexType::INDEX_WILDCARD && numTotalKeys > (*numRecords)) {
        const std::string err = str::stream()
            << "index " << desc->indexName() << " is not multi-key, but has more entries ("
            << numTotalKeys << ") than documents in the index (" << (*numRecords) << ")";
        results.addError(err);
    }

    // Ignore any indexes with a special access method. If an access method name is given, the
    // index may be a full text, geo or special index plugin with different semantics.
    if (results.isValid() && !desc->isSparse() && !desc->isPartial() && !desc->isIdIndex() &&
        desc->getAccessMethodName() == "" && numTotalKeys < (*numRecords)) {
        const std::string msg = str::stream()
            << "index " << desc->indexName() << " is not sparse or partial, but has fewer entries ("
            << numTotalKeys << ") than documents in the index (" << (*numRecords) << ")";
        results.addError(msg);
    }
}

namespace {
// Ensures that index entries are in increasing or decreasing order.
void _validateKeyOrder(OperationContext* opCtx,
                       const IndexCatalogEntry* index,
                       const boost::optional<KeyStringEntry>& currKey,
                       const boost::optional<KeyStringEntry>& prevKey,
                       IndexValidateResults* results) {
    invariant(currKey);
    invariant(prevKey);
    const auto descriptor = index->descriptor();
    const bool unique = descriptor->unique();

    // KeyStrings will be in strictly increasing order because all keys are sorted and they are in
    // the format (Key, RID), and all RecordIDs are unique.
    if (currKey->keyString.compare(prevKey->keyString) <= 0 ||
        MONGO_unlikely(failIndexKeyOrdering.shouldFail())) {
        if (results && results->isValid()) {
            results->addError(str::stream()
                              << "index '" << descriptor->indexName()
                              << "' is not in strictly ascending or descending order");
        }
        return;
    }

    if (unique) {
        // Unique indexes must not have duplicate keys.
        const int cmp = currKey->loc.isLong()
            ? currKey->keyString.compareWithoutRecordIdLong(prevKey->keyString)
            : currKey->keyString.compareWithoutRecordIdStr(prevKey->keyString);
        if (cmp != 0) {
            return;
        }

        if (results && results->isValid()) {
            const auto bsonKey =
                key_string::toBson(currKey->keyString, Ordering::make(descriptor->keyPattern()));
            results->addError(str::stream() << "Unique index '" << descriptor->indexName()
                                            << "' has duplicate key: " << bsonKey
                                            << ", first record: " << prevKey->loc
                                            << ", second record: " << currKey->loc);
        }
    }
}
}  // namespace

int64_t KeyStringIndexConsistency::traverseIndex(OperationContext* opCtx,
                                                 const IndexCatalogEntry* index,
                                                 ProgressMeterHolder& _progress,
                                                 ValidateResults* results) {
    const IndexDescriptor* descriptor = index->descriptor();
    const auto indexName = descriptor->indexName();
    auto& indexResults = results->getIndexValidateResult(indexName);
    IndexInfo& indexInfo = this->getIndexInfo(indexName);
    int64_t numKeys = 0;

    const key_string::Version version =
        index->accessMethod()->asSortedData()->getSortedDataInterface()->getKeyStringVersion();

    key_string::Builder firstKeyStringBuilder(
        version, BSONObj(), indexInfo.ord, key_string::Discriminator::kExclusiveBefore);
    StringData firstKeyString = firstKeyStringBuilder.finishAndGetBuffer();
    boost::optional<KeyStringEntry> prevIndexKeyStringEntry;

    // Ensure that this index has an open index cursor.
    const auto indexCursorIt = _validateState->getIndexCursors().find(indexName);
    invariant(indexCursorIt != _validateState->getIndexCursors().end());

    const std::unique_ptr<SortedDataInterfaceThrottleCursor>& indexCursor = indexCursorIt->second;

    boost::optional<KeyStringEntry> indexEntry;
    try {
        indexEntry = indexCursor->seekForKeyString(opCtx, firstKeyString);
    } catch (const DBException& ex) {
        if (TestingProctor::instance().isEnabled() && ex.code() != ErrorCodes::WriteConflict) {
            const key_string::Value firstKeyString = firstKeyStringBuilder.getValueCopy();
            LOGV2_FATAL(5318400,
                        "Error seeking to first key",
                        "error"_attr = ex.toString(),
                        "index"_attr = indexName,
                        "key"_attr = firstKeyString.toString());
        }
        throw;
    }

    const auto keyFormat =
        index->accessMethod()->asSortedData()->getSortedDataInterface()->rsKeyFormat();
    const RecordId kWildcardMultikeyMetadataRecordId = record_id_helpers::reservedIdFor(
        record_id_helpers::ReservationId::kWildcardMultikeyMetadataId, keyFormat);

    // Warn about unique indexes with keys in old format (without record id).
    bool foundOldUniqueIndexKeys = false;

    while (indexEntry) {
        if (prevIndexKeyStringEntry) {
            _validateKeyOrder(opCtx, index, indexEntry, prevIndexKeyStringEntry, &indexResults);
        }

        if (!foundOldUniqueIndexKeys && !descriptor->isIdIndex() && descriptor->unique() &&
            !indexCursor->isRecordIdAtEndOfKeyString()) {
            results->addWarning(
                fmt::format("Unique index {} has one or more keys in the old format (without "
                            "embedded record id). First record: {}",
                            indexInfo.indexName,
                            indexEntry->loc.toString()));
            foundOldUniqueIndexKeys = true;
        }

        const bool isMetadataKey = indexEntry->loc == kWildcardMultikeyMetadataRecordId;
        if (descriptor->getIndexType() == IndexType::INDEX_WILDCARD && isMetadataKey) {
            this->removeMultikeyMetadataPath(indexEntry->keyString, &indexInfo);
        } else {
            try {
                this->addIndexKey(
                    opCtx, index, indexEntry->keyString, &indexInfo, indexEntry->loc, results);
            } catch (const DBException& e) {
                StringBuilder ss;
                ss << "Parsing index key for " << indexInfo.indexName << " recId "
                   << indexEntry->loc << " threw exception " << e.toString();
                results->addError(ss.str());
            }
        }
        {
            stdx::unique_lock<Client> lk(*opCtx->getClient());
            _progress.get(lk)->hit();
        }
        numKeys++;
        prevIndexKeyStringEntry = indexEntry;

        if (numKeys % kInterruptIntervalNumRecords == 0) {
            // Periodically checks for interrupts and yields.
            opCtx->checkForInterrupt();
            _validateState->yieldCursors(opCtx);
        }

        try {
            indexEntry = indexCursor->nextKeyString(opCtx);
        } catch (const DBException& ex) {
            if (TestingProctor::instance().isEnabled() && ex.code() != ErrorCodes::WriteConflict) {
                LOGV2_FATAL(5318401,
                            "Error advancing index cursor",
                            "error"_attr = ex.toString(),
                            "index"_attr = indexName,
                            "prevKey"_attr = prevIndexKeyStringEntry->keyString.toString());
            }
            throw;
        }
    }

    if (results && this->getMultikeyMetadataPathCount(&indexInfo) > 0) {
        results->addError(str::stream()
                          << "Index '" << descriptor->indexName()
                          << "' has one or more missing multikey metadata index keys");
    }

    // Adjust multikey metadata when allowed. These states are all allowed by the design of
    // multikey. A collection should still be valid without these adjustments.
    if (_validateState->adjustMultikey()) {

        // If this collection has documents that make this index multikey, then check whether those
        // multikey paths match the index's metadata.
        const auto indexPaths = index->getMultikeyPaths(opCtx, _validateState->getCollection());
        const auto& documentPaths = indexInfo.docMultikeyPaths;
        if (indexInfo.multikeyDocs && documentPaths != indexPaths) {
            LOGV2(5367500,
                  "Index's multikey paths do not match those of its documents",
                  "index"_attr = descriptor->indexName(),
                  "indexPaths"_attr = MultikeyPathTracker::dumpMultikeyPaths(indexPaths),
                  "documentPaths"_attr = MultikeyPathTracker::dumpMultikeyPaths(documentPaths));

            // Since we have the correct multikey path information for this index, we can tighten up
            // its metadata to improve query performance. This may apply in two distinct scenarios:
            // 1. Collection data has changed such that the current multikey paths on the index
            // are too permissive and certain document paths are no longer multikey.
            // 2. This index was built before 3.4, and there is no multikey path information for
            // the index. We can effectively 'upgrade' the index so that it does not need to be
            // rebuilt to update this information.
            writeConflictRetry(opCtx, "updateMultikeyPaths", _validateState->nss(), [&]() {
                WriteUnitOfWork wuow(opCtx);
                auto writeableIndex = const_cast<IndexCatalogEntry*>(index);
                const bool isMultikey = true;
                writeableIndex->forceSetMultikey(
                    opCtx, _validateState->getCollection(), isMultikey, documentPaths);
                wuow.commit();
            });

            if (results) {
                results->addWarning(str::stream() << "Updated index multikey metadata"
                                                  << ": " << descriptor->indexName());
                results->setRepaired(true);
            }
        }

        // If this index does not need to be multikey, then unset the flag.
        if (index->isMultikey(opCtx, _validateState->getCollection()) && !indexInfo.multikeyDocs) {
            invariant(!indexInfo.docMultikeyPaths.size());

            LOGV2(5367501,
                  "Index is multikey but there are no multikey documents",
                  "index"_attr = descriptor->indexName());

            // This makes an improvement in the case that no documents make the index multikey and
            // the flag can be unset entirely. This may be due to a change in the data or historical
            // multikey bugs that have persisted incorrect multikey infomation.
            writeConflictRetry(opCtx, "unsetMultikeyPaths", _validateState->nss(), [&]() {
                WriteUnitOfWork wuow(opCtx);
                auto writeableIndex = const_cast<IndexCatalogEntry*>(index);
                const bool isMultikey = false;
                writeableIndex->forceSetMultikey(
                    opCtx, _validateState->getCollection(), isMultikey, {});
                wuow.commit();
            });

            if (results) {
                results->addWarning(str::stream() << "Unset index multikey metadata"
                                                  << ": " << descriptor->indexName());
                results->setRepaired(true);
            }
        }
    }

    return numKeys;
}

void KeyStringIndexConsistency::traverseRecord(OperationContext* opCtx,
                                               const CollectionPtr& coll,
                                               const IndexCatalogEntry* index,
                                               const RecordId& recordId,
                                               const BSONObj& recordBson,
                                               ValidateResults* results) {
    const auto iam = index->accessMethod()->asSortedData();

    const auto descriptor = index->descriptor();
    SharedBufferFragmentBuilder pool(key_string::HeapBuilder::kHeapAllocatorDefaultBytes);
    auto& executionCtx = StorageExecutionContext::get(opCtx);

    const auto documentKeySet = executionCtx.keys();
    const auto multikeyMetadataKeys = executionCtx.multikeyMetadataKeys();
    const auto documentMultikeyPaths = executionCtx.multikeyPaths();

    try {
        iam->getKeys(opCtx,
                     coll,
                     index,
                     pool,
                     recordBson,
                     InsertDeleteOptions::ConstraintEnforcementMode::kEnforceConstraints,
                     SortedDataIndexAccessMethod::GetKeysContext::kAddingKeys,
                     documentKeySet.get(),
                     multikeyMetadataKeys.get(),
                     documentMultikeyPaths.get(),
                     recordId);
    } catch (const AssertionException& ex) {
        if (ex.toStatus() == ErrorCodes::CannotBuildIndexKeys) {
            LOGV2(8411400,
                  "Could not build index key ",
                  "indexName"_attr = descriptor->indexName(),
                  "recordId"_attr = recordId,
                  "record"_attr = redact(recordBson));
            results->addError(str::stream()
                                  << "Could not build key for index '" << descriptor->indexName()
                                  << "' from document with recordId '" << recordId << "'",
                              false);
            return;
        }
    }

    const bool shouldBeMultikey =
        iam->shouldMarkIndexAsMultikey(documentKeySet->size(),
                                       {multikeyMetadataKeys->begin(), multikeyMetadataKeys->end()},
                                       *documentMultikeyPaths);

    auto printMultikeyMetadata = [&]() {
        LOGV2(7556100,
              "Index is not multikey but document has multikey data",
              "indexName"_attr = descriptor->indexName(),
              "recordId"_attr = recordId,
              "record"_attr = redact(recordBson));
        for (auto& key : *documentKeySet) {
            auto indexKey = key_string::toBsonSafe(key.getBuffer(),
                                                   key.getSize(),
                                                   iam->getSortedDataInterface()->getOrdering(),
                                                   key.getTypeBits());
            const BSONObj rehydratedKey = _rehydrateKey(descriptor->keyPattern(), indexKey);
            LOGV2(7556101,
                  "Index key for document with multikey inconsistency",
                  "indexName"_attr = descriptor->indexName(),
                  "recordId"_attr = recordId,
                  "indexKey"_attr = redact(rehydratedKey));
        }
    };

    if (!index->isMultikey(opCtx, coll) && shouldBeMultikey) {
        if (_validateState->fixErrors()) {
            writeConflictRetry(opCtx, "setIndexAsMultikey", coll->ns(), [&] {
                WriteUnitOfWork wuow(opCtx);
                coll->getIndexCatalog()->setMultikeyPaths(
                    opCtx, coll, descriptor, *multikeyMetadataKeys, *documentMultikeyPaths);
                wuow.commit();
            });

            LOGV2(4614700,
                  "Index set to multikey",
                  "indexName"_attr = descriptor->indexName(),
                  "collection"_attr = coll->ns());
            results->addWarning(str::stream()
                                << "Index " << descriptor->indexName() << " set to multikey.");
            results->setRepaired(true);
        } else {
            printMultikeyMetadata();

            auto& curRecordResults = results->getIndexValidateResult(descriptor->indexName());
            const std::string msg = fmt::format(
                "Index {} is not multikey but document with RecordId({}) and {} has multikey data, "
                "{} key(s)",
                descriptor->indexName(),
                recordId.toString(),
                recordBson.getField("_id").toString(),
                documentKeySet->size());
            curRecordResults.addError(msg, false);
            if (crashOnMultikeyValidateFailure.shouldFail()) {
                invariant(false, msg);
            }
        }
    }

    if (index->isMultikey(opCtx, coll)) {
        const MultikeyPaths& indexPaths = index->getMultikeyPaths(opCtx, coll);
        if (!MultikeyPathTracker::covers(indexPaths, *documentMultikeyPaths.get())) {
            if (_validateState->fixErrors()) {
                writeConflictRetry(opCtx, "increaseMultikeyPathCoverage", coll->ns(), [&] {
                    WriteUnitOfWork wuow(opCtx);
                    coll->getIndexCatalog()->setMultikeyPaths(
                        opCtx, coll, descriptor, *multikeyMetadataKeys, *documentMultikeyPaths);
                    wuow.commit();
                });

                LOGV2(4614701,
                      "Multikey paths updated to cover multikey document",
                      "indexName"_attr = descriptor->indexName(),
                      "collection"_attr = coll->ns());
                results->addWarning(str::stream() << "Index " << descriptor->indexName()
                                                  << " multikey paths updated.");
                results->setRepaired(true);
            } else {
                printMultikeyMetadata();

                const std::string msg = fmt::format(
                    "Index {} multikey paths do not cover a document with RecordId({}) and {}. "
                    "Rerun with {{fixMultikey: true}} if not already set. ",
                    descriptor->indexName(),
                    recordId.toString(),
                    recordBson.getField("_id").toString());
                auto& curRecordResults = results->getIndexValidateResult(descriptor->indexName());
                curRecordResults.addError(msg, false);
            }
        }
    }

    IndexInfo& indexInfo = this->getIndexInfo(descriptor->indexName());
    if (shouldBeMultikey) {
        indexInfo.multikeyDocs = true;
    }

    // An empty set of multikey paths indicates that this index does not track path-level
    // multikey information and we should do no tracking.
    if (shouldBeMultikey && documentMultikeyPaths->size()) {
        this->addDocumentMultikeyPaths(&indexInfo, *documentMultikeyPaths);
    }

    for (const auto& keyString : *multikeyMetadataKeys) {
        this->addMultikeyMetadataPath(keyString, &indexInfo);
    }

    for (const auto& keyString : *documentKeySet) {
        _totalIndexKeys++;
        this->addDocKey(opCtx, keyString, &indexInfo, recordId, results);
    }
}

BSONObj KeyStringIndexConsistency::_generateInfo(const std::string& indexName,
                                                 const BSONObj& keyPattern,
                                                 const RecordId& recordId,
                                                 const BSONObj& indexKey,
                                                 const BSONObj& idKey) {
    BSONObj rehydratedKey = _rehydrateKey(keyPattern, indexKey);

    BSONObjBuilder infoBuilder;
    infoBuilder.append("indexName", indexName);
    recordId.serializeToken("recordId", &infoBuilder);

    if (!idKey.isEmpty()) {
        infoBuilder.append("idKey", idKey);
    }

    infoBuilder.append("indexKey", rehydratedKey);

    return infoBuilder.obj();
}

uint32_t KeyStringIndexConsistency::_hashKeyString(const key_string::Value& ks,
                                                   const uint32_t indexNameHash) const {
    return ks.hash(indexNameHash);
}

void KeyStringIndexConsistency::_printMetadata(OperationContext* opCtx,
                                               ValidateResults* results,
                                               const IndexEntryInfo& entryInfo) {
    _validateState->getCollection()->getRecordStore()->printRecordMetadata(
        opCtx, entryInfo.recordId, results->getRecordTimestampsPtr());
    getIndexInfo(entryInfo.indexName)
        .accessMethod->asSortedData()
        ->getSortedDataInterface()
        ->printIndexEntryMetadata(opCtx, entryInfo.keyString);
}

}  // namespace mongo
