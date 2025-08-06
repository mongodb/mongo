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

#include "mongo/db/exec/sbe/stages/lookup_hash_table.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/row.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/collation/collator_interface.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo::sbe {
/**
 * Performs a multi-key hash lookup, that is a combination of left join and wind operations. Rows
 * from 'inner' and 'outer' sides can be associated with multiple keys. 'inner' and 'outer' rows are
 * considered matching, if they match on at least one of the associated keys. The result of a lookup
 * is each 'outer' paired with an array containing all matched 'inner' rows for that 'outer' row. If
 * no 'inner' rows match, Nothing value will be used instead of array.
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
 * The 'outerKeySlot' specifies the slot that contains match keys for the 'outer' row. If the
 * 'outerKeySlot' slot contains an array, the array items will be used as match keys, otherwise the
 * slot value itself will be used a single match key.
 *
 * The 'innerKeySlot' specifies the slot that contains match keys for the 'inner' row. If the
 * 'innerKeySlot' slot contains an array, the array items will be used as match keys, otherwise the
 * slot value itself will be used as a single match key.
 *
 * The 'innerProjectSlot' specifies the slot that contains the projected inner row value.
 *
 * An optional 'collatorSlot' can be provided to make the match predicate use a special definition
 * for string equality. For example, this can be used to perform a case-insensitive matching on
 * string values.
 *
 * Debug string representation:
 *
 *   hash_lookup lookupStageOutputSlot collatorSlot?
 *     outer outerKeySlot outerStage
 *     inner innerKeySlot innerProject innerStage
 */
class HashLookupUnwindStage final : public PlanStage {
public:
    HashLookupUnwindStage(std::unique_ptr<PlanStage> outer,
                          std::unique_ptr<PlanStage> inner,
                          value::SlotId outerKeySlot,
                          value::SlotId innerKeySlot,
                          value::SlotId innerProjectSlot,
                          value::SlotId lookupStageOutputSlot,
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
    bool shouldOptimizeSaveState(size_t idx) const final {
        if (idx == 0) {
            // HashLookupUnwindStage::getNext() guarantees that the outer child's getNext() method
            // will be called when '_outerKeyOpen' is false.
            return !_outerKeyOpen;
        } else {
            // HashLookupUnwindStage::getNext() doesn't make any guarantees about the inner child.
            return false;
        }
    }

    void doAttachToOperationContext(OperationContext* opCtx) override;
    void doDetachFromOperationContext() override;

    void doSaveState() override;
    void doRestoreState() override;

    void doAttachCollectionAcquisition(const MultipleCollectionAccessor& mca) override {
        return;
    }

private:
    using HashTableType = std::unordered_map<value::MaterializedRow,  // NOLINT
                                             std::vector<size_t>,
                                             value::MaterializedRowHasher,
                                             value::MaterializedRowEq>;
    using BufferType = std::vector<value::MaterializedRow>;
    using HashKeyAccessor = value::MaterializedRowKeyAccessor<HashTableType::iterator>;
    using BufferAccessor = value::MaterializedRowAccessor<BufferType>;

    // Resets state of the hash table and miscellany. 'fromClose' == true indicates the call is from
    // the stage's close() method, so it should also try to shrink its memory footprint.
    void reset(bool fromClose);

    PlanStage* outerChild() const {
        return _children[0].get();
    }
    PlanStage* innerChild() const {
        return _children[1].get();
    }

    const value::SlotId _outerKeySlot;
    const value::SlotId _innerKeySlot;
    const value::SlotId _innerProjectSlot;
    const value::SlotId _lookupStageOutputSlot;
    const boost::optional<value::SlotId> _collatorSlot;

    value::SlotAccessor* _inOuterMatchAccessor{nullptr};
    value::SlotAccessor* _inInnerMatchAccessor{nullptr};
    value::SlotAccessor* _inInnerProjectAccessor{nullptr};

    // Accessor for collator. Only set if collatorSlot provided during construction.
    value::SlotAccessor* _collatorAccessor{nullptr};

    // Output row of one column containing the lookup-unwind's "as" result.
    value::MaterializedRow _lookupStageOutput;
    value::MaterializedSingleRowAccessor _lookupStageOutputAccessor{_lookupStageOutput,
                                                                    0 /* column */};

    // LookupHashTable instance holding the inner collection.
    LookupHashTable _hashTable;
    // Tracks whether we are already processing an outer key.
    bool _outerKeyOpen{false};

    void doForceSpill() final {
        _hashTable.forceSpill();
        _memoryTracker.value().set(_hashTable.getMemUsage());
    };
};  // class HashLookupUnwindStage
}  // namespace mongo::sbe
