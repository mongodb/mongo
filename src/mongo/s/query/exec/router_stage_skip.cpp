// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/s/query/exec/router_stage_skip.h"

#include "mongo/util/assert_util.h"

#include <utility>


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {

RouterStageSkip::RouterStageSkip(OperationContext* opCtx,
                                 std::unique_ptr<RouterExecStage> child,
                                 long long skip)
    : RouterExecStage(opCtx, std::move(child)), _skip(skip) {
    tassert(11052353, "Expected positive value for skip", skip >= 0);
}

StatusWith<ClusterQueryResult> RouterStageSkip::next() {
    while (_skippedSoFar < _skip) {
        auto next = getChildStage()->next();
        if (!next.isOK()) {
            return next;
        }

        if (next.getValue().isEOF()) {
            return next;
        }

        ++_skippedSoFar;
    }

    return getChildStage()->next();
}

}  // namespace mongo
