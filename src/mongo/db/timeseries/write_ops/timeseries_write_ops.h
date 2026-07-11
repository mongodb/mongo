// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/curop.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/util/modules.h"

[[MONGO_MOD_PUBLIC]];

namespace mongo::timeseries::write_ops {

/**
 * Performs a write to a time-series collection. Returns an InsertCommandReply if the timeseries
 * writes succeeded.
 */
mongo::write_ops::InsertCommandReply performTimeseriesWrites(
    OperationContext* opCtx,
    const mongo::write_ops::InsertCommandRequest& request,
    const timeseries::CollectionPreConditions& preConditions,
    CurOp* curOp);


/**
 * Performs a write to a time-series collection. Returns an InsertCommandReply if the timeseries
 * writes succeeded. Same as above, but generates its own curOp.
 */
mongo::write_ops::InsertCommandReply performTimeseriesWrites(
    OperationContext* opCtx,
    const mongo::write_ops::InsertCommandRequest& request,
    const timeseries::CollectionPreConditions& preConditions);

}  // namespace mongo::timeseries::write_ops
