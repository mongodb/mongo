/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/util/spilling.h"
#include "mongo/db/exec/sbe/values/row.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/stage_memory_limit_knobs/knobs.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo::sbe {
using HashTableType = std::unordered_map<value::MaterializedRow,  // NOLINT
                                         std::vector<size_t>,
                                         value::MaterializedRowHasher,
                                         value::MaterializedRowEq>;
using BufferType = std::vector<value::MaterializedRow>;
using RecordIndexCollection = std::variant<std::vector<size_t>*, std::set<size_t>*>;

class LookupHashTable;

////////////////////////////////////////////////////////////////////////////////////////////////////
// Class LookupHashTableIter
////////////////////////////////////////////////////////////////////////////////////////////////////

/**
 * Defines iterator state for HashStageLookupUnwind ("$LU")'s following layers of iteration:
 *   1. Outer doc
 *   2.   Outer doc key array (may also be a scalar, but in production passed as ArraySet of size 1)
 *   3.     Hash table match array for the key (may have multiple matches for a single key)
 * This enables iterating through all hash table matches for a set of keys.
 *
 * To reduce the amount of memory used and the number of mallocs needed, this class is tightly
 * coupled to the LookupHashTable class.
 */
class LookupHashTableIter {
public:
    // Indicates there is no document index, and thus no document, in the hash table matching the
    // request.
    static constexpr size_t kNoMatchingIndex = std::numeric_limits<size_t>::max();

    LookupHashTableIter(LookupHashTable& hashTable) : _hashTable{hashTable} {}

    /**
     * Clears the iterator's state.
     */
    inline void clear() {
        _hashTableMatchVector.clear();
        _hashTableMatchSet.clear();
        _hashTableSearched = false;

        // Force the iterator to return 'kNoMatchingIndex' until it is reset to a new key.
        _outerKeyIsArray = false;
        _hashTableMatchVectorIdx = 0;
    }

    /**
     * Returns all matching indices for the current hash keys.
     */
    inline RecordIndexCollection getAllMatchingIndices() {
        if (_outerKeyIsArray) {
            initSearchArray();
            return &_hashTableMatchSet;
        } else {
            initSearchScalar();
            return &_hashTableMatchVector;
        }
    }

    /**
     * Returns the next hash table document store index for the current set of hash keys AND
     * advances the iterator, or returns 'kNoMatchingIndex' if none.
     */
    size_t getNextMatchingIndex();

    /**
     * Resets the iterator state to the start of a (new) outer key or key array.
     * Must be called before "getNextMatchingIndex()" or "getAllMatchingIndices()" are called.
     */
    void reset(const value::TypeTags& outerKeyTag, const value::Value& outerKeyVal);

private:
    /**
     * Initializes the iterator's position for the start of a search for a new outer array key.
     */
    void initSearchArray();

    /**
     * Initializes the iterator's position for the start of a search for a new outer scalar key.
     */
    void initSearchScalar();

    ////////////////////////////////////////////////////////////////////////////////////////////////
    // LookupHashTableIter data members
    ////////////////////////////////////////////////////////////////////////////////////////////////

    // The LookupHashTable instance this is iterating, so we can call some methods in it.
    LookupHashTable& _hashTable;

    // Tag of the current outer key, which may be a scalar or array.
    value::TypeTags _outerKeyTag = value::TypeTags::Nothing;
    // Value of the current outer key, which may be a scalar or array.
    value::Value _outerKeyVal = 0;
    // Indicates whether the outer key is a scalar or array.
    bool _outerKeyIsArray = false;
    // Have we looked for the current individual key yet ('_hashTableMatchXyz' members are valid)?
    bool _hashTableSearched = false;

    // If '_outerKeyIsArray' is false, a sorted vector of inner key match buffer indices.
    std::vector<size_t> _hashTableMatchVector;
    // If '_outerKeyIsArray' is false, the current position's index into '_hashTableMatchVector'.
    size_t _hashTableMatchVectorIdx = 0;
    // If '_outerKeyIsArray' is true, a sorted set of inner key match buffer indices.
    std::set<size_t> _hashTableMatchSet;
    // If '_outerKeyIsArray' is true, the current position in '_hashTableMatchSet'.
    std::set<size_t>::const_iterator _hashTableMatchSetIter;

    // Outer key used to probe the hash table.
    value::MaterializedRow _iterProbeKey{1 /* columns */};
};  // class LookupHashTableIter

////////////////////////////////////////////////////////////////////////////////////////////////////
// Class LookupHashTable
////////////////////////////////////////////////////////////////////////////////////////////////////

/**
 * Implements a hash table with a memory limit and spilling to disk when the limit would be
 * exceeded. Used by HashLookupStage and HashLookupUnwindStage to contain the $lookup's inner
 * collection documents.
 */
class LookupHashTable {
public:
    /**
     * Adds a key entry to the hash table pointing to a doc in its memory or disk store.
     */
    void addHashTableEntry(value::SlotAccessor* keyAccessor, size_t valueIndex);

    /**
     * Adds to the hash table's buffer of docs each inner doc as a MaterializedRow that has just a
     * single column containing the inner doc. The doc ID ("index") in the memory portion of the
     * buffer is a 0-based incrementing size_t which is used as the index into the '_buffer' vector,
     * while the index in the disk portion ('_recordStoreBuf') is 1-based (so ID N in memory becomes
     * ID N+1 on disk) because SpillingStore does not support RecordIds of 0.
     *
     * RowBase::getViewOfValue() can be used to view (tag, val) of a column from a MaterializedRow,
     * given the column index in the row, which in the case of this stage is always 0.
     *
     * The current method adds to the memory buffer until it gets too big, then calls
     * spillBufferedValueToDisk() to start spilling to disk.
     */
    size_t bufferValueOrSpill(value::MaterializedRow& value);

