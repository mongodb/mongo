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

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/ordering.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/index_catalog_entry.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/local_catalog/throttle_cursor.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/db/validate/validate_results.h"
#include "mongo/db/validate/validate_state.h"
#include "mongo/util/progress_meter.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace mongo {

class IndexDescriptor;

/**
 * Contains all the index information and stats throughout the validation.
 */
struct IndexInfo {
    IndexInfo(const IndexDescriptor& descriptor);
    // Index name.
    const std::string indexName;
    // Contains the indexes key pattern.
    const BSONObj keyPattern;
    // Contains the pre-computed hash of the index name.
    const uint32_t indexNameHash;
    // More efficient representation of the ordering of the descriptor's key pattern.
    const Ordering ord;
    // The number of index entries belonging to the index.
    int64_t numKeys = 0;
    // The number of records that have a key in their document that referenced back to the this
    // index.
    int64_t numRecords = 0;
    // A hashed set of indexed multikey paths (applies to $** indexes only).
    std::set<uint32_t> hashedMultikeyMetadataPaths;
    // Indicates whether or not there are documents that make this index multikey.
    bool multikeyDocs = false;
    // The set of multikey paths generated from all documents. Only valid when multikeyDocs is also
    // set and an index tracks path-level information.
    MultikeyPaths docMultikeyPaths;
    // Indicates whether key entries must be unique.
    const bool unique;
    // Index access method pointer.
    const IndexAccessMethod* accessMethod;
    IndexType indexType;
};

/**
 * The IndexConsistency class provides the base class definitions for index-consistency
 * sub-classes. The base implementation in this class provides the basis for keeping track of the
 * index consistency. It does this by using the index keys from index entries and index keys
 * generated from the document to ensure there is a one-to-one mapping for each key.
 */
class IndexConsistency {
public:
    static const long long kInterruptIntervalNumRecords;
    static const size_t kNumHashBuckets;

    IndexConsistency(OperationContext* opCtx,
                     CollectionValidation::ValidateState* validateState,
                     size_t numHashBuckets = kNumHashBuckets);

    /**
     * Informs the IndexConsistency object that we're advancing to the second phase of
     * index validation.
     */
    void setSecondPhase();

    virtual ~IndexConsistency() = default;

protected:
    struct IndexKeyBucket {
        uint32_t indexKeyCount = 0;
        uint32_t bucketSizeBytes = 0;
    };

    CollectionValidation::ValidateState* _validateState;

    // We map the hashed KeyString values to a bucket that contains the count of how many
    // index keys and document keys we've seen in each bucket. This counter is unsigned to avoid
    // undefined behavior in the (unlikely) case of overflow.
    // Count rules:
    //     - If the count is non-zero for a bucket after all documents and index entries have been
    //       processed, one or more indexes are inconsistent for KeyStrings that map to it.
    //       Otherwise, those keys are consistent for all indexes with a high degree of confidence.
    //     - Absent overflow, if a count interpreted as twos complement integer ends up greater
    //       than zero, there are too few index entries.
    //     - Similarly, if that count ends up less than zero, there are too many index entries.

    std::vector<IndexKeyBucket> _indexKeyBuckets;

    // Whether we're in the first or second phase of index validation.
    bool _firstPhase;

private:
    IndexConsistency() = delete;
};  // IndexConsistency

/**
 * The KeyStringIndexConsistency class is used to keep track of the index consistency for
 * KeyString based indexes. It does this by using the index keys from index entries and index keys
 * generated from the document to ensure there is a one-to-one mapping for each key. In addition, an
 * IndexObserver class can be hooked into the IndexAccessMethod to inform this class about changes
 * to the indexes during a validation and compensate for them.
 */
class KeyStringIndexConsistency final : protected IndexConsistency {
    struct AlphabeticalByIndexNameComparator;
    using IndexInfoMap = std::map<std::string, IndexInfo>;
    using IndexKey = std::pair<IndexInfo*, key_string::Value>;
    // TODO(SERVER-62257): Drop the comparator, unfortunately we can't do that for now because there
    // are *quite problematic* dependencies between the order that we repair inconsistencies and the
    // order we determine discrepencies in the count.
    template <typename T>
    using IndexInconsistencyMap = std::map<IndexKey, T, AlphabeticalByIndexNameComparator>;

public:
    KeyStringIndexConsistency(OperationContext* opCtx,
                              CollectionValidation::ValidateState* validateState,
                              size_t numHashBuckets = kNumHashBuckets);

    void setSecondPhase() {
        IndexConsistency::setSecondPhase();
    }

    /**
     * Traverses the column-store index via 'cursor' and accumulates the traversal results.
     */
    int64_t traverseIndex(OperationContext* opCtx,
                          const IndexCatalogEntry* index,
                          ProgressMeterHolder& _progress,
                          ValidateResults* results);

    /**
     * Traverses all paths in a single record from the row-store via the given {'recordId','record'}
     * pair and accumulates the traversal results.
     */
    void traverseRecord(OperationContext* opCtx,
                        const CollectionPtr& coll,
                        const IndexCatalogEntry* index,
                        const RecordId& recordId,
                        const BSONObj& recordBson,
                        ValidateResults* results);

