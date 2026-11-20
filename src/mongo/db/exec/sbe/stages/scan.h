// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/admission/execution_control/execution_admission_context.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/stages/plan_stats.h"
#include "mongo/db/exec/sbe/stages/scan_helpers.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/plan_yield_policy_sbe.h"
#include "mongo/db/record_id.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"
#include "mongo/util/string_listset.h"
#include "mongo/util/string_map.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <absl/container/inlined_vector.h>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace sbe {

using ScanOpenCallback = void (*)(OperationContext*, const CollectionPtr&);

template <typename Derived>
class ScanStageBaseImpl;

/**
 * Contains static state info that can be shared across all cloned copies of a ScanStageBase.
 */
class ScanStageBaseState {
public:
    ScanStageBaseState(UUID inCollUuid,
                       DatabaseName dbName,
                       boost::optional<value::SlotId> inRecordSlot,
                       boost::optional<value::SlotId> inRecordIdSlot,
                       boost::optional<value::SlotId> inSnapshotIdSlot,
                       boost::optional<value::SlotId> inIndexIdentSlot,
                       boost::optional<value::SlotId> inIndexKeySlot,
                       boost::optional<value::SlotId> inIndexKeyPatternSlot,
                       std::vector<std::string> inScanFieldNames,
                       value::SlotVector inScanFieldSlots,
                       ScanOpenCallback inScanOpenCallback,
                       bool forward)
        : collUuid(inCollUuid),
          dbName(dbName),
          recordSlot(inRecordSlot),
          recordIdSlot(inRecordIdSlot),
          snapshotIdSlot(inSnapshotIdSlot),
          indexIdentSlot(inIndexIdentSlot),
          indexKeySlot(inIndexKeySlot),
          indexKeyPatternSlot(inIndexKeyPatternSlot),
          scanFieldNames(inScanFieldNames),
          scanFieldSlots(inScanFieldSlots),
          scanOpenCallback(inScanOpenCallback),
          forward(forward) {
        tassert(11094712,
                "Expecting number of scan fields to match the number of scan slots",
                scanFieldNames.size() == scanFieldSlots.size());
    }

    inline size_t getNumScanFields() {
        return scanFieldNames.size();
    }

    const UUID collUuid;
    const DatabaseName dbName;

    const boost::optional<value::SlotId> recordSlot;
    const boost::optional<value::SlotId> recordIdSlot;
    const boost::optional<value::SlotId> snapshotIdSlot;
    const boost::optional<value::SlotId> indexIdentSlot;
    const boost::optional<value::SlotId> indexKeySlot;
    const boost::optional<value::SlotId> indexKeyPatternSlot;

    // 'scanFieldNames' - names of the fields being scanned from the doc
    // 'scanFieldSlots' - slot IDs for the fields being scanned from the doc
    const StringListSet scanFieldNames;
    const value::SlotVector scanFieldSlots;

    const ScanOpenCallback scanOpenCallback;

    // Tells if this is a forward (as opposed to reverse) scan.
    const bool forward;
};  // class ScanStageBaseState

/**
 * Retrieves documents from the collection with the given 'collUuid' using the storage API.
 *
 * If the 'recordSlot' is provided, then each of the records returned from the scan is placed into
 * an output slot with this slot id. Similarly, if 'recordIdSlot' is provided, then this slot is
 * populated with the record id on each advance.
 *
 * In addition, the scan can extract a set of top-level fields from each document. The caller
 * asks for this by passing a vector of 'scanFieldNames', along with a corresponding slot vector
 * 'scanFieldSlots' into which the resulting values should be stored. These vectors must have the
 * same length.
 *
 * The direction of the scan is controlled by the 'forward' parameter.
 *
 * Debug string representations:
 *
 *  scan recordSlot? recordIdSlot? snapshotIdSlot? indexIdentSlot? indexKeySlot?
 *       indexKeyPatternSlot? minRecordIdSlot? maxRecordIdSlot? [slot1 = fieldName1, ...
 *       slot_n = fieldName_n] collUuid forward
 */
