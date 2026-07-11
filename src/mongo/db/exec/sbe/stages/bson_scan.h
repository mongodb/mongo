// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/sbe/stages/plan_stats.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace sbe {
/**
 * Scans a vector of BSON documents. The resulting BSON documents are placed into the
 * given 'recordSlot', if provided.
 *
 * The caller can also optionally provide a vector of top-level field names, 'scanFieldNames', to
 * extract from each BSON object. The resulting values are placed into the slots indicated by the
 * 'scanFieldSlots' slot vector each time this stage advances. The provided 'scanFieldNames' and
 * 'scanFieldSlots' vectors must be of equal length.
 */
class BSONScanStage final : public PlanStage {
public:
    BSONScanStage(std::vector<BSONObj> bsons,
                  boost::optional<value::SlotId> recordSlot,
                  PlanNodeId planNodeId,
                  std::vector<std::string> scanFieldNames = {},
                  value::SlotVector scanFieldSlots = {},
                  bool participateInTrialRunTracking = true);

    std::unique_ptr<PlanStage> clone() const final;

    void prepare(CompileCtx& ctx) final;
    value::SlotAccessor* getAccessor(CompileCtx& ctx, value::SlotId slot) final;
    void open(bool reOpen) final;
    PlanState getNext() final;
    void close() final;

    std::unique_ptr<PlanStageStats> getStats(bool includeDebugInfo) const final;
    const SpecificStats* getSpecificStats() const final;

    void doDebugPrint(std::vector<DebugPrinter::Block>& ret,
                      DebugPrintInfo& debugPrintInfo) const final;
    size_t estimateCompileTimeSize() const final;

protected:
private:
    const std::vector<BSONObj> _bsons;

    const boost::optional<value::SlotId> _recordSlot;
    const std::vector<std::string> _scanFieldNames;
    const value::SlotVector _scanFieldSlots;

    std::unique_ptr<value::ViewOfValueAccessor> _recordAccessor;

    value::FieldViewAccessorMap _scanFieldAccessors;
    value::SlotAccessorMap _scanFieldAccessorsMap;

    std::vector<BSONObj>::const_iterator _bsonCurrent;

    ScanStats _specificStats;
};
}  // namespace sbe
}  // namespace mongo
