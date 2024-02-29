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

#include <absl/container/inlined_vector.h>
#include <boost/optional/optional.hpp>
#include <cstddef>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/exec/field_name_bloom_filter.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/expressions/runtime_environment.h"
#include "mongo/db/exec/sbe/stages/collection_helpers.h"
#include "mongo/db/exec/sbe/stages/plan_stats.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/trial_run_tracker.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/stage_types.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/mutex.h"
#include "mongo/util/string_listset.h"
#include "mongo/util/string_map.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace sbe {
using ScanOpenCallback = void (*)(OperationContext*, const CollectionPtr&);

struct ScanCallbacks {
    ScanCallbacks(IndexKeyCorruptionCheckCallback indexKeyCorruptionCheck = nullptr,
                  IndexKeyConsistencyCheckCallback indexKeyConsistencyCheck = nullptr,
                  ScanOpenCallback scanOpen = nullptr)
        : indexKeyCorruptionCheckCallback(std::move(indexKeyCorruptionCheck)),
          indexKeyConsistencyCheckCallback(std::move(indexKeyConsistencyCheck)),
          scanOpenCallback(std::move(scanOpen)) {}

    IndexKeyCorruptionCheckCallback indexKeyCorruptionCheckCallback = nullptr;
    IndexKeyConsistencyCheckCallback indexKeyConsistencyCheckCallback = nullptr;
    ScanOpenCallback scanOpenCallback = nullptr;
};

/**
 * Contains static state info that can be shared across all cloned copies of a ScanStage.
 */
class ScanStageState {
public:
    ScanStageState(UUID inCollUuid,
                   boost::optional<value::SlotId> inRecordSlot,
                   boost::optional<value::SlotId> inRecordIdSlot,
                   boost::optional<value::SlotId> inSnapshotIdSlot,
                   boost::optional<value::SlotId> inIndexIdentSlot,
                   boost::optional<value::SlotId> inIndexKeySlot,
                   boost::optional<value::SlotId> inIndexKeyPatternSlot,
                   boost::optional<value::SlotId> inOplogTsSlot,
                   std::vector<std::string> inScanFieldNames,
                   value::SlotVector inScanFieldSlots,
                   boost::optional<value::SlotId> inSeekRecordIdSlot,
                   boost::optional<value::SlotId> inMinRecordIdSlot,
                   boost::optional<value::SlotId> inMaxRecordIdSlot,
                   bool inForward,
                   ScanCallbacks inScanCallbacks,
                   bool inUseRandomCursor)
        : collUuid(inCollUuid),
          recordSlot(inRecordSlot),
          recordIdSlot(inRecordIdSlot),
          snapshotIdSlot(inSnapshotIdSlot),
          indexIdentSlot(inIndexIdentSlot),
          indexKeySlot(inIndexKeySlot),
          indexKeyPatternSlot(inIndexKeyPatternSlot),
          oplogTsSlot(inOplogTsSlot),
          scanFieldNames(inScanFieldNames),
          scanFieldSlots(inScanFieldSlots),
          seekRecordIdSlot(inSeekRecordIdSlot),
          minRecordIdSlot(inMinRecordIdSlot),
          maxRecordIdSlot(inMaxRecordIdSlot),
          forward(inForward),
          scanCallbacks(inScanCallbacks),
          useRandomCursor(inUseRandomCursor) {
        invariant(scanFieldNames.size() == scanFieldSlots.size());
    }

    inline size_t getNumScanFields() {
        return scanFieldNames.size();
    }

    const UUID collUuid;

    const boost::optional<value::SlotId> recordSlot;
    const boost::optional<value::SlotId> recordIdSlot;
    const boost::optional<value::SlotId> snapshotIdSlot;
    const boost::optional<value::SlotId> indexIdentSlot;
    const boost::optional<value::SlotId> indexKeySlot;
    const boost::optional<value::SlotId> indexKeyPatternSlot;
    const boost::optional<value::SlotId> oplogTsSlot;

    // 'scanFieldNames' - names of the fields being scanned from the doc
    // 'scanFieldSlots' - slot IDs for the fields being scanned from the doc
    const StringListSet scanFieldNames;
    const value::SlotVector scanFieldSlots;

    const boost::optional<value::SlotId> seekRecordIdSlot;
    const boost::optional<value::SlotId> minRecordIdSlot;
    const boost::optional<value::SlotId> maxRecordIdSlot;

    // Tells if this is a forward (as opposed to reverse) scan.
    const bool forward;

    const ScanCallbacks scanCallbacks;

    // Used to return a random sample of the collection.
    const bool useRandomCursor;
};  // class ScanStageState

