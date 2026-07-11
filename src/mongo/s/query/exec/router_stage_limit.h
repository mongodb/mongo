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
 * Passes through the first n results and then returns boost::none.
 */
class RouterStageLimit final : public RouterExecStage {
public:
    RouterStageLimit(OperationContext* opCtx,
                     std::unique_ptr<RouterExecStage> child,
                     long long limit);

    StatusWith<ClusterQueryResult> next() final;

    bool isEOF() const final {
        return _returnedSoFar >= _limit || getChildStage()->isEOF();
    }

private:
    long long _limit;

    long long _returnedSoFar = 0;
};

}  // namespace mongo
