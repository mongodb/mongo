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

#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/bson.h"

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

    std::vector<DebugPrinter::Block> debugPrint() const final;
    size_t estimateCompileTimeSize() const final;

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
