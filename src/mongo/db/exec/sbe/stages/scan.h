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

#include "mongo/config.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/expressions/runtime_environment.h"
#include "mongo/db/exec/sbe/stages/collection_helpers.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/storage/record_store.h"

namespace mongo {
namespace sbe {
using ScanOpenCallback = std::function<void(OperationContext*, const CollectionPtr&, bool)>;

struct ScanCallbacks {
    ScanCallbacks(IndexKeyCorruptionCheckCallback indexKeyCorruptionCheck = {},
                  IndexKeyConsistencyCheckCallback indexKeyConsistencyCheck = {},
                  ScanOpenCallback scanOpen = {})
        : indexKeyCorruptionCheckCallback(std::move(indexKeyCorruptionCheck)),
          indexKeyConsistencyCheckCallBack(std::move(indexKeyConsistencyCheck)),
          scanOpenCallback(std::move(scanOpen)) {}

    IndexKeyCorruptionCheckCallback indexKeyCorruptionCheckCallback;
    IndexKeyConsistencyCheckCallback indexKeyConsistencyCheckCallBack;
    ScanOpenCallback scanOpenCallback;
};

/**
 * Retrieves documents from the collection with the given 'collectionUuid' using the storage API.
 * Can be used as either a full scan of the collection, or as a seek. In the latter case, this stage
 * is given a 'seekKeySlot' from which to read a 'RecordId'. We seek to this 'RecordId' and then
 * scan from that point to the end of the collection.
 *
 * If the 'recordSlot' is provided, then each of the records returned from the scan is placed into
 * an output slot with this slot id. Similarly, if 'recordIdSlot' is provided, then this slot is
 * populated with the record id on each advance.
 *
 * In addition, the scan/seek can extract a set of top-level fields from each document. The caller
 * asks for this by passing a vector of 'fields', along with a corresponding slot vector 'vars' into
 * which the resulting values should be stored. These vectors must have the same length.
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
 * via 'snapshotIdSlot', 'indexIdSlot', 'indexKeySlot', and 'indexKeyPatternSlot'.
 *
 * If this is an oplog scan, then the 'oplogTsSlot' will be populated with the "ts" field from each
 * oplog entry.
 *
 * Debug string representations:
 *
 *  scan recordSlot|none recordIdSlot|none snapshotIdSlot|none indexIdSlot|none indexKeySlot|none
 *       indexKeyPatternSlot|none [slot1 = fieldName1, ... slot_n = fieldName_n] collectionUuid
 *       forward needOplogSlotForTs
 *
 *  seek seekKeySlot recordSlot|none recordIdSlot|none snapshotIdSlot|none indexIdSlot|none
 *       indexKeySlot|none indexKeyPatternSlot|none [slot1 = fieldName1, ... slot_n = fieldName_n]
 *       collectionUuid forward needOplogSlotForTs
 */
class ScanStage final : public PlanStage {
public:
    ScanStage(UUID collectionUuid,
              boost::optional<value::SlotId> recordSlot,
              boost::optional<value::SlotId> recordIdSlot,
              boost::optional<value::SlotId> snapshotIdSlot,
              boost::optional<value::SlotId> indexIdSlot,
              boost::optional<value::SlotId> indexKeySlot,
              boost::optional<value::SlotId> indexKeyPatternSlot,
              boost::optional<value::SlotId> oplogTsSlot,
              std::vector<std::string> fields,
              value::SlotVector vars,
              boost::optional<value::SlotId> seekKeySlot,
              bool forward,
              PlanYieldPolicy* yieldPolicy,
              PlanNodeId nodeId,
              ScanCallbacks scanCallbacks,
              bool useRandomCursor = false,
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

    static uint64_t computeFieldMask(const char* name, size_t length) {
        // Discard the upper bits so that 'shiftAmt' is always between 0 and 63 inclusive.
        auto shiftAmt = static_cast<unsigned char>(name[length / 2]) & 63u;
        return uint64_t{1} << shiftAmt;
    }

    const UUID _collUuid;
    const boost::optional<value::SlotId> _recordSlot;
    const boost::optional<value::SlotId> _recordIdSlot;
    const boost::optional<value::SlotId> _snapshotIdSlot;
    const boost::optional<value::SlotId> _indexIdSlot;
    const boost::optional<value::SlotId> _indexKeySlot;
    const boost::optional<value::SlotId> _indexKeyPatternSlot;
    const boost::optional<value::SlotId> _oplogTsSlot;

    const std::vector<std::string> _fields;
    const value::SlotVector _vars;

    const boost::optional<value::SlotId> _seekKeySlot;
    const bool _forward;

    // These members are default constructed to boost::none and are initialized when 'prepare()'
    // is called. Once they are set, they are never modified again.
    boost::optional<NamespaceString> _collName;
    boost::optional<uint64_t> _catalogEpoch;

    CollectionPtr _coll;

    // If provided, used during a trial run to accumulate certain execution stats. Once the trial
    // run is complete, this pointer is reset to nullptr.
    TrialRunTracker* _tracker{nullptr};

    const ScanCallbacks _scanCallbacks;

    std::unique_ptr<value::OwnedValueAccessor> _recordAccessor;
    std::unique_ptr<value::OwnedValueAccessor> _recordIdAccessor;
    value::SlotAccessor* _snapshotIdAccessor{nullptr};
    value::SlotAccessor* _indexIdAccessor{nullptr};
    value::SlotAccessor* _indexKeyAccessor{nullptr};
    value::SlotAccessor* _indexKeyPatternAccessor{nullptr};

    // If this ScanStage was constructed with _oplogTsSlot set, then _oplogTsAccessor will point to
    // an accessor in the RuntimeEnvironment, and value of the "ts" field (if it exists) from each
    // record scanned will be written to this accessor. The engine uses mechanism to keep track of
    // the most recent timestamp that has been observed when scanning the oplog collection.
    RuntimeEnvironment::Accessor* _oplogTsAccessor{nullptr};

    // Used to return a random sample of the collection.
    const bool _useRandomCursor;

    value::FieldAccessorMap _fieldAccessors;
    value::SlotAccessorMap _varAccessors;
    value::SlotAccessor* _seekKeyAccessor{nullptr};

    // _tsFieldAccessor points to the accessor for field "ts". We use _tsFieldAccessor to get at
    // the accessor quickly rather than having to look it up in the _fieldAccessors hashtable.
    value::SlotAccessor* _tsFieldAccessor{nullptr};

    uint64_t _fieldsBloomFilter{0};

    RecordId _recordId;

    bool _open{false};

    std::unique_ptr<SeekableRecordCursor> _cursor;

    // TODO: SERVER-62647. Consider removing random cursor when no longer needed.
    std::unique_ptr<RecordCursor> _randomCursor;

    RecordId _key;
    bool _firstGetNext{false};

    ScanStats _specificStats;

    // Flag set upon restoring the stage that indicates whether the cursor's position in the
    // collection is still valid. Only relevant to capped collections.
    bool _needsToCheckCappedPositionLost = false;

#if defined(MONGO_CONFIG_DEBUG_BUILD)
    // Debug-only buffer used to track the last thing returned from the stage. Between
    // saves/restores this is used to check that the storage cursor has not changed position.
    std::vector<char> _lastReturned;
#endif
};

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
    ParallelScanStage(UUID collectionUuid,
                      boost::optional<value::SlotId> recordSlot,
                      boost::optional<value::SlotId> recordIdSlot,
                      boost::optional<value::SlotId> snapshotIdSlot,
                      boost::optional<value::SlotId> indexIdSlot,
                      boost::optional<value::SlotId> indexKeySlot,
                      boost::optional<value::SlotId> indexKeyPatternSlot,
                      std::vector<std::string> fields,
                      value::SlotVector vars,
                      PlanYieldPolicy* yieldPolicy,
                      PlanNodeId nodeId,
                      ScanCallbacks callbacks,
                      bool participateInTrialRunTracking = true);

