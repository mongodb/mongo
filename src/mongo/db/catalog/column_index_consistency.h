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

#pragma once

#include <absl/hash/hash.h>
#include <algorithm>
#include <vector>

#include "mongo/db/catalog/index_consistency.h"
#include "mongo/db/catalog/throttle_cursor.h"
#include "mongo/db/catalog/validate_state.h"
#include "mongo/db/index/columns_access_method.h"
#include "mongo/db/storage/column_store.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/progress_meter.h"

namespace mongo {

/**
 * The ColumnIndexConsistency class is used to keep track of the index consistency in column-stor
 * indexes. It does this by using the index keys from index entries and index keys generated from
 * the document to ensure there is a one-to-one mapping for each key.
 * In addition, an IndexObserver class can be hooked into the IndexAccessMethod to inform this class
 * about changes to the indexes during a validation and compensate for them.
 */
class ColumnIndexConsistency final : protected IndexConsistency {

public:
    ColumnIndexConsistency(OperationContext* opCtx,
                           CollectionValidation::ValidateState* validateState,
                           const size_t numHashBuckets = kNumHashBuckets)
        : IndexConsistency(opCtx, validateState, numHashBuckets) {}

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
    bool haveEntryMismatch() const {
        return std::any_of(_indexKeyBuckets.cbegin(), _indexKeyBuckets.cend(), [](auto b) {
            return b.indexKeyCount != 0;
        });
    }

    /**
     * If repair mode enabled, tries to repair the given column-store index.
     */
    void repairIndexEntries(OperationContext* opCtx,
                            const IndexCatalogEntry* index,
                            ValidateResults* results);

    /**
     * If repair mode enabled, tries to repair the column-store indexes.
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
    bool limitMemoryUsageForSecondPhase(ValidateResults* result) {
        return true;
    }

    void validateIndexKeyCount(OperationContext* opCtx,
                               const IndexCatalogEntry* index,
                               long long* numRecords,
                               IndexValidateResults& results);

    uint64_t getTotalIndexKeys() {
        return _numIndexEntries;
    }

    //////////////////////////////////////////////////////////
    // Beginning of methods being public for testing purposes
    //////////////////////////////////////////////////////////
    int64_t getNumDocs() const {
        return _numDocs;
    }

    uint32_t getBucketCount(size_t bucketNum) const {
        invariant(bucketNum < _indexKeyBuckets.size());
        return _indexKeyBuckets[bucketNum].indexKeyCount;
    }

    uint32_t getBucketSizeBytes(size_t bucketNum) const {
        invariant(bucketNum < _indexKeyBuckets.size());
        return _indexKeyBuckets[bucketNum].bucketSizeBytes;
    }

    /**
     * Accumulates the info about a cell extracted from a shredded row-store record.
     */
    void addDocEntry(const FullCellView& val);

    /**
     * Accumulates the info about a column-store index entry cell.
     */
    void addIndexEntry(const FullCellView& val);

    static uint32_t _hash(const FullCellView& cell, uint64_t seed = 0) {
        MONGO_STATIC_ASSERT_MSG(std::is_same<decltype(cell.rid), int64_t>::value,
                                "'rid' should be an int64 (i.e., it's not a class) so that we're "
                                "not hashing based on the compiler chosen layout for a struct.");

        MONGO_STATIC_ASSERT_MSG(sizeof(uint64_t) <= sizeof(size_t),
                                "Unable to safely store a uint64_t value in a size_t variable");

        size_t hash = static_cast<size_t>(seed);
        boost::hash_combine(hash,
                            absl::hash_internal::CityHash64(cell.path.rawData(), cell.path.size()));
        boost::hash_combine(hash,
                            absl::hash_internal::CityHash64(
                                reinterpret_cast<const char*>(&cell.rid), sizeof(cell.rid)));
        boost::hash_combine(
            hash, absl::hash_internal::CityHash64(cell.value.rawData(), cell.value.size()));
        return hash;
    }
    //////////////////////////////////////////////////////////
    // End of methods being public for testing purposes
    //////////////////////////////////////////////////////////

private:
    ColumnIndexConsistency() = delete;

    /**
     * Pinpoints the errors from the accumulated information from traversal of both row-store and
     * column-store index and adds these errors to 'results'.
     */
    void _addIndexEntryErrors(OperationContext* opCtx,
                              const IndexCatalogEntry* index,
                              ValidateResults* results);

    void _investigateSuspects(OperationContext* opCtx, const IndexCatalogEntry* index);

    void _tabulateEntry(const FullCellView& cell, int step);

    BSONObj _generateInfo(const std::string& indexName,
                          const RecordId& recordId,
                          PathView path,
                          RowId rowId,
                          StringData value = "");

    void _updateSuspectList(const FullCellView& cell, ValidateResults* results);

    std::vector<FullCellValue> _missingIndexEntries;
    std::vector<FullCellValue> _extraIndexEntries;

    stdx::unordered_set<RowId> _suspects;
    int64_t _numDocs = 0;
    int64_t _numIndexEntries = 0;
    BufBuilder _cellBuilder;
};
}  // namespace mongo
