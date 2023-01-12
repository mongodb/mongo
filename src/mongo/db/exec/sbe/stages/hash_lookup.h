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

#include <vector>

#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/db/query/query_knobs_gen.h"

namespace mongo::sbe {
/**
 * Performs a multi-key hash lookup, that is a combination of left join and wind operations.
 * Rows from 'inner' and 'outer' sides can be associated with multiple keys. 'inner' and 'outer'
 * rows are considered matching, if they match on at least one of the associated keys. The result of
 * a lookup is each 'outer' paired paired with an array containing all matched 'inner' rows for that
 * 'outer' row. If no 'inner' rows match, Nothing value will be used instead of array.
 *
 * All rows from the 'inner' side are used to construct a hash table. Each 'inner' row can be
 * associated with multiple hash table entries. To avoid space amplification, each 'inner' row is
 * given a unique sequential id and the hash table maps keys to ids rather than rows themselves.
 *
 * This is a binding reflector for the 'inner' side; since the data is materialized in a hash
 * table, stages higher in the tree cannot see any slots lower in the tree on the  'inner' side.
 * This is _not_ the case for the 'outer' side, since it can stream data as it probes the hash
 * table. This stage preserves all slots and order of the 'outer' side.
 *
 * The 'outerCond' specifies the slot that contains match keys for the 'outer' row. If the
 * 'outerCond' slot contains an array, the array items will be used as match keys, otherwise the
 * slot value itself will be used a single match key.
 *
 * The 'innerCond' specifies the slot that contains match keys for the 'inner' row. If the
 * 'innerCond' slot contains an array, the array items will be used as match keys, otherwise the
 * slot value itself will be used as a single match key.
 *
 * The 'innerProjects' specifies the slots that contains the projected values of the 'inner' row.
 * These values will be buffered and made visible to 'innerAggs' expressions.
 *
 * The 'innerAggs' specifies an aggregate expressions SlotMap that will be used to compute the
 * aggregation results for each outer row. Those slots are accessible outside of this stage.
 *
 * An optional 'collatorSlot' can be provided to make the match predicate use a special definition
 * for string equality. For example, this can be used to perform a case-insensitive matching on
 * string values.
 *
 * Debug string representation:
 *
 *   hash_lookup [slot_1 = expr_1, ..., slot_n = expr_n] collatorSlot?
 *     outer outerCond outerStage
 *     inner innerCond [innerProjects] innerStage
 */
class HashLookupStage final : public PlanStage {
public:
    HashLookupStage(std::unique_ptr<PlanStage> outer,
                    std::unique_ptr<PlanStage> inner,
                    value::SlotId outerCond,
                    value::SlotId innerCond,
                    value::SlotVector innerProjects,
                    value::SlotMap<std::unique_ptr<EExpression>> innerAggs,
                    boost::optional<value::SlotId> collatorSlot,
                    PlanNodeId planNodeId,
                    bool participateInTrialRunTracking = true);

    std::unique_ptr<PlanStage> clone() const final;

    void prepare(CompileCtx& ctx) final;
    value::SlotAccessor* getAccessor(CompileCtx& ctx, value::SlotId slot) final;
    void open(bool reOpen) final;
    PlanState getNext() final;
    void close() final;

    std::unique_ptr<PlanStageStats> getStats(bool includeDebugInfo) const final;
    const SpecificStats* getSpecificStats() const final;
    std::vector<DebugPrinter::Block> debugPrint() const final;
    size_t estimateCompileTimeSize() const final;

protected:
    void saveChildrenState(bool relinquishCursor, bool disableSlotAccess) final;

private:
    using HashTableType = std::unordered_map<value::MaterializedRow,  // NOLINT
                                             std::vector<size_t>,
                                             value::MaterializedRowHasher,
                                             value::MaterializedRowEq>;

    using BufferType = std::vector<value::MaterializedRow>;

    using HashKeyAccessor = value::MaterializedRowKeyAccessor<HashTableType::iterator>;
    using BufferAccessor = value::MaterializedRowAccessor<BufferType>;

    void reset();
    template <typename C>
    void accumulateFromValueIndices(const C& projectIndices);

