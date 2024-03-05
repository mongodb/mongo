/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include <memory>
#include <utility>

#include "mongo/db/exec/sbe/values/row.h"
#include "mongo/db/exec/sbe/values/value.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/util/spilling.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/query_knobs_gen.h"

namespace mongo {
namespace sbe {

template <class Derived>
class HashAggBaseStage : public PlanStage {
protected:
    HashAggBaseStage(StringData stageName,
                     PlanYieldPolicy* yieldPolicy,
                     PlanNodeId planNodeId,
                     value::SlotAccessor* _collatorAccessor,
                     bool participateInTrialRunTracking,
                     bool allowDiskUse,
                     bool forceIncreasedSpilling);

    void doSaveState(bool relinquishCursor) override;
    void doRestoreState(bool relinquishCursor) override;
    void doDetachFromOperationContext() override;
    void doAttachToOperationContext(OperationContext* opCtx) override;

    void doDetachFromTrialRunTracker() override;
    PlanStage::TrialRunTrackerAttachResultMask doAttachToTrialRunTracker(
        TrialRunTracker* tracker,
        PlanStage::TrialRunTrackerAttachResultMask childrenAttachResult) override;

    using SpilledRow = std::pair<value::MaterializedRow, value::MaterializedRow>;
    using TableType = stdx::unordered_map<value::MaterializedRow,
                                          value::MaterializedRow,
                                          value::MaterializedRowHasher,
                                          value::MaterializedRowEq>;

    using HashKeyAccessor = value::MaterializedRowKeyAccessor<TableType::iterator>;
    using HashAggAccessor = value::MaterializedRowValueAccessor<TableType::iterator>;

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
            lastEstimatedMemoryUsage = 0;
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

        int64_t lastEstimatedMemoryUsage = 0;
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

    void makeTemporaryRecordStore();

    /**
     * Inserts a key and value pair to the '_recordStore'. They key is serialized to a
     * 'key_string::Value' which becomes the 'RecordId'.
     *
     * Note that the 'typeBits' are needed to reconstruct the spilled 'key' to a 'MaterializedRow',
     * but are not necessary for comparison purposes. Therefore, we carry the type bits separately
     * from the record id, instead appending them to the end of the serialized 'val' buffer.
     */
    void spillRowToDisk(const value::MaterializedRow& key, const value::MaterializedRow& val);
    void spill(MemoryCheckData& mcd);
    void checkMemoryUsageAndSpillIfNecessary(MemoryCheckData& mcd, bool emptyInKeyAccessors);

    // Memory tracking and spilling to disk.
    const long long _approxMemoryUseInBytesBeforeSpill =
        internalQuerySBEAggApproxMemoryUseInBytesBeforeSpill.load();

    // Hash table where we'll map groupby key to the accumulators.
    boost::optional<TableType> _ht;
    TableType::iterator _htIt;

    // Only set if collator slot provided on construction.
    value::SlotAccessor* _collatorAccessor = nullptr;

    // If provided, used during a trial run to accumulate certain execution stats. Once the trial
    // run is complete, this pointer is reset to nullptr.
    TrialRunTracker* _tracker{nullptr};

    const bool _allowDiskUse;
    // When true, we spill frequently without reaching the memory limit. This allows us to exercise
    // the spilling logic more often in test contexts.
    const bool _forceIncreasedSpilling;

    // A record store which is instantiated and written to in the case of spilling.
    std::unique_ptr<SpillingStore> _recordStore;
    std::unique_ptr<SeekableRecordCursor> _rsCursor;

    // A monotically increasing counter used to ensure uniqueness of 'RecordId' values. When
    // spilling, the key is encoding into the 'RecordId' of the '_recordStore'. Record ids must be
    // unique by definition, but we might end up spilling multiple partial aggregates for the same
    // key. We ensure uniqueness by appending a unique integer to the end of this key, which is
    // simply ignored during deserialization.
    int64_t _ridSuffixCounter = 0;
};

}  // namespace sbe
}  // namespace mongo
