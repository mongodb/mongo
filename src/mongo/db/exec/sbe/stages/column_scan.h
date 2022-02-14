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

#include "mongo/config.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/stages/collection_helpers.h"
#include "mongo/db/exec/sbe/stages/stages.h"

namespace mongo {
namespace sbe {
/**
 * A stage that scans provided columnar index. A set of paths is retrieved from the index and values
 * are put in output slots.
 */
class ColumnScanStage final : public PlanStage {
public:
    ColumnScanStage(UUID collectionUuid,
                    StringData columnIndexName,
                    value::SlotVector fieldSlots,
                    std::vector<std::string> paths,
                    boost::optional<value::SlotId> recordSlot,
                    boost::optional<value::SlotId> recordIdSlot,
                    std::unique_ptr<EExpression> internalExpr,
                    std::vector<std::unique_ptr<EExpression>> pathExprs,
                    value::SlotId internalSlot,
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
    const UUID _collUuid;
    const std::string _columnIndexName;
    const value::SlotVector _fieldSlots;
    const std::vector<std::string> _paths;
    const boost::optional<value::SlotId> _recordSlot;
    const boost::optional<value::SlotId> _recordIdSlot;

    // An optional expression used to reconstruct the output document.
    const std::unique_ptr<EExpression> _recordExpr;
    // Expressions to get values from the row store document.
    const std::vector<std::unique_ptr<EExpression>> _pathExprs;
    // An internal slot that points to the row store document.
    const value::SlotId _rowStoreSlot;

    std::vector<value::OwnedValueAccessor> _outputFields;
    value::SlotAccessorMap _outputFieldsMap;
    std::unique_ptr<value::OwnedValueAccessor> _recordAccessor;
    std::unique_ptr<value::OwnedValueAccessor> _recordIdAccessor;

    std::unique_ptr<vm::CodeFragment> _recordExprCode;
    std::vector<std::unique_ptr<vm::CodeFragment>> _pathExprsCode;
    std::unique_ptr<value::OwnedValueAccessor> _rowStoreAccessor;

    vm::ByteCode _bytecode;

    // These members are default constructed to boost::none and are initialized when 'prepare()'
    // is called. Once they are set, they are never modified again.
    boost::optional<NamespaceString> _collName;
    boost::optional<uint64_t> _catalogEpoch;

    CollectionPtr _coll;

    std::unique_ptr<SeekableRecordCursor> _cursor;

    RecordId _recordId;

    bool _open{false};
    bool _firstGetNext{false};

    // If provided, used during a trial run to accumulate certain execution stats. Once the trial
    // run is complete, this pointer is reset to nullptr.
    TrialRunTracker* _tracker{nullptr};

    ScanStats _specificStats;
};
}  // namespace sbe
}  // namespace mongo
