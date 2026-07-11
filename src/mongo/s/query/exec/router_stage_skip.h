// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/s/query/exec/cluster_query_result.h"
#include "mongo/s/query/exec/router_exec_stage.h"
#include "mongo/util/modules.h"

#include <memory>

namespace mongo {

/**
 * Skips the first n results from the child and then passes through the remaining results.
 */
class RouterStageSkip final : public RouterExecStage {
public:
    RouterStageSkip(OperationContext* opCtx,
                    std::unique_ptr<RouterExecStage> child,
                    long long skip);

    StatusWith<ClusterQueryResult> next() final;

    bool isEOF() const final {
        return getChildStage()->isEOF();
    }

private:
    long long _skip;

    long long _skippedSoFar = 0;
};

}  // namespace mongo
