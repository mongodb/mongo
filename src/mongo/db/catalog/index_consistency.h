
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

#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/util/elapsed_tracker.h"

namespace mongo {

/**
 * The IndexConsistency class is used to keep track of the index consistency.
 * It does this by using the index keys from index entries and index keys generated from the
 * document to ensure there is a one-to-one mapping for each key.
 * In addition, an IndexObserver class can be hooked into the IndexAccessMethod to inform
 * this class about changes to the indexes during a validation and compensate for them.
 */

/**
 * Contains all the index information and stats throughout the validation.
 */
struct IndexInfo {
    // The name of the index.
    std::string indexName;
    // The index key pattern.
    BSONObj keyPattern;
    // Informs us if the index was ready or not for consumption during the start of validation.
    bool isReady;
    // Contains the pre-computed hash of the index name.
    uint32_t indexNameHash;
    // True if the index has finished scanning from the index scan stage, otherwise false.
    bool indexScanFinished;
    // The number of index entries belonging to the index.
    int64_t numKeys;
    // The number of long keys that are not indexed for the index.
    int64_t numLongKeys;
    // The number of records that have a key in their document that referenced back to the
    // this index
    int64_t numRecords;
    // Keeps track of how many indexes were removed (-1) and added (+1) after the
    // point of validity was set for this index.
    int64_t numExtraIndexKeys;
};

class IndexConsistency final {
    using ValidateResultsMap = std::map<std::string, ValidateResults>;

public:
    IndexConsistency(OperationContext* opCtx,
                     Collection* collection,
                     NamespaceString nss,
                     RecordStore* recordStore,
                     std::unique_ptr<Lock::CollectionLock> collLk,
                     const bool background);

    /**
     * Helper functions for `_addDocKey` and `_addIndexKey` for concurrency control.
     */
    void addDocKey(const KeyString& ks,
                   int indexNumber,
                   const RecordId& recordId,
                   const BSONObj& indexKey);
    void addIndexKey(const KeyString& ks,
                     int indexNumber,
                     const RecordId& recordId,
                     const BSONObj& indexKey);

    /**
     * Add one to the `_longKeys` count for the given `indexNs`.
     * This is required because index keys > `KeyString::kMaxKeyBytes` are not indexed.
     */
    void addLongIndexKey(int indexNumber);

    /**
     * Returns the number of index entries for the given `indexNs`.
     */
    int64_t getNumKeys(int indexNumber) const;

    /**
     * Returns the number of long keys that were not indexed for the given `indexNs`.
     */
    int64_t getNumLongKeys(int indexNumber) const;

    /**
     * Return the number of records with keys for the given `indexNs`.
     */
    int64_t getNumRecords(int indexNumber) const;

    /**
     * Returns true if any value in the `_indexKeyCount` map is not equal to 0, otherwise
     * return false.
     */
    bool haveEntryMismatch() const;

    /**
     * Index entries may be added or removed by concurrent writes during the index scan phase,
     * after establishing the point of validity. We need to account for these additions and
     * removals so that when we validate the index key count, we also have a pre-image of the
     * index counts and won't get incorrect results because of the extra index entries we may or
     * may not have scanned.
     */
    int64_t getNumExtraIndexKeys(int indexNumber) const;

    /**
     * Returns the index number for the corresponding index name.
     */
    int getIndexNumber(const std::string& indexName);

    /**
     * Informs the IndexConsistency object that we're advancing to the second phase of index
     * validation.
     */
    void setSecondPhase();

    /**
     * Records the errors gathered from the second phase of index validation into the provided
     * ValidateResultsMap and ValidateResults.
     */
    void addIndexEntryErrors(ValidateResultsMap* indexNsResultsMap, ValidateResults* results);

