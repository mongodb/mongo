/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/stages/scan.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <string>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace sbe {
class GenericScanStage final : public ScanStageBaseImpl<GenericScanStage> {
    friend class ScanStageBaseImpl<GenericScanStage>;

public:
    GenericScanStage(UUID collUuid,
                     DatabaseName dbName,
                     boost::optional<value::SlotId> recordSlot,
                     boost::optional<value::SlotId> recordIdSlot,
                     boost::optional<value::SlotId> snapshotIdSlot,
                     boost::optional<value::SlotId> indexIdentSlot,
                     boost::optional<value::SlotId> indexKeySlot,
                     boost::optional<value::SlotId> indexKeyPatternSlot,
                     std::vector<std::string> scanFieldNames,
                     value::SlotVector scanFieldSlots,
                     bool forward,
                     PlanYieldPolicy* yieldPolicy,
                     PlanNodeId nodeId,
                     ScanOpenCallback scanOpenCallback,
                     // Optional arguments:
                     bool participateInTrialRunTracking = true);
    /**
     * Constructor for clone(). Copies '_state' shared_ptr.
     */
    GenericScanStage(std::shared_ptr<ScanStageBaseState> state,
                     PlanYieldPolicy* yieldPolicy,
                     PlanNodeId nodeId,
                     bool participateInTrialRunTracking);

    std::unique_ptr<PlanStage> clone() const final;
    PlanState getNext() final;
    void inline prepare(CompileCtx& ctx) final {
        prepareShared(ctx);
    }
    inline void close() final {
        closeShared();
        _cursor.reset();
    }
    std::unique_ptr<PlanStageStats> getStats(bool includeDebugInfo) const final;
    std::vector<DebugPrinter::Block> debugPrint(const DebugPrintInfo& debugPrintInfo) const final;

private:
    inline void scanResetState(bool reOpen) {
        _cursor = _coll.getPtr()->getCursor(_opCtx, _state->forward);
    }
    inline RecordCursor* getActiveCursor() const {
        return _cursor.get();
    }

    std::unique_ptr<SeekableRecordCursor> _cursor;
};  // class GenericScanStage
}  // namespace sbe
}  // namespace mongo