class ScanStageBase : public PlanStage {
public:
    void getStatsShared(BSONObjBuilder& bob) const;
    const SpecificStats* getSpecificStats() const final;
    size_t estimateCompileTimeSize() const final;

protected:
    /**
     * Regular constructor. Initializes static '_state' managed by a shared_ptr.
     * Should only be able to be called by ScanStageBaseImpl.
     */
    ScanStageBase(UUID collUuid,
                  DatabaseName dbName,
                  boost::optional<value::SlotId> recordSlot,
                  boost::optional<value::SlotId> recordIdSlot,
                  boost::optional<value::SlotId> snapshotIdSlot,
                  boost::optional<value::SlotId> indexIdentSlot,
                  boost::optional<value::SlotId> indexKeySlot,
                  boost::optional<value::SlotId> indexKeyPatternSlot,
                  std::vector<std::string> scanFieldNames,
                  value::SlotVector scanFieldSlots,
                  PlanYieldPolicySBE* yieldPolicy,
                  PlanNodeId nodeId,
                  ScanOpenCallback scanOpenCallback,
                  bool forward,
                  // Optional arguments:
                  bool participateInTrialRunTracking = true);

    /**
     * Constructor for clone(). Copies '_state' shared_ptr.
     */
    ScanStageBase(std::shared_ptr<ScanStageBaseState> state,
                  PlanYieldPolicySBE* yieldPolicy,
                  PlanNodeId nodeId,
                  bool participateInTrialRunTracking);

    value::SlotAccessor* getAccessor(CompileCtx& ctx, value::SlotId slot) final;
    void closeShared();
    void debugPrintShared(std::vector<DebugPrinter::Block>& ret) const;
    // Shared logic for getNext()
    inline void handleInterruptAndSlotAccess() {
        // We are about to call next() on a storage cursor so do not bother saving our internal
        // state in case it yields as the state will be completely overwritten after the next()
        // call.
        disableSlotAccess();

        // This call to checkForInterrupt() may result in a call to save() or restore() on the
        // entire PlanStage tree if a yield occurs.
        checkForInterruptAndYield(_opCtx);
    };

    // Shared logic for getNext()
    inline void handleEOF() {
        if (_state->recordIdSlot) {
            _recordIdAccessor.reset(
                value::TagValueOwned::fromRaw(sbe::value::makeCopyRecordId(RecordId())));
        }
    };

    // Shared logic for getNext()
    // Helper to reset record ID if a `recordIdSlot` is present and to track end bounds.
    inline void resetRecordId(const boost::optional<Record>& nextRecord) {
        if (_state->recordSlot) {
            _recordAccessor.reset(
                value::TagValueView{value::TypeTags::bsonObject,
                                    value::bitcastFrom<const char*>(nextRecord->data.data())});
        }
    };

    MONGO_COMPILER_ALWAYS_INLINE
    value::OwnedValueAccessor* getFieldAccessor(std::string_view name) {
        if (size_t pos = _state->scanFieldNames.findPos(name); pos != StringListSet::npos) {
            return &_scanFieldAccessors[pos];
        }
        return nullptr;
    }

    /**
     * This contains logic shared between ScanStage
     * RandomScanStage.
     */
    void prepareShared(CompileCtx& ctx);

    // Contains unchanging state that will be shared across clones instead of copied.
    const std::shared_ptr<ScanStageBaseState> _state;

    // Holds the current record. A BSONObjValueAccessor lets consumers share one owned copy of the
    // record via getOwnedBSONObj() instead of copying it each time.
    value::BSONObjValueAccessor _recordAccessor;

    // Holds the RecordId of the current record as a TypeTags::RecordId.
    value::OwnedValueAccessor _recordIdAccessor;
    RecordId _recordId;

    value::SlotAccessor* _snapshotIdAccessor{nullptr};
    value::SlotAccessor* _indexIdentAccessor{nullptr};
    value::SlotAccessor* _indexKeyAccessor{nullptr};
    value::SlotAccessor* _indexKeyPatternAccessor{nullptr};

