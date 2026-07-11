// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/timestamp.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/replicated_fast_count/size_count_checkpoint_buffer.h"

#include <boost/optional/optional.hpp>

namespace mongo::replicated_fast_count::oplog_tailer {

/**
 * Scans and buffers newly visible entries into `buffer`.
 */
void bufferNewOplogEntries(OperationContext* opCtx, SizeCountCheckpointBuffer& buffer);

/**
 * Tails the oplog for size/count deltas in a loop until interrupted.
 *
 * New size/count deltas are written to `buffer`.
 */
void run(OperationContext* opCtx, SizeCountCheckpointBuffer& buffer);

}  // namespace mongo::replicated_fast_count::oplog_tailer
