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

#include "mongo/db/db_raii.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/storage/record_store.h"

namespace mongo {
namespace sbe {
using ScanOpenCallback = std::function<void(OperationContext*, const CollectionPtr&, bool)>;

class ScanStage final : public PlanStage {
public:
    ScanStage(const NamespaceStringOrUUID& name,
              boost::optional<value::SlotId> recordSlot,
              boost::optional<value::SlotId> recordIdSlot,
              std::vector<std::string> fields,
              value::SlotVector vars,
              boost::optional<value::SlotId> seekKeySlot,
              bool forward,
              PlanYieldPolicy* yieldPolicy,
              PlanNodeId nodeId,
              ScanOpenCallback openCallback = {});

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
    const NamespaceStringOrUUID _name;
    const boost::optional<value::SlotId> _recordSlot;
    const boost::optional<value::SlotId> _recordIdSlot;
    const std::vector<std::string> _fields;
    const value::SlotVector _vars;
    const boost::optional<value::SlotId> _seekKeySlot;
    const bool _forward;

    // If provided, used during a trial run to accumulate certain execution stats. Once the trial
    // run is complete, this pointer is reset to nullptr.
    TrialRunTracker* _tracker{nullptr};

    ScanOpenCallback _openCallback;

    std::unique_ptr<value::ViewOfValueAccessor> _recordAccessor;
    std::unique_ptr<value::ViewOfValueAccessor> _recordIdAccessor;

    value::FieldAccessorMap _fieldAccessors;
    value::SlotAccessorMap _varAccessors;
    value::SlotAccessor* _seekKeyAccessor{nullptr};

    bool _open{false};

    std::unique_ptr<SeekableRecordCursor> _cursor;
    boost::optional<AutoGetCollectionForReadMaybeLockFree> _coll;
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
    ParallelScanStage(const NamespaceStringOrUUID& name,
                      boost::optional<value::SlotId> recordSlot,
                      boost::optional<value::SlotId> recordIdSlot,
                      std::vector<std::string> fields,
                      value::SlotVector vars,
                      PlanYieldPolicy* yieldPolicy,
                      PlanNodeId nodeId);

    ParallelScanStage(const std::shared_ptr<ParallelState>& state,
                      const NamespaceStringOrUUID& name,
                      boost::optional<value::SlotId> recordSlot,
                      boost::optional<value::SlotId> recordIdSlot,
                      std::vector<std::string> fields,
                      value::SlotVector vars,
                      PlanYieldPolicy* yieldPolicy,
                      PlanNodeId nodeId);

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

    const NamespaceStringOrUUID _name;
    const boost::optional<value::SlotId> _recordSlot;
    const boost::optional<value::SlotId> _recordIdSlot;
    const std::vector<std::string> _fields;
    const value::SlotVector _vars;

    std::shared_ptr<ParallelState> _state;

    std::unique_ptr<value::ViewOfValueAccessor> _recordAccessor;
    std::unique_ptr<value::ViewOfValueAccessor> _recordIdAccessor;

    value::FieldAccessorMap _fieldAccessors;
    value::SlotAccessorMap _varAccessors;

    size_t _currentRange{std::numeric_limits<std::size_t>::max()};
    Range _range;

    bool _open{false};

    std::unique_ptr<SeekableRecordCursor> _cursor;
    boost::optional<AutoGetCollectionForReadMaybeLockFree> _coll;
};
}  // namespace sbe
}  // namespace mongo