    // These members hold info about the target fields being scanned from the record.
    //     '_scanFieldAccessors' - slot accessors corresponding, by index, to _state->scanFieldNames
    //     '_scanFieldAccessorsMap' - a map from vector index to pointer to the corresponding
    //         accessor in '_scanFieldAccessors'
    absl::InlinedVector<value::OwnedValueAccessor, 4> _scanFieldAccessors;
    value::SlotAccessorMap _scanFieldAccessorsMap;

    boost::optional<CollectionAcquisition> _coll;

    bool _open{false};

    ScanStats _specificStats;

    StringMap<const IndexCatalogEntry*> _indexCatalogEntryMap;

#if defined(MONGO_CONFIG_DEBUG_BUILD)
    // Debug-only buffer used to track the last thing returned from the stage. Between
    // saves/restores this is used to check that the storage cursor has not changed position.
    std::vector<char> _lastReturned;
#endif
};  // class ScanStageBase

template <typename Derived>
class ScanStageBaseImpl : public ScanStageBase {

public:
    /**
     * Regular constructor. Initializes static '_state' managed by a shared_ptr.
     */
    ScanStageBaseImpl(UUID collUuid,
                      DatabaseName dbName,
                      boost::optional<value::SlotId> recordSlot,
                      boost::optional<value::SlotId> recordIdSlot,
                      boost::optional<value::SlotId> snapshotIdSlot,
                      boost::optional<value::SlotId> indexIdentSlot,
                      boost::optional<value::SlotId> indexKeySlot,
                      boost::optional<value::SlotId> indexKeyPatternSlot,
                      std::vector<std::string> scanFieldNames,
                      value::SlotVector scanFieldSlots,
                      PlanYieldPolicySBE* yieldPolicy,
                      PlanNodeId nodeId,
                      ScanOpenCallback scanOpenCallback,
                      bool forward,
                      // Optional arguments:
                      bool participateInTrialRunTracking = true);

    /**
     * Constructor for clone(). Copies '_state' shared_ptr.
     */
    ScanStageBaseImpl(std::shared_ptr<ScanStageBaseState> state,
                      PlanYieldPolicySBE* yieldPolicy,
                      PlanNodeId nodeId,
                      bool participateInTrialRunTracking);
    void open(bool reOpen) final;

protected:
    void doSaveState() override;
    void doRestoreState() override;
    void doDetachFromOperationContext() override;
    void doAttachToOperationContext(OperationContext* opCtx) override;
    PlanState getNext() final;

private:
    /**
     * Required self() functions for CRTP
     */
    inline constexpr Derived* self() noexcept {
        return static_cast<Derived*>(this);
    }
    inline constexpr const Derived* self() const noexcept {
        return static_cast<const Derived*>(this);
    }
};

/**
 * The record id ranges a set of parallel ScanStage clones divide the collection into, shared by all
 * of them. Rather than statically assigning one range per clone, each clone pulls the next range
 * out of here whenever it has exhausted the one it was working on, so that a range that happens to
 * hold many more records than the others does not put the whole scan on one thread's critical path.
 *
 * The ranges are half open, '[lower, higher)', and are handed out in scan order.
 */
class ParallelScanChunks {
public:
    // 'boundaries' must already be sorted in scan order and hold at least two entries; entry 'i'
    // and 'i + 1' delimit the i-th chunk. The chunks are split into 'dop' ranges of consecutive
    // chunks, one per clone.
    ParallelScanChunks(std::vector<RecordId> boundaries, size_t dop)
        : _boundaries(std::move(boundaries)) {
        const size_t count = _boundaries.size() - 1;
        const size_t baseSize = count / dop;
        const size_t numLarger = count % dop;
        size_t begin = 0;
        for (size_t i = 0; i < dop; ++i) {
            const size_t size = baseSize + (i < numLarger ? 1 : 0);
            _initialRanges.push_back({begin, begin + size});
            begin += size;
        }
        _ranges = _initialRanges;
        _opened.resize(dop, false);
    }

