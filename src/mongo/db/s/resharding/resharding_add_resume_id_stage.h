// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {

/**
 * This stage is responsible for attaching the resharding's _id field to all the input oplog entry
 * documents. For a document that corresponds to an applyOps oplog entry for a committed
 * transaction, this will be {clusterTime: <transaction commit timestamp>, ts: <applyOps
 * optime.ts>}. For all other documents, this will be {clusterTime: <optime.ts>, ts: <optime.ts>}.
 */
class ReshardingAddResumeIdStage final : public Stage {
public:
    ReshardingAddResumeIdStage(std::string_view stageName,
                               const boost::intrusive_ptr<ExpressionContext>& expCtx);

private:
    GetNextResult doGetNext() override;
};

}  // namespace mongo::exec::agg
