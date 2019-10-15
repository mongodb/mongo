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

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/catalog/validate_state.h"
#include "mongo/db/storage/key_string.h"

namespace mongo {

class IndexDescriptor;

/**
 * Contains all the index information and stats throughout the validation.
 */
struct IndexInfo {
    IndexInfo(const IndexDescriptor* descriptor);
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
};

/**
 * The IndexConsistency class is used to keep track of the index consistency.
 * It does this by using the index keys from index entries and index keys generated from the
 * document to ensure there is a one-to-one mapping for each key.
 * In addition, an IndexObserver class can be hooked into the IndexAccessMethod to inform
 * this class about changes to the indexes during a validation and compensate for them.
 */
class IndexConsistency final {
    using IndexInfoMap = std::map<std::string, IndexInfo>;
    using ValidateResultsMap = std::map<std::string, ValidateResults>;
    using IndexKey = std::pair<std::string, std::string>;

public:
    IndexConsistency(OperationContext* opCtx, CollectionValidation::ValidateState* validateState);

    /**
     * During the first phase of validation, given the document's key KeyString, increment the
     * corresponding `_indexKeyCount` by hashing it.
     * For the second phase of validation, keep track of the document keys that hashed to
     * inconsistent hash buckets during the first phase of validation.
     */
    void addDocKey(OperationContext* opCtx,
                   const KeyString::Value& ks,
                   IndexInfo* indexInfo,
                   RecordId recordId);

    /**
     * During the first phase of validation, given the index entry's KeyString, decrement the
     * corresponding `_indexKeyCount` by hashing it.
     * For the second phase of validation, try to match the index entry keys that hashed to
     * inconsistent hash buckets during the first phase of validation to document keys.
     */
    void addIndexKey(const KeyString::Value& ks, IndexInfo* indexInfo, RecordId recordId);

    /**
     * To validate $** multikey metadata paths, we first scan the collection and add a hash of all
     * multikey paths encountered to a set. We then scan the index for multikey metadata path
     * entries and remove any path encountered. As we expect the index to contain a super-set of
     * the collection paths, a non-empty set represents an invalid index.
     */
    void addMultikeyMetadataPath(const KeyString::Value& ks, IndexInfo* indexInfo);
    void removeMultikeyMetadataPath(const KeyString::Value& ks, IndexInfo* indexInfo);
    size_t getMultikeyMetadataPathCount(IndexInfo* indexInfo);

    /**
     * Returns true if any value in the `_indexKeyCount` map is not equal to 0, otherwise
     * return false.
     */
    bool haveEntryMismatch() const;

    /**
     * Return info on all indexes tracked by this.
     */
    IndexInfoMap& getIndexInfo() {
        return _indexesInfo;
    }
    IndexInfo& getIndexInfo(const std::string& indexName) {
        return _indexesInfo.at(indexName);
    }

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

private:
    IndexConsistency() = delete;

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

    std::vector<uint32_t> _indexKeyCount;

    // A vector of IndexInfo indexes by index number
    IndexInfoMap _indexesInfo;

    // Whether we're in the first or second phase of index validation.
    bool _firstPhase;

    // Populated during the second phase of validation, this map contains the index entries that
    // were pointing at an invalid document key.
    // The map contains a IndexKey pointing at a set of BSON objects as there may be multiple
    // extra index entries for the same IndexKey.
    std::map<IndexKey, SimpleBSONObjSet> _extraIndexEntries;

    // Populated during the second phase of validation, this map contains the index entries that
    // were missing while the document key was in place.
    // The map contains a IndexKey pointing to a BSON object as there can only be one missing index
    // entry for a given IndexKey for each index.
    std::map<IndexKey, BSONObj> _missingIndexEntries;

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
    BSONObj _generateInfo(const IndexInfo& indexInfo,
                          RecordId recordId,
                          const BSONObj& indexKey,
                          boost::optional<BSONElement> idKey);

    /**
     * Returns a hashed value from the given KeyString and index namespace.
     */
    uint32_t _hashKeyString(const KeyString::Value& ks, uint32_t indexNameHash) const;

};  // IndexConsistency
}  // namespace mongo
