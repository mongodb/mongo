// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/util/spilling.h"
#include "mongo/db/exec/sbe/values/row.h"
#include "mongo/db/query/plan_yield_policy_sbe.h"
#include "mongo/db/query/query_execution_knobs_gen.h"
#include "mongo/db/query/query_integration_knobs_gen.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>
#include <utility>

namespace mongo {
namespace sbe {

/**
 * The hash table '_ht' contains a map from $group keys to the current accumulator states for
 * all accumulators of that $group stage. The first MaterializedRow in SpilledRow and TableType
 * contains a key and the second contains the state values.
 */
using TableType = stdx::unordered_map<value::MaterializedRow,
                                      value::MaterializedRow,
                                      value::MaterializedRowHasher,
                                      value::MaterializedRowEq>;

using HashKeyAccessor = value::MaterializedRowKeyAccessor<TableType::iterator>;
using HashAggAccessor = value::MaterializedRowValueAccessor<TableType::iterator>;

template <class Derived>
class HashAggBaseStage : public PlanStage {
protected:
    HashAggBaseStage(std::string_view stageName,
                     PlanYieldPolicySBE* yieldPolicy,
                     PlanNodeId planNodeId,
                     value::SlotAccessor* _collatorAccessor,
                     bool participateInTrialRunTracking,
                     bool allowDiskUse,
                     bool forceIncreasedSpilling);

    void doSaveState() override;
    void doRestoreState() override;
    void doDetachFromOperationContext() override;
    void doAttachToOperationContext(OperationContext* opCtx) override;

    using SpilledRow = std::pair<value::MaterializedRow, value::MaterializedRow>;

    /**
     * We check amount of used memory every T processed incoming records, where T is calculated
     * based on the estimated used memory and its recent growth. When the memory limit is exceeded,
     * 'checkMemoryUsageAndSpillIfNecessary()' will create '_recordStore' (if it hasn't already been
     * created) and spill the contents of the hash table into this record store.
     */
    struct MemoryCheckData {
        MemoryCheckData() {
            reset();
        }

        void reset() {
            memoryCheckFrequency = std::min(atMostCheckFrequency, atLeastMemoryCheckFrequency);
            nextMemoryCheckpoint = 0;
            memoryCheckpointCounter = 0;
        }

        const double checkpointMargin = internalQuerySBEAggMemoryUseCheckMargin.load();
        const int64_t atMostCheckFrequency = internalQuerySBEAggMemoryCheckPerAdvanceAtMost.load();
        const int64_t atLeastMemoryCheckFrequency =
            internalQuerySBEAggMemoryCheckPerAdvanceAtLeast.load();

        // The check frequency upper bound, which start at 'atMost' and exponentially backs off
        // to 'atLeast' as more data is accumulated. If 'atLeast' is less than 'atMost', the memory
        // checks will be done every 'atLeast' incoming records.
        int64_t memoryCheckFrequency = 1;

        // The number of incoming records to process before the next memory checkpoint.
        int64_t nextMemoryCheckpoint = 0;

        // The counter of the incoming records between memory checkpoints.
        int64_t memoryCheckpointCounter = 0;
    };

    /**
     * Given a 'record' from the record store, decodes it into a pair of materialized rows (one for
     * the group-by keys and another for the agg values).
     *
     * The given 'keyBuffer' is cleared, and then used to hold data (e.g. long strings and other
     * values that can't be inlined) obtained by decoding the 'RecordId' keystring to a
     * 'MaterializedRow'. The values in the resulting 'MaterializedRow' may be pointers into
     * 'keyBuffer', so it is important that 'keyBuffer' outlive the row.
     *
     * This method is used when there is no collator.
     */
    SpilledRow deserializeSpilledRecord(const Record& record,
                                        size_t numKeys,
                                        BufBuilder& keyBuffer);

    void makeInternalRecordStore();

    /**
     * Inserts a key and value pair to the '_recordStore'. They key is serialized to a
     * 'key_string::Value' which becomes the 'RecordId'.
     *
     * Note that the 'typeBits' are needed to reconstruct the spilled 'key' to a 'MaterializedRow',
     * but are not necessary for comparison purposes. Therefore, we carry the type bits separately
     * from the record id, instead appending them to the end of the serialized 'val' buffer.
     *
     * Returns the size in bytes of the record that is spilled to disk.
     */
    int64_t spillRowToDisk(const value::MaterializedRow& key, const value::MaterializedRow& val);
    void spill(MemoryCheckData& mcd);
    void checkMemoryUsageAndSpillIfNecessary(MemoryCheckData& mcd);

    void doForceSpill() final;

    // Hash table where we'll map groupby key to the accumulators.
    boost::optional<TableType> _ht;
    TableType::iterator _htIt;

    // Only set if collator slot provided on construction.
    value::SlotAccessor* _collatorAccessor = nullptr;

    const bool _allowDiskUse;
    // When true, we spill frequently without reaching the memory limit. This allows us to exercise
    // the spilling logic more often in test contexts.
    const bool _forceIncreasedSpilling;

    // A record store which is instantiated and written to in the case of spilling.
    std::unique_ptr<SpillingStore> _recordStore;
    std::unique_ptr<SpillTable::Cursor> _rsCursor;

    // A monotonically increasing counter used to ensure uniqueness of 'RecordId' values. When
    // spilling, the key is encoding into the 'RecordId' of the '_recordStore'. Record ids must be
    // unique by definition, but we might end up spilling multiple partial aggregates for the same
    // key. We ensure uniqueness by appending a unique integer to the end of this key, which is
    // simply ignored during deserialization.
    int64_t _ridSuffixCounter = 0;

private:
    void spill();

    Derived& derived() {
        return static_cast<Derived&>(*this);
    }
};

}  // namespace sbe
}  // namespace mongo