/**
 * Retrieves documents from the collection with the given 'collUuid' using the storage API.
 *
 * Iff resuming a prior scan, this stage is given a 'seekRecordIdSlot' from which to read a
 * 'RecordId'. We seek to this 'RecordId' before resuming the scan. 'stageType' is set to "seek"
 * instead of "scan" for this case only.
 *
 * If the 'recordSlot' is provided, then each of the records returned from the scan is placed into
 * an output slot with this slot id. Similarly, if 'recordIdSlot' is provided, then this slot is
 * populated with the record id on each advance.
 *
 * In addition, the scan/seek can extract a set of top-level fields from each document. The caller
 * asks for this by passing a vector of 'scanFieldNames', along with a corresponding slot vector
 * 'scanFieldSlots' into which the resulting values should be stored. These vectors must have the
 * same length.
 *
 * The direction of the scan is controlled by the 'forward' parameter.
 *
 * If this scan is acting as a seek used to obtain the record assocated with a particular record id,
 * then a set of special slots will be provided. In this scenario, we need to detect whether a yield
 * has caused the storage snapshot to advance since the index key was obtained from storage. When
 * the snapshot has indeed advanced, the key may no longer be consistent with the 'RecordStore' and
 * we must verify at runtime that no such inconsistency exists. This requires the scan to know the
 * value of the index key, the identity of the index from which it was obtained, and the id of the
 * storage snapshot from which it was obtained. This information is made available to the seek stage
 * via 'snapshotIdSlot', 'indexIdentSlot', 'indexKeySlot', and 'indexKeyPatternSlot'.
 *
 * For oplog scans, 'oplogTsSlot' will be populated with a copy of the "ts" field (which is the
 * oplog clustering key) from the doc if it is a clustered scan (for use by the EOF filter above the
 * scan) or the caller asked for the latest oplog "ts" value.
 *
 * Debug string representations:
 *
 *  scan recordSlot? recordIdSlot? snapshotIdSlot? indexIdentSlot? indexKeySlot?
 *       indexKeyPatternSlot? minRecordIdSlot? maxRecordIdSlot? [slot1 = fieldName1, ...
 *       slot_n = fieldName_n] collUuid forward needOplogSlotForTs
 *
 *  seek seekKeySlot recordSlot? recordIdSlot? snapshotIdSlot? indexIdentSlot? indexKeySlot?
 *       indexKeyPatternSlot? minRecordIdSlot? maxRecordIdSlot? [slot1 = fieldName1, ...
 *       slot_n = fieldName_n] collUuid forward needOplogSlotForTs
 */
class ScanStage final : public PlanStage {
public:
    /**
     * Regular constructor. Initializes static '_state' managed by a shared_ptr.
     */
    ScanStage(UUID collUuid,
              boost::optional<value::SlotId> recordSlot,
              boost::optional<value::SlotId> recordIdSlot,
              boost::optional<value::SlotId> snapshotIdSlot,
              boost::optional<value::SlotId> indexIdentSlot,
              boost::optional<value::SlotId> indexKeySlot,
              boost::optional<value::SlotId> indexKeyPatternSlot,
              boost::optional<value::SlotId> oplogTsSlot,
              std::vector<std::string> scanFieldNames,
              value::SlotVector scanFieldSlots,
              boost::optional<value::SlotId> seekRecordIdSlot,
              boost::optional<value::SlotId> minRecordIdSlot,
              boost::optional<value::SlotId> maxRecordIdSlot,
              bool forward,
              PlanYieldPolicy* yieldPolicy,
              PlanNodeId nodeId,
              ScanCallbacks scanCallbacks,
              // Optional arguments:
              bool lowPriority = false,
              bool useRandomCursor = false,
              bool participateInTrialRunTracking = true,
              bool includeScanStartRecordId = true,
              bool includeScanEndRecordId = true);

    /**
     * Constructor for clone(). Copies '_state' shared_ptr.
     */
    ScanStage(const std::shared_ptr<ScanStageState>& state,
              PlanYieldPolicy* yieldPolicy,
              PlanNodeId nodeId,
              bool lowPriority,
              bool participateInTrialRunTracking,
              bool includeScanStartRecordId,
              bool includeScanEndRecordId);

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
    // Returns the primary cursor or the random cursor depending on whether _useRandomCursor is set.
    RecordCursor* getActiveCursor() const;

    /**
     * Resets the state data members for starting the scan in the 'reOpen' case, i.e. skipping state
     * that would be correct after a prior open() call that was NOT followed by a close() call. This
     * is also called by the initial open() to set the same subset of state for the first time to
     * avoid duplicating this code.
     */
    void scanResetState(bool reOpen);

    // Only for a resumed scan ("seek"), this sets '_seekRecordId' to the resume point at runtime.
    void setSeekRecordId();

