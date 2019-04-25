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
 * The ValidationStage allows the IndexConsistency class to perform
 * the correct operations that depend on where we are in the validation.
 */
enum class ValidationStage { DOCUMENT, INDEX, NONE };

/**
 * The ValidationOperation is used by classes using the IndexObserver to let us know what operation
 * was associated with it.
 * The `UPDATE` operation can be seen as two independent operations (`REMOVE` operation followed
 * by an `INSERT` operation).
 */
enum class ValidationOperation { INSERT, REMOVE };

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
    // Informs us if the index was ready or not for consumption during the start of validation.
    bool isReady;
    // Contains the pre-computed hashed of the index namespace.
    uint32_t indexNsHash;
    // True if the index has finished scanning from the index scan stage, otherwise false.
    bool indexScanFinished;
    // The number of index entries belonging to the index.
    int64_t numKeys;
    // The number of long keys that are not indexed for the index.
    int64_t numLongKeys;
    // The number of records that have a key in their document that referenced back to the
    // this index.
    int64_t numRecords;
    // Keeps track of how many indexes were removed (-1) and added (+1) after the
    // point of validity was set for this index.
    int64_t numExtraIndexKeys;
    // A hashed set of indexed multikey paths (applies to $** indexes only).
    std::set<uint32_t> hashedMultikeyMetadataPaths;
};

class IndexConsistency final {
public:
    IndexConsistency(OperationContext* opCtx,
                     Collection* collection,
                     NamespaceString nss,
                     RecordStore* recordStore,
                     const bool background);

    /**
     * Helper functions for `_addDocKey` and `_addIndexKey` for concurrency control.
     */
    void addDocKey(const KeyString& ks, int indexNumber);
    void addIndexKey(const KeyString& ks, int indexNumber);

    /**
     * To validate $** multikey metadata paths, we first scan the collection and add a hash of all
     * multikey paths encountered to a set. We then scan the index for multikey metadata path
     * entries and remove any path encountered. As we expect the index to contain a super-set of
     * the collection paths, a non-empty set represents an invalid index.
     */
    void addMultikeyMetadataPath(const KeyString& ks, int indexNumber);
    void removeMultikeyMetadataPath(const KeyString& ks, int indexNumber);
    size_t getMultikeyMetadataPathCount(int indexNumber);

    /**
     * Add one to the `_longKeys` count for the given `indexNs`.
     * This is required because index keys > `KeyString::kMaxKeyBytes` are not indexed.
     * TODO SERVER-36385: Completely remove the key size check in 4.4
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
     * Moves the `_stage` variable to the next corresponding stage in the following order:
     * `DOCUMENT` -> `INDEX`
     * `INDEX` -> `NONE`
     * `NONE` -> `NONE`
     */
    void nextStage();

    /**
     * Returns the `_stage` that the validation is on.
     */
    ValidationStage getStage() const;

    /**
     * Returns the index number for the corresponding index namespace's.
     */
    int getIndexNumber(const std::string& indexNs);

private:
    OperationContext* _opCtx;
    Collection* _collection;
    const NamespaceString _nss;
    const RecordStore* _recordStore;
    ElapsedTracker _tracker;

    // We map the hashed KeyString values to a bucket which contain the count of how many
    // index keys and document keys we've seen in each bucket.
    // Count rules:
    //     - If the count is 0 in the bucket, we have index consistency for
    //       KeyStrings that mapped to it
    //     - If the count is > 0 in the bucket at the end of the validation pass, then there
    //       are too few index entries.
    //     - If the count is < 0 in the bucket at the end of the validation pass, then there
    //       are too many index entries.
    std::map<uint32_t, uint32_t> _indexKeyCount;

    // Contains the corresponding index number for each index namespace
    std::map<std::string, int> _indexNumber;

    // A mapping of index numbers to IndexInfo
    std::map<int, IndexInfo> _indexesInfo;

    // The current index namespace being scanned in the index scan phase.
    int _currentIndex = -1;

    // The stage that the validation is currently on.
    ValidationStage _stage = ValidationStage::DOCUMENT;

    // Threshold for the number of errors to record before returning "There are too many errors".
    static const int _kErrorThreshold = 100;

    // The current number of errors that are recorded.
    int _numErrorsRecorded = 0;

    // Only one thread can use the class at a time
    mutable stdx::mutex _classMutex;

    /**
     * Given the document's key KeyString, increment the corresponding `_indexKeyCount`
     * by hashing it.
     */
    void _addDocKey_inlock(const KeyString& ks, int indexNumber);

    /**
     * Given the index entry's KeyString, decrement the corresponding `_indexKeyCount`
     * by hashing it.
     */
    void _addIndexKey_inlock(const KeyString& ks, int indexNumber);

    /**
     * Returns a hashed value from the given KeyString and index namespace.
     */
    uint32_t _hashKeyString(const KeyString& ks, int indexNumbers) const;
};  // IndexConsistency
}  // namespace mongo
