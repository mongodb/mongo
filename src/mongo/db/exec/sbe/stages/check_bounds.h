/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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
#include "mongo/db/query/index_bounds.h"

namespace mongo::sbe {
struct CheckBoundsParams {
    const IndexBounds bounds;
    const BSONObj keyPattern;
    const int direction;
    const KeyString::Version version;
    const Ordering ord;
};

/**
 * This PlanStage takes a pair of index key/recordId slots as an input from its child stage (usually
 * an index scan), and uses the 'IndexBoundsChecker' to check if the index key is within the index
 * bounds specified in the 'CheckBoundsParams'.
 *
 * The stage is used when index bounds cannot be specified as valid low and high keys, which can be
 * fed into the 'IndexScanStage' stage. For example, when index bounds are specified as
 * multi-interval bounds and we cannot decompose it into a number of single-interval bounds.
 *
 * For each input pair, the stage can produce the following output, bound to the 'outSlot':
 *
 *   1. The key is within the bounds - caller can use data associated with this key, so the
 *      'outSlot' would contain the recordId value.
 *   2. The key is past the bounds - no further keys will satisfy the bounds and the caller should
 *      stop, so an EOF would be returned.
 *   3. The key is not within the bounds, but has not exceeded the maximum value. The index scan
 *      would need to advance to the index key provided by the 'IndexBoundsChecker' and returned in
 *      the 'outSlot', and restart the scan from that key.
 *
 * This stage is usually used along with the stack spool to recursively feed the index key produced
 * in case #3 back to the index scan,
 */
class CheckBoundsStage final : public PlanStage {
public:
    CheckBoundsStage(std::unique_ptr<PlanStage> input,
                     const CheckBoundsParams& params,
                     value::SlotId inKeySlot,
                     value::SlotId inRecordIdSlot,
                     value::SlotId outSlot,
                     PlanNodeId planNodeId);

    std::unique_ptr<PlanStage> clone() const final;

    void prepare(CompileCtx& ctx) final;
    value::SlotAccessor* getAccessor(CompileCtx& ctx, value::SlotId slot) final;
    void open(bool reOpen) final;
    PlanState getNext() final;
    void close() final;

    std::unique_ptr<PlanStageStats> getStats() const final;
    const SpecificStats* getSpecificStats() const final;
    std::vector<DebugPrinter::Block> debugPrint() const final;

private:
    const CheckBoundsParams _params;
    IndexBoundsChecker _checker;

    const value::SlotId _inKeySlot;
    const value::SlotId _inRecordIdSlot;
    const value::SlotId _outSlot;

    value::SlotAccessor* _inKeyAccessor{nullptr};
    value::SlotAccessor* _inRecordIdAccessor{nullptr};
    value::OwnedValueAccessor _outAccessor;

    bool _isEOF{false};
};
}  // namespace mongo::sbe
