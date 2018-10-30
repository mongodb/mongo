/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/exec/record_store_fast_count.h"

namespace mongo {

const char* RecordStoreFastCountStage::kStageType = "RECORD_STORE_FAST_COUNT";

RecordStoreFastCountStage::RecordStoreFastCountStage(OperationContext* opCtx,
                                                     Collection* collection,
                                                     long long skip,
                                                     long long limit)
    : RequiresCollectionStage(kStageType, opCtx, collection), _skip(skip), _limit(limit) {
    invariant(_skip >= 0);
    invariant(_limit >= 0);
}

std::unique_ptr<PlanStageStats> RecordStoreFastCountStage::getStats() {
    auto planStats = std::make_unique<PlanStageStats>(_commonStats, STAGE_RECORD_STORE_FAST_COUNT);
    planStats->specific = std::make_unique<CountStats>(_specificStats);
    return planStats;
}

PlanStage::StageState RecordStoreFastCountStage::doWork(WorkingSetID* out) {
    // This stage never returns a working set member.
    *out = WorkingSet::INVALID_ID;

    long long nCounted = collection()->numRecords(getOpCtx());

    if (_skip) {
        nCounted -= _skip;
        if (nCounted < 0) {
            nCounted = 0;
        }
    }

    long long limit = _limit;
    if (limit < 0) {
        limit = -limit;
    }

    if (limit < nCounted && 0 != limit) {
        nCounted = limit;
    }

    _specificStats.nCounted = nCounted;
    _specificStats.nSkipped = _skip;
    _commonStats.isEOF = true;

    return PlanStage::IS_EOF;
}

}  // namespace mongo
