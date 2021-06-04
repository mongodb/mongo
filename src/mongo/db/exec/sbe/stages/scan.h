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

#include "mongo/db/exec/sbe/expressions/expression.h"
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

class ScanStage final : public PlanStage {
public:
    ScanStage(CollectionUUID collectionUuid,
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
              ScanCallbacks scanCallbacks);

    std::unique_ptr<PlanStage> clone() const final;

    void prepare(CompileCtx& ctx) final;
    value::SlotAccessor* getAccessor(CompileCtx& ctx, value::SlotId slot) final;
    void open(bool reOpen) final;
    PlanState getNext() final;
    void close() final;

    std::unique_ptr<PlanStageStats> getStats(bool includeDebugInfo) const final;
    const SpecificStats* getSpecificStats() const final;
    std::vector<DebugPrinter::Block> debugPrint() const final;

protected:
    void doSaveState() override;
    void doRestoreState() override;
    void doDetachFromOperationContext() override;
    void doAttachToOperationContext(OperationContext* opCtx) override;
    void doDetachFromTrialRunTracker() override;
    void doAttachToTrialRunTracker(TrialRunTracker* tracker) override;

private:
    const CollectionUUID _collUuid;
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

    NamespaceString _collName;
    uint64_t _catalogEpoch;

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
    RuntimeEnvironment::Accessor* _oplogTsAccessor{nullptr};

    value::FieldAccessorMap _fieldAccessors;
    value::SlotAccessorMap _varAccessors;
    value::SlotAccessor* _seekKeyAccessor{nullptr};

    bool _open{false};

    std::unique_ptr<SeekableRecordCursor> _cursor;
    RecordId _key;
    bool _firstGetNext{false};

    ScanStats _specificStats;
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
    ParallelScanStage(CollectionUUID collectionUuid,
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
                      ScanCallbacks callbacks);

    ParallelScanStage(const std::shared_ptr<ParallelState>& state,
                      CollectionUUID collectionUuid,
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
                      ScanCallbacks callbacks);

    std::unique_ptr<PlanStage> clone() const final;

    void prepare(CompileCtx& ctx) final;
    value::SlotAccessor* getAccessor(CompileCtx& ctx, value::SlotId slot) final;
    void open(bool reOpen) final;
    PlanState getNext() final;
    void close() final;

    std::unique_ptr<PlanStageStats> getStats(bool includeDebugInfo) const final;
    const SpecificStats* getSpecificStats() const final;
    std::vector<DebugPrinter::Block> debugPrint() const final;

protected:
    void doSaveState() final;
    void doRestoreState() final;
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

    const CollectionUUID _collUuid;
    const boost::optional<value::SlotId> _recordSlot;
    const boost::optional<value::SlotId> _recordIdSlot;
    const boost::optional<value::SlotId> _snapshotIdSlot;
    const boost::optional<value::SlotId> _indexIdSlot;
    const boost::optional<value::SlotId> _indexKeySlot;
    const boost::optional<value::SlotId> _indexKeyPatternSlot;
    const std::vector<std::string> _fields;
    const value::SlotVector _vars;

    NamespaceString _collName;
    uint64_t _catalogEpoch;

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

    bool _open{false};

    std::unique_ptr<SeekableRecordCursor> _cursor;
};
}  // namespace sbe
}  // namespace mongo