    // Only for a clustered collection scan, this sets '_minRecordId' to the lower scan bound.
    void setMinRecordId();

    // Only for a clustered collection scan, this sets '_maxRecordId' to the upper scan bound.
    void setMaxRecordId();

    MONGO_COMPILER_ALWAYS_INLINE
    value::OwnedValueAccessor* getFieldAccessor(StringData name) {
        if (size_t pos = _state->scanFieldNames.findPos(name); pos != StringListSet::npos) {
            return &_scanFieldAccessors[pos];
        }
        return nullptr;
    }

    // Contains unchanging state that will be shared across clones instead of copied.
    const std::shared_ptr<ScanStageState> _state;

    // Holds the current record.
    value::OwnedValueAccessor _recordAccessor;

    // Holds the RecordId of the current record as a TypeTags::RecordId.
    value::OwnedValueAccessor _recordIdAccessor;
    RecordId _recordId;

    value::SlotAccessor* _snapshotIdAccessor{nullptr};
    value::SlotAccessor* _indexIdentAccessor{nullptr};
    value::SlotAccessor* _indexKeyAccessor{nullptr};
    value::SlotAccessor* _indexKeyPatternAccessor{nullptr};

    // For oplog scans only, holds a copy of the "ts" field of the record (which is the oplog
    // clustering key) for use by the end-bound EOF filter above the scan, if applicable.
    RuntimeEnvironment::Accessor* _oplogTsAccessor{nullptr};

    // For oplog scans only, holds a cached pointer to the accessor for the "ts" field in the
    // current document to get this accessor quickly rather than having to look it up in the
    // '_scanFieldAccessors' hashtable each time.
    value::SlotAccessor* _tsFieldAccessor{nullptr};

    // These members hold info about the target fields being scanned from the record.
    //     '_scanFieldAccessors' - slot accessors corresponding, by index, to _state->scanFieldNames
    //     '_scanFieldAccessorsMap' - a map from vector index to pointer to the corresponding
    //         accessor in '_scanFieldAccessors'
    absl::InlinedVector<value::OwnedValueAccessor, 4> _scanFieldAccessors;
    value::SlotAccessorMap _scanFieldAccessorsMap;

    // Only for a resumed scan ("seek"). Slot holding the TypeTags::RecordId of the record to resume
    // the scan from. '_seekRecordId' is the RecordId value, initialized from the slot at runtime.
    value::SlotAccessor* _seekRecordIdAccessor{nullptr};
    RecordId _seekRecordId;

    // Only for clustered collection scans, holds the minimum record ID of the scan, if applicable.
    value::SlotAccessor* _minRecordIdAccessor{nullptr};
    RecordId _minRecordId;

    // Only for clustered collection scans, holds the maximum record ID of the scan, if applicable.
    value::SlotAccessor* _maxRecordIdAccessor{nullptr};
    RecordId _maxRecordId;

    // Only for clustered collection scans: must ScanStage::getNext() include the starting bound?
    bool _includeScanStartRecordId = true;

    // Only for clustered collection scans: must ScanStage::getNext() include the ending bound?
    bool _includeScanEndRecordId = true;

    // Only for clustered collection scans: does the scan have an end bound?
    bool _hasScanEndRecordId = false;

    // Only for clustered collection scans: have we crossed the scan end bound if there is one?
    bool _havePassedScanEndRecordId = false;

    CollectionRef _coll;

    // If provided, used during a trial run to accumulate certain execution stats. Once the trial
    // run is complete, this pointer is reset to nullptr.
    TrialRunTracker* _tracker{nullptr};

    bool _open{false};
    std::unique_ptr<SeekableRecordCursor> _cursor;

    // TODO: SERVER-62647. Consider removing random cursor when no longer needed.
    std::unique_ptr<RecordCursor> _randomCursor;

    // Tells whether this is the first getNext() call of the scan or after restarting.
    bool _firstGetNext{false};

    // Whether the scan should have low storage admission priority.
    bool _lowPriority;
    boost::optional<ScopedAdmissionPriorityForLock> _priority;

    ScanStats _specificStats;

    // Flag set upon restoring the stage that indicates whether the cursor's position in the
    // collection is still valid. Only relevant to capped collections.
    bool _needsToCheckCappedPositionLost = false;

    StringMap<const IndexCatalogEntry*> _indexCatalogEntryMap;

#if defined(MONGO_CONFIG_DEBUG_BUILD)
    // Debug-only buffer used to track the last thing returned from the stage. Between
    // saves/restores this is used to check that the storage cursor has not changed position.
    std::vector<char> _lastReturned;
#endif
};  // class ScanStage