    /**
     * Returns true if any value in the `_indexKeyCount` map is not equal to 0, otherwise return
     * false.
     */
    bool haveEntryMismatch() const;

    /**
     * If repair mode enabled, try inserting _missingIndexEntries into indexes.
     */
    void repairIndexEntries(OperationContext* opCtx, ValidateResults* results);

    /**
     * Records the errors gathered from the second phase of index validation into the provided
     * ValidateResultsMap and ValidateResults.
     */
    void addIndexEntryErrors(OperationContext* opCtx, ValidateResults* results);

    /**
     * Sets up this IndexConsistency object to limit memory usage in the second phase of index
     * validation. Returns whether the memory limit is sufficient to report at least one index entry
     * inconsistency and continue with the second phase of validation.
     */
    bool limitMemoryUsageForSecondPhase(ValidateResults* result);

    void validateIndexKeyCount(OperationContext* opCtx,
                               const IndexCatalogEntry* index,
                               long long* numRecords,
                               IndexValidateResults& results);

    uint64_t getTotalIndexKeys() {
        return _totalIndexKeys;
    }

private:
    // Comparator used by index inconsistency maps to ensure that we traverse inconsistencies in the
    // same order that we would traverse the indices themselves.
    struct AlphabeticalByIndexNameComparator {
        bool operator()(const IndexKey& lhs, const IndexKey& rhs) const;
    };

    KeyStringIndexConsistency() = delete;

    // A vector of IndexInfo indexes by index number
    IndexInfoMap _indexesInfo;

    // Populated during the second phase of validation, this map contains the index entries that
    // were pointing at an invalid document key.
    // The map contains a IndexKey pointing at a list of RecordIDs as there may be multiple
    // extra index entries for the same IndexKey.
    IndexInconsistencyMap<std::vector<RecordId>> _extraIndexEntries;

    // Populated during the second phase of validation, this map contains the index entries that
    // were missing while the document key was in place.
    // The map contains a IndexKey pointing to a single RecordId as there can only be one missing
    // index entry for a given IndexKey for each index.
    IndexInconsistencyMap<RecordId> _missingIndexEntries;

    // The total number of index keys is stored during the first validation phase, since this
    // count may change during a second phase.
    uint64_t _totalIndexKeys = 0;

    /**
     * Return info for an index tracked by this with the given 'indexName'.
     */
    IndexInfo& getIndexInfo(const std::string& indexName) {
        return _indexesInfo.at(indexName);
    }

    /**
     * During the first phase of validation, given the document's key KeyString, increment the
     * corresponding `_indexKeyCount` by hashing it.
     * For the second phase of validation, keep track of the document keys that hashed to
     * inconsistent hash buckets during the first phase of validation.
     */
    void addDocKey(OperationContext* opCtx,
                   const key_string::Value& ks,
                   IndexInfo* indexInfo,
                   const RecordId& recordId,
                   ValidateResults* results);

    /**
     * During the first phase of validation, given the index entry's KeyString, decrement the
     * corresponding `_indexKeyCount` by hashing it.
     * For the second phase of validation, try to match the index entry keys that hashed to
     * inconsistent hash buckets during the first phase of validation to document keys.
     */
    void addIndexKey(OperationContext* opCtx,
                     const IndexCatalogEntry* entry,
                     const key_string::Value& ks,
                     IndexInfo* indexInfo,
                     const RecordId& recordId,
                     ValidateResults* results);

    /**
     * Returns true if we should skip doing the hash bucket counting to detect extra/missing index
     * keys. This is currently done for geo indexes as they can experience rounding errors in trig
     * functions leading to false positives
     */
    bool skipTrackingIndexKeyCount(const IndexInfo& indexInfo);

    /**
     * During the first phase of validation, tracks the multikey paths for every observed document.
     */
    void addDocumentMultikeyPaths(IndexInfo* indexInfo, const MultikeyPaths& multikeyPaths);

    /**
     * To validate $** multikey metadata paths, we first scan the collection and add a hash of all
     * multikey paths encountered to a set. We then scan the index for multikey metadata path
     * entries and remove any path encountered. As we expect the index to contain a super-set of
     * the collection paths, a non-empty set represents an invalid index.
     */
    void addMultikeyMetadataPath(const key_string::Value& ks, IndexInfo* indexInfo);
    void removeMultikeyMetadataPath(const key_string::Value& ks, IndexInfo* indexInfo);
    size_t getMultikeyMetadataPathCount(IndexInfo* indexInfo);

    /**
     * Generates information about missing/extra index entries for the second phase of validation
     * and adds it to the results. The format is the following:
     * {
     *     indexName: <string>,
     *     recordId: <number>,
     *     idKey: <object>,  // Only available for missing index entries.
     *     indexKey: {
     *         <key>: <value>,
     *         ...
     *     }
     * }
     */
    void _foundInconsistency(OperationContext* opCtx,
                             const IndexKey& key,
                             const RecordId& recordId,
                             ValidateResults& results,
                             bool isMissing);

    /**
     * Returns a hashed value from the given KeyString and index namespace.
     */
    uint32_t _hashKeyString(const key_string::Value& ks, uint32_t indexNameHash) const;

};  // KeyStringIndexConsistency
}  // namespace mongo