    // Spilling helpers.
    void addHashTableEntry(value::SlotAccessor* keyAccessor, size_t valueIndex);
    void spillBufferedValueToDisk(OperationContext* opCtx,
                                  RecordStore* rs,
                                  size_t bufferIdx,
                                  const value::MaterializedRow&);
    size_t bufferValueOrSpill(value::MaterializedRow& value);
    void setInnerProjectSwitchAccessor(int idx);

    boost::optional<std::vector<size_t>> readIndicesFromRecordStore(RecordStore* rs,
                                                                    value::TypeTags tagKey,
                                                                    value::Value valKey);

    void writeIndicesToRecordStore(RecordStore* rs,
                                   value::TypeTags tagKey,
                                   value::Value valKey,
                                   const std::vector<size_t>& value,
                                   bool update);

    void spillIndicesToRecordStore(RecordStore* rs,
                                   value::TypeTags tagKey,
                                   value::Value valKey,
                                   const std::vector<size_t>& value);
    /**
     * Constructs a RecordId for a value index. It must be shifted by 1 since a valid RecordId
     * with the value 0 is invalid.
     */
    RecordId getValueRecordId(size_t index) {
        return RecordId(static_cast<int64_t>(index) + 1);
    }

    bool hasSpilledHtToDisk() {
        return _recordStoreHt != nullptr;
    }

    bool hasSpilledBufToDisk() {
        return _recordStoreBuf != nullptr;
    }

    void makeTemporaryRecordStore();

    std::pair<RecordId, KeyString::TypeBits> serializeKeyForRecordStore(
        const value::MaterializedRow& key) const;

    /**
     * Normalizes a string if _collatorSlot is pouplated and returns a third parameter to let the
     * caller know if it should own the tag and value.
     */
    std::tuple<bool, value::TypeTags, value::Value> normalizeStringIfCollator(value::TypeTags tag,
                                                                              value::Value val);


    PlanStage* outerChild() const {
        return _children[0].get();
    }
    PlanStage* innerChild() const {
        return _children[1].get();
    }

    const value::SlotId _outerCond;
    const value::SlotId _innerCond;
    const value::SlotVector _innerProjects;
    const value::SlotMap<std::unique_ptr<EExpression>> _innerAggs;
    const boost::optional<value::SlotId> _collatorSlot;
    CollatorInterface* _collator{nullptr};

    value::SlotAccessorMap _outAccessorMap;
    value::SlotAccessorMap _outInnerProjectAccessorMap;

    value::SlotAccessor* _inOuterMatchAccessor{nullptr};

    value::SlotAccessor* _inInnerMatchAccessor{nullptr};
    std::vector<value::SlotAccessor*> _inInnerProjectAccessors;
    std::vector<value::SwitchAccessor> _outInnerProjectAccessors;
    std::vector<value::MaterializedSingleRowAccessor> _outResultAggAccessors;
    std::vector<std::unique_ptr<vm::CodeFragment>> _aggCodes;

    // The accessor for the '_ht' agg value.
    std::vector<value::MaterializedSingleRowAccessor> _outAggAccessors;

    // Accessor for the buffered value stored in the '_recordStore' when data is spilled to
    // disk.
    value::MaterializedRow _bufValueRecordStore{0};
    std::vector<value::MaterializedSingleRowAccessor> _outInnerBufValueRecordStoreAccessors;
    std::vector<BufferAccessor> _outInnerBufferProjectAccessors;

    // Accessor for collator. Only set if collatorSlot provided during construction.
    value::SlotAccessor* _collatorAccessor{nullptr};

    // Key used to probe inside the hash table.
    value::MaterializedRow _probeKey;

    // Result aggregate row;
    value::MaterializedRow _resultAggRow;

    BufferType _buffer;
    size_t _bufferIt{0};
    long long _valueId{0};
    boost::optional<HashTableType> _ht;

    vm::ByteCode _bytecode;

    bool _compileInnerAgg{false};

    // Memory tracking and spilling to disk.
    long long _memoryUseInBytesBeforeSpill =
        internalQuerySBELookupApproxMemoryUseInBytesBeforeSpill.load();
    int _currentSwitchIdx = 0;

    // This counter tracks an exact size for the '_ht' and an approximate size for the buffered
    // rows in '_buffer'.
    long long _computedTotalMemUsage = 0;

    std::unique_ptr<TemporaryRecordStore> _recordStoreHt;
    std::unique_ptr<TemporaryRecordStore> _recordStoreBuf;

    HashLookupStats _specificStats;
};
}  // namespace mongo::sbe
