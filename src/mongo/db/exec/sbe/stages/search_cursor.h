/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/stages/plan_stats.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/pipeline/search_helper.h"
#include "mongo/db/query/stage_types.h"
#include "mongo/executor/task_executor_cursor.h"

namespace mongo::sbe {
/**
 * A stage for $search which maintains a mongot cursor, retrieves one response from mongot in each
 * getNext() call, and puts the whole response (without metadata) along with specific
 * fields/metadata into the given slots.
 *
 * Debug string representation:
 *
 * search_cursor_stage resultSlot? [metaSlot1, ..., metadataSlotN] [fieldSlot1, ..., fieldSlotN]
 *     searchMetaSlot? cursorIdSlot firstBatchSlot limitSlot searchMetaSlot?
 */
class SearchCursorStage final : public PlanStage {
public:
    SearchCursorStage(NamespaceString nss,
                      boost::optional<UUID> collUuid,
                      boost::optional<value::SlotId> resultSlot,
                      std::vector<std::string> metadataNames,
                      value::SlotVector metadataSlots,
                      std::vector<std::string> fieldNames,
                      value::SlotVector fieldSlots,
                      value::SlotId cursorIdSlot,
                      value::SlotId firstBatchSlot,
                      value::SlotId searchQuerySlot,
                      boost::optional<value::SlotId> sortSpecSlot,
                      value::SlotId limitSlot,
                      value::SlotId protocolVersionSlot,
                      boost::optional<ExplainOptions::Verbosity> explain,
                      PlanYieldPolicy* yieldPolicy,
                      PlanNodeId planNodeId);

    std::unique_ptr<PlanStage> clone() const final;

    void prepare(CompileCtx& ctx) final;
    value::SlotAccessor* getAccessor(CompileCtx& ctx, value::SlotId slot) final;
    void open(bool reOpen) final;
    PlanState getNext() final;
    void close() final;

    std::unique_ptr<PlanStageStats> getStats(bool includeDebugInfo) const final;
    const SpecificStats* getSpecificStats() const final;
    std::vector<DebugPrinter::Block> debugPrint() const final override;
    size_t estimateCompileTimeSize() const final;

    /**
     * Calculate the number of documents needed to satisfy a user-defined limit. This information
     * can be used in a getMore sent to mongot.
     */
    boost::optional<long long> calcDocsNeeded();

    void setDocsReturnedStats(const CommonStats* docsReturnedStats) {
        _docsReturnedStats = docsReturnedStats;
    }

private:
    bool shouldReturnEOF();

private:
    const NamespaceString _namespace;
    const boost::optional<UUID> _collUuid;
    // Output slots.
    const boost::optional<value::SlotId> _resultSlot;
    const IndexedStringVector _metadataNames;
    const value::SlotVector _metadataSlots;
    const IndexedStringVector _fieldNames;
    const value::SlotVector _fieldSlots;

    // Input slots.
    const value::SlotId _cursorIdSlot;
    const value::SlotId _firstBatchSlot;
    const value::SlotId _searchQuerySlot;
    const boost::optional<value::SlotId> _sortSpecSlot;
    const value::SlotId _limitSlot;
    const value::SlotId _protocolVersionSlot;

    // Output slot accessors.
    value::OwnedValueAccessor _resultAccessor;
    absl::InlinedVector<value::OwnedValueAccessor, 3> _metadataAccessors;
    value::SlotAccessorMap _metadataAccessorsMap;
    absl::InlinedVector<value::OwnedValueAccessor, 3> _fieldAccessors;
    value::SlotAccessorMap _fieldAccessorsMap;

    // Input slot accessors.
    value::SlotAccessor* _cursorIdAccessor;
    value::SlotAccessor* _firstBatchAccessor;
    value::SlotAccessor* _searchQueryAccessor;
    value::SlotAccessor* _sortSpecAccessor{nullptr};
    value::SlotAccessor* _limitAccessor{nullptr};
    value::SlotAccessor* _protocolVersionAccessor{nullptr};

    // Variables to save the value from input slots.
    boost::optional<BSONObj> _response;
    boost::optional<BSONObj> _resultObj;
    BSONObj _searchQuery;
    uint64_t _limit{0};

    boost::optional<SortKeyGenerator> _sortKeyGen;
    boost::optional<executor::TaskExecutorCursor> _cursor;
    SearchStats _specificStats;
    const boost::optional<ExplainOptions::Verbosity> _explain;
    // A CommonStats that tracks how many documents is returned for $search, in the stored source
    // case, _docsReturnedStats ptr points to current stage, otherwise it points to another stage to
    // skip the docs that been filtered out.
    // TODO: SERVER-80648 to have a better way to track count of idx scan stage.
    const CommonStats* _docsReturnedStats;
    bool _isStoredSource{false};
};
}  // namespace mongo::sbe
