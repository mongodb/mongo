/**
 *    Copyright (C) 2026-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

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
