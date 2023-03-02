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

#include "mongo/db/exec/spool.h"

namespace mongo {

const char* SpoolStage::kStageType = "SPOOL";

SpoolStage::SpoolStage(ExpressionContext* expCtx, WorkingSet* ws, std::unique_ptr<PlanStage> child)
    : PlanStage(expCtx, std::move(child), kStageType), _ws(ws) {}

bool SpoolStage::isEOF() {
    return _nextIndex == static_cast<int>(_buffer.size());
}

std::unique_ptr<PlanStageStats> SpoolStage::getStats() {
    _commonStats.isEOF = isEOF();
    auto ret = std::make_unique<PlanStageStats>(_commonStats, stageType());
    ret->specific = std::make_unique<SpoolStats>(_specificStats);
    ret->children.emplace_back(child()->getStats());
    return ret;
}

PlanStage::StageState SpoolStage::doWork(WorkingSetID* out) {
    if (isEOF()) {
        return PlanStage::IS_EOF;
    }

    if (_nextIndex < 0) {
        // We have not yet received EOF from our child yet. Eagerly consume and cache results as
        // long as the child keeps advancing (we'll propagate yields and NEED_TIME).
        WorkingSetID id = WorkingSet::INVALID_ID;
        StageState status = child()->work(&id);

        while (status == PlanStage::ADVANCED) {
            // The child has returned another result, put it in our cache.
            auto member = _ws->get(id);
            tassert(
                7443500, "WSM passed to spool stage must have a RecordId", member->hasRecordId());

            // TODO SERVER-74437 spill to disk if necessary
            _specificStats.totalDataSizeBytes += member->recordId.memUsage();
            _buffer.emplace_back(std::move(member->recordId));

            // We've cached the RecordId, so go ahead and free the object in the working set.
            _ws->free(id);

            // Ask the child for another record.
            status = child()->work(&id);
        }

        if (status != PlanStage::IS_EOF) {
            *out = id;
            return status;
        }

        // The child has returned all of its results. Fall through and begin consuming the results
        // from our buffer.
    }

    // Increment to the next element in our buffer. Note that we increment the index *first* so that
    // we will return EOF in a call to doWork() before isEOF() returns true.
    if (++_nextIndex == static_cast<int>(_buffer.size())) {
        return PlanStage::IS_EOF;
    }

    *out = _ws->allocate();
    auto member = _ws->get(*out);
    member->recordId = std::move(_buffer[_nextIndex]);
    // Only store the record id, not any index information or full objects. This is to
    // reduce memory and disk usage - it is the responsibility of our caller to fetch the records.
    member->transitionToRecordIdAndObj();
    return PlanStage::ADVANCED;
}
}  // namespace mongo
