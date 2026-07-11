// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/shard_role/shard_catalog/uncommitted_multikey.h"

#include <memory>

namespace mongo {

class OperationContext;
class TxnWildcardMultikeyPaths;

/**
 * Aggregates per-transaction multikey state behind a single RecoveryUnit::Snapshot decoration slot.
 * This is an optimization to avoid the overhead of multiple Snapshot decorations in the hot-path.
 */
struct MultikeyState {
    // The uncommitted multikey state for the current snapshot.
    UncommittedMultikey uncommittedMultikey;
    // Side-committed (via setMultikeyMetadata oplog entry) wildcard multikey state for
    // multi-document transactions. Intentionally lazy initialized to avoid construction cost in hot
    // path outside of multi-document transactions.
    std::unique_ptr<TxnWildcardMultikeyPaths> wildcardPaths;
};

MultikeyState& getMultikeyState(OperationContext* opCtx);

}  // namespace mongo
