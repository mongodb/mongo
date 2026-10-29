// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/query/exec/router_stage_transform.h"

#include <utility>

namespace mongo {

RouterStageTransform::RouterStageTransform(OperationContext* opCtx,
                                           std::unique_ptr<RouterExecStage> child,
                                           TransformFn transform)
    : RouterExecStage(opCtx, std::move(child)), _transform(std::move(transform)) {}

StatusWith<ClusterQueryResult> RouterStageTransform::next() {
    auto childResult = getChildStage()->next();
    if (!childResult.isOK() || childResult.getValue().isEOF()) {
        return childResult;
    }
    return ClusterQueryResult(_transform(*childResult.getValue().getResult()),
                              childResult.getValue().getShardId());
}

}  // namespace mongo