    /**
     * Splits the collection identified by 'collUuid' into chunks to be scanned in the 'forward'
     * direction by 'dop' parallel clones, sampling the boundaries with a random cursor. Returns
     * null if the collection cannot be found or is too small to be split into 'dop' chunks.
     */
    static std::shared_ptr<ParallelScanChunks> make(
        CompileCtx& ctx, OperationContext* opCtx, const UUID& collUuid, bool forward, size_t dop);

    /**
     * Returns the next chunk for the clone 'id', or boost::none once they have all been handed
     * out. The chunks are taken from the front of the clone's own range; when that is empty, one is
     * stolen from the back of another range. Thread safe: it is called concurrently by every clone
     * participating in the scan.
     */
    boost::optional<std::pair<RecordId, RecordId>> nextChunk(size_t id) {
        std::lock_guard lk(_mutex);
        size_t chunk;
        if (auto& own = _ranges[id]; !own.empty()) {
            chunk = own.takeFront();
        } else {
            // Steal from the range that comes right after ours first, wrapping around.
            auto victim = _ranges.size();
            for (size_t i = 1; i < _ranges.size(); ++i) {
                auto& other = _ranges[(id + i) % _ranges.size()];
                if (!other.empty()) {
                    chunk = other.takeBack();
                    victim = i;
                    break;
                }
            }
            if (victim == _ranges.size()) {
                return boost::none;
            }
        }
        return std::make_pair(_boundaries[chunk], _boundaries[chunk + 1]);
    }

    /**
     * Makes every chunk available again. Called by each participating clone as it opens; only the
     * first one performs the reset, the other invocations are a no-op. This way a plan that
     * includes a stage that consumes its inputs during the call to open() (e.g. BlockHashAggStage,
     * AndHashStage, HashLookupStage, StreamingAggStage, SortStage::SortImpl) can safely start
     * consuming partitions while slower threads are still attempting to open their cloned source.
     */
    void reset(size_t id) {
        std::lock_guard lk(_mutex);
        if (_opened[id]) {
            // we are reopening a stage that has already processed its input, check if it's the
            // first one and hence we have to restore the partitions as they were defined in the
            // constructor.
            tassert(13436602,
                    "Reset operation requested before the previous one was completed",
                    std::find(_opened.begin(), _opened.end(), false) == _opened.end());
            _opened.assign(_opened.size(), false);
            _ranges = _initialRanges;
        }
        _opened[id] = true;
    }

private:
    // Chunks '[next, end)' are still to be handed out from this range. 'next' advances from the
    // front for the owner of the range, 'end' retreats from the back for the clones stealing.
    struct ChunkRange {
        size_t next;
        size_t end;
        bool empty() const {
            return next >= end;
        }
        size_t takeFront() {
            return next++;
        }
        size_t takeBack() {
            return --end;
        }
    };

    std::mutex _mutex;
    std::vector<bool> _opened;
    const std::vector<RecordId> _boundaries;
    std::vector<ChunkRange> _initialRanges;
    std::vector<ChunkRange> _ranges;
};

/**
 * Scan stage for a clustered collection scan over a single contiguous RecordId range (or an
 * unbounded scan with a known direction). Use MultiRangeClusteredScanStage for non-contiguous
 * (multi-range) clustered scans.
 */
class ScanStage final : public ScanStageBaseImpl<ScanStage> {
    friend class ScanStageBaseImpl<ScanStage>;

public:
    ScanStage(UUID collUuid,
              DatabaseName dbName,
              boost::optional<value::SlotId> recordSlot,
              boost::optional<value::SlotId> recordIdSlot,
              boost::optional<value::SlotId> snapshotIdSlot,
              boost::optional<value::SlotId> indexIdentSlot,
              boost::optional<value::SlotId> indexKeySlot,
              boost::optional<value::SlotId> indexKeyPatternSlot,
              std::vector<std::string> scanFieldNames,
              value::SlotVector scanFieldSlots,
              boost::optional<value::SlotId> minRecordIdSlot,
              boost::optional<value::SlotId> maxRecordIdSlot,
              bool forward,
              PlanYieldPolicySBE* yieldPolicy,
              PlanNodeId nodeId,
              ScanOpenCallback scanOpenCallback,
              // Optional arguments:
              bool participateInTrialRunTracking = true,
              bool includeScanStartRecordId = true,
              bool includeScanEndRecordId = true);


