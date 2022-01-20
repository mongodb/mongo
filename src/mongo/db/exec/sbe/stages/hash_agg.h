/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <unordered_map>

#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/storage/temporary_record_store.h"
#include "mongo/stdx/unordered_map.h"

namespace mongo {
namespace sbe {
/**
 * Performs a hash-based aggregation. Appears as the "group" stage in debug output. Groups the input
 * based on the provided vector of group-by slots, 'gbs'. The 'aggs' parameter is a map from
 * 'SlotId' to expression. This defines a set of output slots whose values will be computed based on
 * the corresponding aggregate expressions. Each distinct grouping will produce a single output,
 * consisting of the values of the group-by keys and the results of the aggregate functions.
 *
 * Since the data must be buffered in a hash table, this is a "binding reflector". This means slots
 * from the 'input' tree are not visible higher in tree. Stages higher in the tree can only see the
 * slots holding the group-by keys as well as those holding the corresponding aggregate values.
 *
 * The optional 'seekKeys', if provided, limit the results returned from the hash table only to
 * those equal to seekKeys.
 *
 * The 'optimizedClose' flag controls whether we can close the child subtree right after building
 * the hash table. If true it means that we do not expect the subtree to be reopened.
 *
 * The optional 'collatorSlot', if provided, changes the definition of string equality used when
 * determining whether two group-by keys are equal. For instance, the plan may require us to do a
 * case-insensitive group on a string field.
 *
 * Debug string representation:
 *
 *  group [<group by slots>] [slot_1 = expr_1, ..., slot_n = expr_n] [<seek slots>]? reopen?
 * collatorSlot? childStage
 */
class HashAggStage final : public PlanStage {
public:
    HashAggStage(std::unique_ptr<PlanStage> input,
                 value::SlotVector gbs,
                 value::SlotMap<std::unique_ptr<EExpression>> aggs,
                 value::SlotVector seekKeysSlots,
                 bool optimizedClose,
                 boost::optional<value::SlotId> collatorSlot,
                 bool allowDiskUse,
                 PlanNodeId planNodeId);

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
    void doSaveState(bool relinquishCursor) override;
    void doRestoreState(bool relinquishCursor) override;
    void doDetachFromOperationContext() override;
    void doAttachToOperationContext(OperationContext* opCtx) override;
    void doDetachFromTrialRunTracker() override;
    TrialRunTrackerAttachResultMask doAttachToTrialRunTracker(
        TrialRunTracker* tracker, TrialRunTrackerAttachResultMask childrenAttachResult) override;

private:
    boost::optional<value::MaterializedRow> getFromRecordStore(const RecordId& rid);

    using TableType = stdx::unordered_map<value::MaterializedRow,
                                          value::MaterializedRow,
                                          value::MaterializedRowHasher,
                                          value::MaterializedRowEq>;

    using HashKeyAccessor = value::MaterializedRowKeyAccessor<TableType::iterator>;
    using HashAggAccessor = value::MaterializedRowValueAccessor<TableType::iterator>;

    void makeTemporaryRecordStore();

    /**
     * Spills a key and value pair to the '_recordStore' where the semantics are insert or update
     * depending on the 'update' flag. When the 'update' flag is true this method already expects
     * the 'key' to be inserted into the '_recordStore', otherwise the 'key' and 'val' pair are
     * fresh.
     *
     * This method expects the key to be seralized into a KeyString::Value so that the key is
     * memcmp-able and lookups can be done to update the 'val' in the '_recordStore'. Note that the
     * 'typeBits' are needed to reconstruct the spilled 'key' when calling 'getNext' to deserialize
     * the 'key' to a MaterializedRow. Since the '_recordStore' only stores the memcmp-able part of
     * the KeyString we need to carry the 'typeBits' separately, and we do this by appending the
     * 'typeBits' to the end of the serialized 'val' buffer and store them at the leaves of the
     * backing B-tree of the '_recordStore'. used as the RecordId.
     */
    void spillValueToDisk(const RecordId& key,
                          const value::MaterializedRow& val,
                          const KeyString::TypeBits& typeBits,
                          bool update);


    const value::SlotVector _gbs;
    const value::SlotMap<std::unique_ptr<EExpression>> _aggs;
    const boost::optional<value::SlotId> _collatorSlot;
    const bool _allowDiskUse;
    const value::SlotVector _seekKeysSlots;
    // When this operator does not expect to be reopened (almost always) then it can close the child
    // early.
    const bool _optimizedClose{true};
    // Memory tracking variables.
    const long long _approxMemoryUseInBytesBeforeSpill =
        internalQuerySBEAggApproxMemoryUseInBytesBeforeSpill.load();
    const int _memoryUseSampleRate = internalQuerySBEAggMemoryUseSampleRate.load();
    // Used in collaboration with memoryUseSampleRatePercentage to determine whether we should
    // re-approximate memory usage.
    PseudoRandom _pseudoRandom = PseudoRandom(Date_t::now().asInt64());

    value::SlotAccessorMap _outAccessors;
    std::vector<value::SlotAccessor*> _inKeyAccessors;

    // Accesors for the key stored in '_ht', a SwitchAccessor is used so we can produce the key from
    // either the '_ht' or the '_recordStore'.
    std::vector<std::unique_ptr<HashKeyAccessor>> _outHashKeyAccessors;
    std::vector<std::unique_ptr<value::SwitchAccessor>> _outKeyAccessors;

    // Accessor for the agg state value stored in the '_recordStore' when data is spilled to disk.
    value::MaterializedRow _aggKeyRecordStore{0};
    value::MaterializedRow _aggValueRecordStore{0};
    std::vector<std::unique_ptr<value::MaterializedSingleRowAccessor>> _outRecordStoreKeyAccessors;
    std::vector<std::unique_ptr<value::MaterializedSingleRowAccessor>> _outRecordStoreAggAccessors;

    std::vector<value::SlotAccessor*> _seekKeysAccessors;
    value::MaterializedRow _seekKeys;

    // Accesors for the agg state in '_ht', a SwitchAccessor is used so we can produce the agg state
    // from either the '_ht' or the '_recordStore' when draining the HashAgg stage.
    std::vector<std::unique_ptr<value::SwitchAccessor>> _outAggAccessors;
    std::vector<std::unique_ptr<HashAggAccessor>> _outHashAggAccessors;
    std::vector<std::unique_ptr<vm::CodeFragment>> _aggCodes;

    // Only set if collator slot provided on construction.
    value::SlotAccessor* _collatorAccessor = nullptr;

    boost::optional<TableType> _ht;
    TableType::iterator _htIt;

    vm::ByteCode _bytecode;

    bool _compiled{false};
    bool _childOpened{false};

    // Used when spilling to disk.
    std::unique_ptr<TemporaryRecordStore> _recordStore;
    bool _drainingRecordStore{false};
    std::unique_ptr<SeekableRecordCursor> _rsCursor;

    // If provided, used during a trial run to accumulate certain execution stats. Once the trial
    // run is complete, this pointer is reset to nullptr.
    TrialRunTracker* _tracker{nullptr};
};

}  // namespace sbe
}  // namespace mongo
