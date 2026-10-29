// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/s/query/exec/router_stage_limit.h"

#include "mongo/util/assert_util.h"

#include <utility>


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {

RouterStageLimit::RouterStageLimit(OperationContext* opCtx,
                                   std::unique_ptr<RouterExecStage> child,
                                   long long limit)
    : RouterExecStage(opCtx, std::move(child)), _limit(limit) {
    tassert(11052347, "Expected positive value for limit", limit > 0);
}

StatusWith<ClusterQueryResult> RouterStageLimit::next() {
    if (_returnedSoFar >= _limit) {
        return {ClusterQueryResult()};
    }

    auto childResult = getChildStage()->next();
    if (!childResult.isOK()) {
        return childResult;
    }

    if (!childResult.getValue().isEOF()) {
        ++_returnedSoFar;
    }
    return childResult;
}

}  // namespace mongo