    /**
     * Constructor for clone(). Copies '_state' shared_ptr.
     */
    ScanStage(std::shared_ptr<ScanStageBaseState> state,
              PlanYieldPolicySBE* yieldPolicy,
              PlanNodeId nodeId,
              boost::optional<value::SlotId> minRecordIdSlot,
              boost::optional<value::SlotId> maxRecordIdSlot,
              bool participateInTrialRunTracking,
              bool includeScanStartRecordId,
              bool includeScanEndRecordId,
              std::shared_ptr<ParallelScanChunks> parallelChunks = nullptr,
              size_t parallelId = 0);

    std::vector<std::unique_ptr<PlanStage>> cloneForParallelExecution(CompileCtx& ctx,
                                                                      size_t dop) const final;
    std::unique_ptr<PlanStage> clone() const final;
    void prepare(CompileCtx& ctx) final;
    void close() final;
    std::unique_ptr<PlanStageStats> getStats(bool includeDebugInfo) const final;
    void doDebugPrint(std::vector<DebugPrinter::Block>& ret,
                      DebugPrintInfo& debugPrintInfo) const final;

private:
    inline RecordCursor* getActiveCursor() const {
        return _cursor.get();
    }
    void scanResetState(bool reOpen);
    void getNextHangFailPoint();
    bool pastEnd() const {
        return _havePassedScanEndRecordId;
    }
    boost::optional<Record> getNextInternal();
    boost::optional<Record> getNextInternalParallel();

    // Only for a clustered collection scan, this sets '_minRecordId' to the lower scan bound.
    void setMinRecordId();

    // Only for a clustered collection scan, this sets '_maxRecordId' to the upper scan bound.
    void setMaxRecordId();

    std::unique_ptr<SeekableRecordCursor> _cursor;

    // Only for clustered collection scans: must ScanStageBase::getNext() include the starting
    // bound?
    bool _includeScanStartRecordId = true;

    // Only for clustered collection scans: must ScanStageBase::getNext() include the ending bound?
    bool _includeScanEndRecordId = true;

    // Only for clustered collection scans: does the scan have an end bound?
    bool _hasScanEndRecordId = false;

    // Only for clustered collection scans: have we crossed the scan end bound if there is one?
    bool _havePassedScanEndRecordId = false;

    // Only for clustered collection scans, holds the maximum record ID of the scan, if applicable.
    boost::optional<value::SlotId> _maxRecordIdSlot;
    value::SlotAccessor* _minRecordIdAccessor{nullptr};
    RecordId _minRecordId;

    // Only for clustered collection scans, holds the minimum record ID of the scan, if applicable.
    boost::optional<value::SlotId> _minRecordIdSlot;
    value::SlotAccessor* _maxRecordIdAccessor{nullptr};
    RecordId _maxRecordId;
    // Only care about whether first call of getNext() if clustered scan because we need to seek.
    bool _firstGetNext{false};

    // Hard boundary of the chunk currently being scanned, when this stage is participating in a
    // parallel scan.
    RecordId _excludedHigherBound;

    // The chunks left to scan, shared with the sibling clones of a parallel scan. Null when this
    // stage is not part of one, in which case the bounds above stay at their unbounded values.
    std::shared_ptr<ParallelScanChunks> _parallelChunks;
    // Identifies this clone among its siblings; selects the range of chunks it consumes first.
    size_t _parallelId = 0;

};  // class ScanStage

extern FailPoint hangScanGetNext;

}  // namespace sbe
}  // namespace mongo
