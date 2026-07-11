// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/exec/classic/record_store_fast_count.h"

#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/util/assert_util.h"

namespace mongo {


RecordStoreFastCountStage::RecordStoreFastCountStage(ExpressionContext* expCtx,
                                                     CollectionAcquisition collection,
                                                     long long skip,
                                                     long long limit)
    : RequiresCollectionStage(kStageType, expCtx, collection), _skip(skip), _limit(limit) {
    tassert(11051634, "Expecting skip parameter to be non-negative", _skip >= 0);
    tassert(11051633, "Expecting limit parameter to be non-negative", _limit >= 0);
}

std::unique_ptr<PlanStageStats> RecordStoreFastCountStage::getStats() {
    auto planStats = std::make_unique<PlanStageStats>(_commonStats, STAGE_RECORD_STORE_FAST_COUNT);
    planStats->specific = std::make_unique<CountStats>(_specificStats);
    return planStats;
}

PlanStage::StageState RecordStoreFastCountStage::doWork(WorkingSetID* out) {
    // This stage never returns a working set member.
    *out = WorkingSet::INVALID_ID;

    long long nCounted = collectionPtr()->latestSizeCount(opCtx()).count;

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
