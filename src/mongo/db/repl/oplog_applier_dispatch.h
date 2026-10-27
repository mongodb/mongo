// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog_applier.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_or_grouped_inserts.h"
#include "mongo/util/modules.h"

#include <vector>

namespace mongo {
namespace replicated_fast_count {
class SizeCountCheckpointValidator;
}  // namespace replicated_fast_count

namespace repl {

class SessionUpdateTracker;

/**
 * ops - This only modifies the isForCappedCollection and hasRetryImage fields on each op. It does
 *       not alter the ops vector in any other way.
 *
 * writerVectors - Set of operations for each worker thread to apply.
 *
 * derivedOps - If provided, this function inserts a decomposition of applyOps operations and
 *              instructions for updating the transactions table.  Required if processing oplogs
 *              with transactions.
 *
 * sessionUpdateTracker - if provided, keeps track of session info from ops.
 *
 * checkpointValidator - if provided, each entry past options.beginApplyingOpTime is preprocessed
 *                       for continuous internode checkpoint validation. Callers are responsible
 *                       for gating on the application mode and the enabling server parameter.
 */
void deriveOpsAndFillWriterVectors(
    const OplogApplier::Options& options,
    OperationContext* opCtx,
    std::vector<OplogEntry>* ops,
    std::vector<std::vector<ApplierOperation>>* writerVectors,
    std::vector<std::vector<OplogEntry>>* derivedOps,
    SessionUpdateTracker* sessionUpdateTracker,
    replicated_fast_count::SizeCountCheckpointValidator* checkpointValidator);

/**
 * Derives the operations to apply from a batch of oplog entries and distributes them across
 * per-worker writer vectors.
 *
 * checkpointValidator - if provided, each entry past options.beginApplyingOpTime is preprocessed
 *                       for continuous internode checkpoint validation. Callers are responsible
 *                       for gating on the application mode and the enabling server parameter.
 */
void fillWriterVectors(const OplogApplier::Options& options,
                       bool disableTransactionUpdateCoalescing,
                       OperationContext* opCtx,
                       std::vector<OplogEntry>* ops,
                       std::vector<std::vector<ApplierOperation>>* writerVectors,
                       std::vector<std::vector<OplogEntry>>* derivedOps,
                       replicated_fast_count::SizeCountCheckpointValidator* checkpointValidator);

}  // namespace repl
}  // namespace mongo
