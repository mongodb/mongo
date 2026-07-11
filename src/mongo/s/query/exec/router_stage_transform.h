// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/s/query/exec/cluster_query_result.h"
#include "mongo/s/query/exec/router_exec_stage.h"
#include "mongo/util/modules.h"

#include <functional>
#include <memory>

namespace mongo {

/**
 * A router execution stage that applies a caller-supplied per-document transformation as results
 * flow through the cursor pipeline. The transform is applied to every document returned by
 * next(), including documents from getMore batches, without buffering the full result set.
 */
class RouterStageTransform final : public RouterExecStage {
public:
    using TransformFn = std::function<BSONObj(BSONObj)>;

    RouterStageTransform(OperationContext* opCtx,
                         std::unique_ptr<RouterExecStage> child,
                         TransformFn transform);

    StatusWith<ClusterQueryResult> next() final;

private:
    TransformFn _transform;
};

}  // namespace mongo
