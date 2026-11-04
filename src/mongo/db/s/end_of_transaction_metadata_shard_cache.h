// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/platform/rwmutex.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <absl/container/flat_hash_map.h>

namespace mongo {

class ServiceContext;

/**
 * Process-wide cache recording which shards are known not to understand the end-of-transaction
 * accounting metadata ('endOfTransactionScopes') carried by the 'commitTransaction' command.
 *
 * The transaction coordinator attaches the metadata to the commit command. A shard whose binary
 * predates the metadata field rejects the command with 'IDLUnknownField'; the coordinator then
 * retries the commit without the metadata and records the shard here, so that subsequent commits to
 * that shard skip the metadata without paying for another failed round trip.
 *
 * The expected end state of a mixed-version cluster is that every shard is upgraded and thus
 * understands the metadata, so the cache only stores the exceptional shards (those known to be
 * ineligible). Looking up an unknown shard is therefore the fast path: it only needs a shared lock
 * and returns immediately.
 *
 * Eviction is purely time-based and lazy; there is no background reaper and no explicit
 * invalidation API:
 *  - An entry stores the instant at which the knowledge expires, set to the time of the last
 *    'markShardIneligible' plus 'kIneligibilityTtl'. 'isShardDefinitelyEligible' treats the shard
 * as eligible again as soon as the current time reaches that instant. The transition therefore
 * takes effect at the first lookup at or after the expiry, not at a fixed schedule.
 *  - The expired entry is not removed from the map. It lingers until it is overwritten by the next
 *    'markShardIneligible' for the same shard, so the map is bounded by the number of distinct
 *    shards ever marked ineligible, not by the number of currently-ineligible shards.
 *  - Re-marking a shard refreshes its expiry, extending it by another 'kIneligibilityTtl' from the
 *    time of the new mark.
 *  - The cache is an in-memory per-ServiceContext decoration; it is neither persisted nor
 *    replicated, so restarting the process clears it entirely.
 */
class [[MONGO_MOD_PUBLIC]] EndOfTransactionMetadataShardCache {
public:
    /**
     * How long a shard is remembered as ineligible. Long enough that the extra probe round trip is
     * amortized over many commits, and short enough that an upgraded shard is re-evaluated within a
     * reasonable time even if it was upgraded without restarting (and thus without dropping its
     * connections).
     */
    static constexpr Seconds kIneligibilityTtl{300};

    static EndOfTransactionMetadataShardCache& get(ServiceContext* service);

    /**
     * Returns true unless the shard is known, and the knowledge has not expired, to reject the
     * metadata.
     */
    bool isShardDefinitelyEligible(const ShardId& shardId) const;

    /**
     * Records that the shard rejected the metadata, for 'kIneligibilityTtl'.
     */
    void markShardIneligible(const ShardId& shardId);

private:
    Date_t _now() const;

    // Guards '_ineligibleUntil'. Reads are the common case and only take a shared lock; writes
    // happen only when a shard rejects the metadata, which is the exceptional case. 'RWMutex' is
    // optimized for frequent, short reads and infrequent writes.
    //
    // 'RWMutex' is used rather than the reader-optimized 'WriteRarelyRWMutex' because the latter
    // lets a thread hold a read lock on at most one instance at a time (across all instances) and
    // requires release on the acquiring thread. Lookups here happen on arbitrary caller threads and
    // we cannot guarantee that no caller further up the stack already holds a 'WriteRarelyRWMutex'
    // read lock, which would trip its invariant.
    mutable RWMutex _mutex;

    // Shards known to be ineligible, mapped to the time at which that knowledge expires. A shard
    // absent from the map is eligible.
    absl::flat_hash_map<ShardId, Date_t> _ineligibleUntil;
};

}  // namespace mongo