class ParallelScanStage final : public PlanStage {
    struct Range {
        RecordId begin;
        RecordId end;
    };
    struct ParallelState {
        Mutex mutex = MONGO_MAKE_LATCH("ParallelScanStage::ParallelState::mutex");
        std::vector<Range> ranges;
        AtomicWord<size_t> currentRange{0};
    };

public:
    ParallelScanStage(UUID collUuid,
                      boost::optional<value::SlotId> recordSlot,
                      boost::optional<value::SlotId> recordIdSlot,
                      boost::optional<value::SlotId> snapshotIdSlot,
                      boost::optional<value::SlotId> indexIdentSlot,
                      boost::optional<value::SlotId> indexKeySlot,
                      boost::optional<value::SlotId> indexKeyPatternSlot,
                      std::vector<std::string> scanFieldNames,
                      value::SlotVector scanFieldSlots,
                      PlanYieldPolicy* yieldPolicy,
                      PlanNodeId nodeId,
                      ScanCallbacks callbacks,
                      // Optional arguments:
                      bool participateInTrialRunTracking = true);

    ParallelScanStage(const std::shared_ptr<ParallelState>& state,
                      UUID collUuid,
                      boost::optional<value::SlotId> recordSlot,
                      boost::optional<value::SlotId> recordIdSlot,
                      boost::optional<value::SlotId> snapshotIdSlot,
                      boost::optional<value::SlotId> indexIdentSlot,
                      boost::optional<value::SlotId> indexKeySlot,
                      boost::optional<value::SlotId> indexKeyPatternSlot,
                      std::vector<std::string> scanFieldNames,
                      value::SlotVector scanFieldSlots,
                      PlanYieldPolicy* yieldPolicy,
                      PlanNodeId nodeId,
                      ScanCallbacks callbacks,
                      // Optional arguments:
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
    void doSaveState(bool fullSave) final;
    void doRestoreState(bool fullSave) final;
    void doDetachFromOperationContext() final;
    void doAttachToOperationContext(OperationContext* opCtx) final;

private:
    boost::optional<Record> nextRange();
    bool needsRange() const {
        return _currentRange == std::numeric_limits<std::size_t>::max();
    }
    void setNeedsRange() {
        _currentRange = std::numeric_limits<std::size_t>::max();
    }

    value::OwnedValueAccessor* getFieldAccessor(StringData name);

    const std::shared_ptr<ParallelState> _state;

    const UUID _collUuid;

    const boost::optional<value::SlotId> _recordSlot;
    const boost::optional<value::SlotId> _recordIdSlot;
    const boost::optional<value::SlotId> _snapshotIdSlot;
    const boost::optional<value::SlotId> _indexIdentSlot;
    const boost::optional<value::SlotId> _indexKeySlot;
    const boost::optional<value::SlotId> _indexKeyPatternSlot;

    // '_scanFieldNames' - names of the fields being scanned from the doc
    // '_scanFieldSlots' - slot IDs corresponding, by index, to _scanFieldAccessors
    const StringListSet _scanFieldNames;
    const value::SlotVector _scanFieldSlots;

    const ScanCallbacks _scanCallbacks;

    // Holds the current record.
    value::OwnedValueAccessor _recordAccessor;

    // Holds the RecordId of the current record as a TypeTags::RecordId.
    value::OwnedValueAccessor _recordIdAccessor;
    RecordId _recordId;

    value::SlotAccessor* _snapshotIdAccessor{nullptr};
    value::SlotAccessor* _indexIdentAccessor{nullptr};
    value::SlotAccessor* _indexKeyAccessor{nullptr};
    value::SlotAccessor* _indexKeyPatternAccessor{nullptr};

    // These members hold info about the target fields being scanned from the record.
    //     '_scanFieldAccessors' - slot accessors corresponding, by index, to _scanFieldNames
    //     '_scanFieldAccessorsMap' - a map from vector index to pointer to the corresponding
    //         accessor in '_scanFieldAccessors'
    absl::InlinedVector<value::OwnedValueAccessor, 4> _scanFieldAccessors;
    value::SlotAccessorMap _scanFieldAccessorsMap;

    CollectionRef _coll;

    size_t _currentRange{std::numeric_limits<std::size_t>::max()};
    Range _range;

    bool _open{false};

    std::unique_ptr<SeekableRecordCursor> _cursor;

    StringMap<const IndexCatalogEntry*> _indexCatalogEntryMap;

#if defined(MONGO_CONFIG_DEBUG_BUILD)
    // Debug-only buffer used to track the last thing returned from the stage. Between
    // saves/restores this is used to check that the storage cursor has not changed position.
    std::vector<char> _lastReturned;
#endif
};  // class ParallelScanStage
}  // namespace sbe
}  // namespace mongo