    /**
     * Returns true if a new snapshot should be accquired.
     * If the `recordId` is equal to or greater than `_yieldAtRecordId` then we must get
     * a new snapshot otherwise we will use stale data.
     * Otherwise we do not need a new snapshot and can continue with the collection scan.
     */
    bool shouldGetNewSnapshot(const RecordId recordId) const;

    /**
     * Returns true if a new snapshot should be accquired.
     * If the `keyString` is equal to or greater than `_yieldAtIndexEntry` then we must get
     * a new snapshot otherwise we will use stale data.
     * Otherwise we do not need a new snapshot and can continue with the index scan.
     */
    bool shouldGetNewSnapshot(const KeyString& keyString) const;

    /**
     * Gives up the lock that the collection is currently held in and requests the
     * the collection again in LockMode `mode`
     */
    void relockCollectionWithMode(LockMode mode);

    /**
     * Returns true if the ElapsedTracker says its time to yield during background validation.
     */
    bool scanLimitHit();

private:
    IndexConsistency() = delete;

    OperationContext* _opCtx;
    Collection* _collection;
    const NamespaceString _nss;
    const RecordStore* _recordStore;
    std::unique_ptr<Lock::CollectionLock> _collLk;
    const bool _isBackground;
    ElapsedTracker _tracker;

    // Protects the variables below.
    mutable stdx::mutex _classMutex;

    // We map the hashed KeyString values to a bucket which contain the count of how many
    // index keys and document keys we've seen in each bucket.
    // Count rules:
    //     - If the count is 0 in the bucket, we have index consistency for
    //       KeyStrings that mapped to it
    //     - If the count is > 0 in the bucket at the end of the validation pass, then there
    //       are too few index entries.
    //     - If the count is < 0 in the bucket at the end of the validation pass, then there
    //       are too many index entries.
    std::map<uint32_t, int32_t> _indexKeyCount;

    // Contains the corresponding index number for each index namespace
    std::map<std::string, int> _indexNumber;

    // A mapping of index numbers to IndexInfo
    std::map<int, IndexInfo> _indexesInfo;

    // Whether we're in the first or second phase of index validation.
    bool _firstPhase;

    // Populated during the second phase of validation, this map contains the index entries that
    // were pointing at an invalid document key.
    // The map contains a KeyString pointing at a set of BSON objects as there may be multiple
    // extra index entries for the same KeyString.
    std::map<std::string, BSONObjSet> _extraIndexEntries;

    // Populated during the second phase of validation, this map contains the index entries that
    // were missing while the document key was in place.
    // The map contains a KeyString pointing to a BSON object as there can only be one missing index
    // entry for a given KeyString.
    std::map<std::string, BSONObj> _missingIndexEntries;

    /**
     * During the first phase of validation, given the document's key KeyString, increment the
     * corresponding `_indexKeyCount` by hashing it.
     * For the second phase of validation, keep track of the document keys that hashed to
     * inconsistent hash buckets during the first phase of validation.
     */
    void _addDocKey_inlock(const KeyString& ks,
                           int indexNumber,
                           const RecordId& recordId,
                           const BSONObj& indexKey);

    /**
     * During the first phase of validation, given the index entry's KeyString, decrement the
     * corresponding `_indexKeyCount` by hashing it.
     * For the second phase of validation, try to match the index entry keys that hashed to
     * inconsistent hash buckets during the first phase of validation to document keys.
     */
    void _addIndexKey_inlock(const KeyString& ks,
                             int indexNumber,
                             const RecordId& recordId,
                             const BSONObj& indexKey);

    /**
     * Generates a key for the second phase of validation. The keys format is the following:
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
    BSONObj _generateInfo(const int& indexNumber,
                          const RecordId& recordId,
                          const BSONObj& indexKey,
                          boost::optional<BSONElement> idKey);

    /**
     * Returns a hashed value from the given KeyString and index namespace.
     */
    uint32_t _hashKeyString(const KeyString& ks, int indexNumbers) const;

};  // IndexConsistency
}  // namespace mongo