    ParallelScanStage(const std::shared_ptr<ParallelState>& state,
                      const UUID& collectionUuid,
                      boost::optional<value::SlotId> recordSlot,
                      boost::optional<value::SlotId> recordIdSlot,
                      boost::optional<value::SlotId> snapshotIdSlot,
                      boost::optional<value::SlotId> indexIdSlot,
                      boost::optional<value::SlotId> indexKeySlot,
                      boost::optional<value::SlotId> indexKeyPatternSlot,
                      std::vector<std::string> fields,
                      value::SlotVector vars,
                      PlanYieldPolicy* yieldPolicy,
                      PlanNodeId nodeId,
                      ScanCallbacks callbacks,
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

    const UUID _collUuid;
    const boost::optional<value::SlotId> _recordSlot;
    const boost::optional<value::SlotId> _recordIdSlot;
    const boost::optional<value::SlotId> _snapshotIdSlot;
    const boost::optional<value::SlotId> _indexIdSlot;
    const boost::optional<value::SlotId> _indexKeySlot;
    const boost::optional<value::SlotId> _indexKeyPatternSlot;
    const std::vector<std::string> _fields;
    const value::SlotVector _vars;

    // These members are default constructed to boost::none and are initialized when 'prepare()'
    // is called. Once they are set, they are never modified again.
    boost::optional<NamespaceString> _collName;
    boost::optional<uint64_t> _catalogEpoch;

    CollectionPtr _coll;

    std::shared_ptr<ParallelState> _state;

    const ScanCallbacks _scanCallbacks;

    std::unique_ptr<value::OwnedValueAccessor> _recordAccessor;
    std::unique_ptr<value::OwnedValueAccessor> _recordIdAccessor;
    value::SlotAccessor* _snapshotIdAccessor{nullptr};
    value::SlotAccessor* _indexIdAccessor{nullptr};
    value::SlotAccessor* _indexKeyAccessor{nullptr};
    value::SlotAccessor* _indexKeyPatternAccessor{nullptr};

    value::FieldAccessorMap _fieldAccessors;
    value::SlotAccessorMap _varAccessors;

    size_t _currentRange{std::numeric_limits<std::size_t>::max()};
    Range _range;

    RecordId _recordId;

    bool _open{false};

    std::unique_ptr<SeekableRecordCursor> _cursor;

#if defined(MONGO_CONFIG_DEBUG_BUILD)
    // Debug-only buffer used to track the last thing returned from the stage. Between
    // saves/restores this is used to check that the storage cursor has not changed position.
    std::vector<char> _lastReturned;
#endif
};
}  // namespace sbe
}  // namespace mongo