    void doSaveState();
    void doRestoreState();

    void forceSpill();

    const HashLookupStats* getHashLookupStats() const {
        return &_hashLookupStats;
    }

    /**
     * Retrieves a slot view (tag, val) of the value at 'index' of the hash table's sequential
     * store, whether it is in the memory or disk portion.
     */
    boost::optional<std::pair<value::TypeTags, value::Value>> getValueAtIndex(size_t index);

    /**
     * Constructs a RecordId for a value index. It must be incremented since a valid RecordId with
     * the value 0 is invalid.
     */
    static RecordId getValueRecordId(size_t index) {
        return RecordId(static_cast<int64_t>(index) + 1);
    }

    /**
     * Opens a new, empty hash table, with a collator if one was provided.
     */
    void open() {
        init();
    }

    void reset(bool fromClose);

    /**
     * Sets the collator for the query if one was specified.
     */
    inline void setCollator(CollatorInterface* collator) {
        _collator = collator;
    }

    /**
     * Delegate of PlanStage::doAttachToOperationContext().
     */
    inline void doAttachToOperationContext(OperationContext* opCtx) {
        _opCtx = opCtx;
    }

    /**
     * Delegate of PlanStage::doDetachFromOperationContext().
     */
    inline void doDetachFromOperationContext() {
        _opCtx = nullptr;
    }

    /**
     * The iterator for this hash table instance. Its public methods are part of the LookupHashTable
     * API.
     */
    LookupHashTableIter htIter{*this};

    int64_t getMemUsage() const {
        return _computedTotalMemUsage;
    }

private:
    void init() {
        if (_collator) {
            const value::MaterializedRowHasher hasher(_collator);
            const value::MaterializedRowEq equator(_collator);
            _memoryHt.emplace(0, hasher, equator);
        } else {
            _memoryHt.emplace();
        }
    }

    inline bool hasSpilledBufToDisk() {
        return _recordStoreBuf != nullptr;
    }

    inline bool hasSpilledHtToDisk() {
        return _recordStoreHt != nullptr;
    }

    void makeTemporaryRecordStore();

    // Determines if we should perform the check for sufficient disk space for spilling.
    // We do the check after every 100MB of spilling.
    bool shouldCheckDiskSpace();

    /**
     * Normalizes a string if '_collator' is populated and returns a third parameter to let the
     * caller know if it should own the tag and value.
     */
    std::tuple<bool, value::TypeTags, value::Value> normalizeStringIfCollator(
        value::TypeTags tag, value::Value val) const;

    boost::optional<std::vector<size_t>> readIndicesFromRecordStore(SpillingStore* rs,
                                                                    value::TypeTags tagKey,
                                                                    value::Value valKey);

    std::pair<RecordId, key_string::TypeBits> serializeKeyForRecordStore(
        const value::MaterializedRow& key) const;

    // Writes an inner collection doc to the disk buffer. via upsertToRecordStore(). These can be
    // read back from the disk store via SpillingStore::readFromRecordStore() (spilling.h).
    void spillBufferedValueToDisk(SpillingStore* rs,
                                  size_t bufferIdx,
                                  const value::MaterializedRow&);

    void spillIndicesToRecordStore(SpillingStore* rs,
                                   value::TypeTags tagKey,
                                   value::Value valKey,
                                   const std::vector<size_t>& value);

    int64_t writeIndicesToRecordStore(SpillingStore* rs,
                                      value::TypeTags tagKey,
                                      value::Value valKey,
                                      const std::vector<size_t>& value,
                                      bool update);

    ////////////////////////////////////////////////////////////////////////////////////////////////
    // LookupHashTable data members
    ////////////////////////////////////////////////////////////////////////////////////////////////

    // The calling stage's 'opCtx'.
    OperationContext* _opCtx;

    // Collator, if one was specified.
    CollatorInterface* _collator{nullptr};

    // Memory portion of the hash table of the inner collection.
    boost::optional<HashTableType> _memoryHt;
    // Documents of the inner collection that are in memory.
    BufferType _buffer;
    // This counter tracks an exact size for the '_memoryHt' and an approximate size for the
    // buffered rows in '_buffer'.
    long long _computedTotalMemUsage = 0;

    // Memory tracking and spilling to disk.
    long long _memoryUseInBytesBeforeSpill =
        loadMemoryLimit(StageMemoryLimit::QuerySBELookupApproxMemoryUseInBytesBeforeSpill);

    // The portion of the inner collection hash table that has spilled to disk.
    std::unique_ptr<SpillingStore> _recordStoreHt;
    // Documents of the inner collection that have spilled to disk.
    std::unique_ptr<SpillingStore> _recordStoreBuf;

    // Next inner collection document index.
    size_t _valueId{0};

    // Outer key used to probe the hash table.
    value::MaterializedRow _htProbeKey{1 /* columns */};

    HashLookupStats _hashLookupStats;

    // Used to hold a copy of a MaterializedRow from the disk store, so it does not go out of scope
    // when getValueAtIndex() returns a view of it.
    boost::optional<value::MaterializedRow> _bufValueRecordStore;

    // Amount of bytes spilled since last time we performed the disk space check. We reset this
    // value and perform the disk space check everytime it crosses 100 MB.
    long long _spilledBytesSinceLastCheck{0};
    long long _totalSpilledBytes{0};

    static constexpr long long kMaxSpilledBytesForDiskSpaceCheck = 100ll * 1024 * 1024;

    friend class LookupHashTableIter;
};  // class LookupHashTable
}  // namespace mongo::sbe
