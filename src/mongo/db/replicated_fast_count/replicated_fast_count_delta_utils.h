// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/replicated_fast_count/replicated_fast_size_count.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <vector>

#include <absl/container/flat_hash_map.h>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace replicated_fast_count {

/**
 * Returns the size and count delta extracted from the oplog entry's size metadata ('m' field), if
 * present.
 *
 * This function expects to be called on an oplog entry for a single operation. For 'applyOps'
 * entries, the top-level entry cannot have an 'm' (size metadata) field; however, the inner
 * operations within the 'applyOps' array can and should be parsed separately.
 */
boost::optional<CollectionSizeCount> extractSizeCountDeltaForOp(const repl::OplogEntry& oplogEntry);

/**
 * Aggregates per-collection size and count deltas across a list of operations. Returns one
 * `MultiOpSizeMetadata` entry per collection UUID touched. Operations without size metadata
 * (`m` field) are skipped.
 */
[[MONGO_MOD_PUBLIC]] std::vector<MultiOpSizeMetadata> aggregateMultiOpSizeMetadata(
    const std::vector<repl::ReplOperation>& ops);

[[MONGO_MOD_PUBLIC]] std::vector<MultiOpSizeMetadata> aggregateMultiOpSizeMetadata(
    const std::vector<repl::OplogEntry>& ops);

/**
 * Accumulates cumulative size and count deltas for each uuid across the inner operations of the
 * 'applyOpsEntry' into 'sizeCountDeltasOut'. Returns the number of size/count entries processed.
 *
 * The OplogEntry provided must be of type 'repl::OplogEntry::CommandType::kApplyOps'; otherwise,
 * the method throws and terminates the current operation.
 */
int extractSizeCountDeltasForApplyOps(const repl::OplogEntry& applyOpsEntry,
                                      SizeCountDeltas& sizeCountDeltasOut);

/**
 * Processes a single oplog entry and accumulates its size/count contribution into
 * 'sizeCountDeltasOut'. Handles applyOps (including nested), truncateRange, commitTransaction,
 * and CRUD operations. Returns the number of size/count entries processed.
 */
int processOplogEntry(const repl::OplogEntry& entry, SizeCountDeltas& sizeCountDeltasOut);

/**
 * Merges per-UUID deltas from 'src' into 'dst', handling DDL states (kCreated requires recording
 * a create; kDropped is not permitted as drops are disallowed in multi-document transactions).
 */
void mergeDeltas(const SizeCountDeltas& src, SizeCountDeltas& dst);

/**
 * Fast-scan eligibility predicate used inside the cursor scan's Layer 2 / Layer 2.5 fast path.
 * Equivalent to `isReplicatedFastCountEligible(nss)` for any namespace that is NOT
 * `config.fast_count_metadata_store` or `config.fast_count_metadata_store_timestamps`. Layer 1
 * and Layer 2.5 callers filter those store namespaces upstream so this helper can skip the two
 * extra `NamespaceString` constructions the canonical function performs to check for them.
 *
 * Defined inline so the fast-scan hot path in
 * `replicated_fast_count_streaming_oplog_delta_accumulator.cpp` can inline it across the
 * translation unit boundary. Exposed for parity testing against `isReplicatedFastCountEligible`;
 * the two functions must agree on every input outside the store namespaces.
 */
inline bool isFastCountEligibleNonStore(const NamespaceString& nss) {
    if (nss.isOplog()) {
        return true;
    }
    return !nss.isLocalDB() && !nss.isImplicitlyReplicated() &&
        !nss.isServerConfigurationCollection() && !nss.isSystemDotProfile();
}
}  // namespace replicated_fast_count


}  // namespace mongo
