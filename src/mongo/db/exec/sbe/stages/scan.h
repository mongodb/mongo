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

#include "mongo/base/string_data.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/admission/execution_control/execution_admission_context.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/stages/collection_helpers.h"
#include "mongo/db/exec/sbe/stages/plan_stats.h"
#include "mongo/db/exec/sbe/stages/scan_helpers.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/record_id.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/modules.h"
#include "mongo/util/string_listset.h"
#include "mongo/util/string_map.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <limits>
#include <memory>
#include <string>
#include <utility>
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
                  PlanYieldPolicy* yieldPolicy,
                  PlanNodeId nodeId,
                  ScanOpenCallback scanOpenCallback,
                  bool forward,
                  // Optional arguments:
                  bool participateInTrialRunTracking = true);

    /**
     * Constructor for clone(). Copies '_state' shared_ptr.
     */
    ScanStageBase(std::shared_ptr<ScanStageBaseState> state,
                  PlanYieldPolicy* yieldPolicy,
                  PlanNodeId nodeId,
                  bool participateInTrialRunTracking);

    value::SlotAccessor* getAccessor(CompileCtx& ctx, value::SlotId slot) final;
    void closeShared();
    void debugPrintShared(std::vector<DebugPrinter::Block>& ret) const;
    void doAttachCollectionAcquisition(const MultipleCollectionAccessor& mca) override;
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
    inline void handleEOF(const boost::optional<Record>& nextRecord) {
        if (_state->recordIdSlot) {
            auto [tag, val] = sbe::value::makeCopyRecordId(RecordId());
            _recordIdAccessor.reset(true, tag, val);
        }
    };

    // Shared logic for getNext()
    // Helper to reset record ID if a `recordIdSlot` is present and to track end bounds.
    inline void resetRecordId(const boost::optional<Record>& nextRecord) {
        if (_state->recordSlot) {
            _recordAccessor.reset(false,
                                  value::TypeTags::bsonObject,
                                  value::bitcastFrom<const char*>(nextRecord->data.data()));
        }
    };

    MONGO_COMPILER_ALWAYS_INLINE
    value::OwnedValueAccessor* getFieldAccessor(StringData name) {
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
    //     '_scanFieldAccessors' - slot accessors corresponding, by index, to _state->scanFieldNames
    //     '_scanFieldAccessorsMap' - a map from vector index to pointer to the corresponding
    //         accessor in '_scanFieldAccessors'
    absl::InlinedVector<value::OwnedValueAccessor, 4> _scanFieldAccessors;
    value::SlotAccessorMap _scanFieldAccessorsMap;

    CollectionRef _coll;

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
                      PlanYieldPolicy* yieldPolicy,
                      PlanNodeId nodeId,
                      ScanOpenCallback scanOpenCallback,
                      bool forward,
                      // Optional arguments:
                      bool participateInTrialRunTracking = true);

    /**
     * Constructor for clone(). Copies '_state' shared_ptr.
     */
    ScanStageBaseImpl(std::shared_ptr<ScanStageBaseState> state,
                      PlanYieldPolicy* yieldPolicy,
                      PlanNodeId nodeId,
                      bool participateInTrialRunTracking);
    void open(bool reOpen) final;

protected:
    void doSaveState() override;
    void doRestoreState() override;
    void doDetachFromOperationContext() override;
    void doAttachToOperationContext(OperationContext* opCtx) override;

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
              PlanYieldPolicy* yieldPolicy,
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
              PlanYieldPolicy* yieldPolicy,
              PlanNodeId nodeId,
              boost::optional<value::SlotId> minRecordIdSlot,
              boost::optional<value::SlotId> maxRecordIdSlot,
              bool participateInTrialRunTracking,
              bool includeScanStartRecordId,
              bool includeScanEndRecordId);

    std::unique_ptr<PlanStage> clone() const final;
    PlanState getNext() final;
    void prepare(CompileCtx& ctx) final;
    void close() final;
    std::unique_ptr<PlanStageStats> getStats(bool includeDebugInfo) const final;
    std::vector<DebugPrinter::Block> debugPrint(const DebugPrintInfo& debugPrintInfo) const final;

private:
    inline RecordCursor* getActiveCursor() const {
        return _cursor.get();
    }
    void scanResetState(bool reOpen);

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

    // Only for clustered collection scans, holds the minimum record ID of the scan, if applicable.
    boost::optional<value::SlotId> _maxRecordIdSlot;
    value::SlotAccessor* _minRecordIdAccessor{nullptr};
    RecordId _minRecordId;

    // Only for clustered collection scans, holds the maximum record ID of the scan, if applicable.
    boost::optional<value::SlotId> _minRecordIdSlot;
    value::SlotAccessor* _maxRecordIdAccessor{nullptr};
    RecordId _maxRecordId;
    // Only care about whether first call of getNext() if clustered scan because we need to seek
    bool _firstGetNext{false};
};  // class ScanStage
}  // namespace sbe
}  // namespace mongo
