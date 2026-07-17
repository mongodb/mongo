// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/ordering.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/db/throttle_cursor.h"
#include "mongo/db/validate/validate_results.h"
#include "mongo/db/validate/validate_state.h"
#include "mongo/util/modules.h"
#include "mongo/util/progress_meter.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_set.h>

namespace mongo {

class IndexCatalogEntry;

/**
 * Contains all the index information and stats throughout the validation.
 */
class IndexInfo {
public:
    explicit IndexInfo(const IndexCatalogEntry& entry);

    const IndexCatalogEntry& getEntry() const {
        return *_entry;
    }
    uint32_t indexNameHash() const {
        return _indexNameHash;
    }
    const Ordering& ord() const {
        return _ord;
    }
    int64_t numKeys = 0;
    int64_t numRecords = 0;
    absl::flat_hash_set<uint32_t> hashedMultikeyMetadataPaths;
    bool multikeyDocs = false;
    MultikeyPaths docMultikeyPaths;

private:
    std::shared_ptr<const IndexCatalogEntry> _entry;
    uint32_t _indexNameHash = 0;
    Ordering _ord;
};

/**
 * The KeyStringIndexConsistency class is used to keep track of the index consistency for
 * KeyString based indexes. It does this by using the index keys from index entries and index keys
 * generated from the document to ensure there is a one-to-one mapping for each key. In addition, an
 * IndexObserver class can be hooked into the IndexAccessMethod to inform this class about changes
 * to the indexes during a validation and compensate for them.
 */
class KeyStringIndexConsistency {
    struct AlphabeticalByIndexNameComparator;

public:
    struct IndexKeyBucket {
        uint32_t indexKeyCount = 0;
        uint32_t bucketSizeBytes = 0;
    };

    using IndexInfoMap = std::map<std::string, IndexInfo>;
    using IndexKey = std::pair<IndexInfo*, key_string::Value>;
    // TODO(SERVER-62257): Drop the comparator, unfortunately we can't do that for now because there
    // are *quite problematic* dependencies between the order that we repair inconsistencies and the
    // order we determine discrepancies in the count.
    template <typename T>
    using IndexInconsistencyMap = std::map<IndexKey, T, AlphabeticalByIndexNameComparator>;

    static constexpr int64_t kInterruptIntervalNumRecords = 4096;
    static constexpr size_t kNumHashBuckets = 1U << 16;
    enum class Phase { kFirst, kSecond };

    KeyStringIndexConsistency() = delete;
    KeyStringIndexConsistency(OperationContext* opCtx,
                              collection_validation::ValidateState* validateState,
                              size_t numHashBuckets = kNumHashBuckets);

    KeyStringIndexConsistency(KeyStringIndexConsistency&&) noexcept = default;
    KeyStringIndexConsistency& operator=(KeyStringIndexConsistency&&) noexcept = default;

    // A custom implementation of copy constructors and assignment operators is required as there
    // are internal pointers between IndexKey and IndexInfoMap types.
    KeyStringIndexConsistency(const KeyStringIndexConsistency& other);
    KeyStringIndexConsistency& operator=(const KeyStringIndexConsistency& other);

    ~KeyStringIndexConsistency() noexcept = default;

    /**
     * Informs the instance that we're advancing to the second phase of
     * index validation.
     */
    void setSecondPhase();

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
     * Sets up this instance to limit memory usage in the second phase of index
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

    /**
     * Merges another instance of this class into this one by inserting elements from the other's
     * container member variables into the containers owned by this. It is only valid for
     * instances of this class that are still in the First Phase of index consistency checks.
     */
    void merge(const KeyStringIndexConsistency& other);

    /**
     * Returns the index-key consistency buckets accumulated so far. First-phase scans are
     * order-independent and reproduce a single serial scan.
     */
    std::span<const IndexKeyBucket> getIndexKeyBuckets() const {
        return _indexKeyBuckets;
    }

private:
    collection_validation::ValidateState* _validateState;

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

    Phase _phase{Phase::kFirst};

    // Comparator used by index inconsistency maps to ensure that we traverse inconsistencies in the
    // same order that we would traverse the indices themselves.
    struct AlphabeticalByIndexNameComparator {
        bool operator()(const IndexKey& lhs, const IndexKey& rhs) const;
    };

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
