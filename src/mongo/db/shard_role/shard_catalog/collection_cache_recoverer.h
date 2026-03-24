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

#include "mongo/db/shard_role/shard_catalog/collection_metadata.h"
#include "mongo/db/shard_role/shard_catalog/type_oplog_catalog_metadata_gen.h"

#include <queue>

namespace mongo {
/**
 * A class containing all necessary tools to:
 * - Apply oplog entries to the existing CollectionMetadata
 * - Recover the sharding metadata from the authoritative source of information on disk + all oplog
 *   entries that were supposed to be applied before disk recovery finished.
 *
 * The contract that users of the Recoverer should expect are as follows:
 * - The recovery should ony be started once there's a guarantee the durable changes are stable on
 *   disk. That is, recovery can start within a critical section so long as no more durable changes
 *   will be done before it is released.
 * - The chosen timestamp may or may not be within a critical section.
 * - The returned metadata is not majority committed and instead should be treated with local read
 *   concern.
 * - The returned metadata may be from a time when the critical section was active.
 *
 *  This class is expected to be used with two modes of operation:
 * - Applying oplog entries to a known set of collection metadata
 * - Fully recovering collection metadata from disk and catching up with any potentially concurrent
 *   oplog entries.
 *
 * In both cases the class must be used as follows:
 *
 * while (true) {
 *     auto roundId = recoverer.start();
 *     recoverer.wait(roundId);
 *     if (auto collMetadata = recoverer.drain(roundId)) {
 *         CSR.install(collMetadata);
 *         break;
 *     }
 * }
 *
 * We expect the first case to just be a single loop and for both start and wait to be immediate
 * no-ops.
 */
class MONGO_MOD_PARENT_PRIVATE CollectionCacheRecoverer {
public:
    // An opaque identifier for the round. Used in order to support concurrent access to the
    // recoverer.
    struct RecoveryRoundId {
    private:
        friend class CollectionCacheRecoverer;
        RecoveryRoundId(repl::OpTime id) : id(id) {};
        repl::OpTime id;
    };

    CollectionCacheRecoverer(const NamespaceString& nss, CollectionMetadata existingMetadata)
        : _collMetadata(
              SemiFuture<CollectionMetadata>::makeReady(std::move(existingMetadata)).share()),
          _nss(nss) {};
    CollectionCacheRecoverer(const NamespaceString& nss) : _nss(nss) {};

    RecoveryRoundId start(OperationContext* opCtx, ExecutorPtr executor);

    /**
     * Waits until the CollectionMetadata has been recovered from disk. Note that in order to get it
     * we must first call `drainAndApply` in order to drain the potentially concurrent oplog
     * entries.
     *
     * Returns a failed Status with AtomicityFailure if the current round already finished to signal
     * the caller that it has to restart the loop.
     */
    Status waitForInitialPass(OperationContext* opCtx, RecoveryRoundId recoveryRound);

    /**
     * Drain and publish the latest collection metadata state to the caller. This method can return
     * boost::none if we encountered an invalidate oplog entry during the drain that forces recovery
     * to happen again.
     *
     * The returned CollectionMetadata should be valid and return whether the collection is
     * currently untracked (no sharding metadata exists) or tracked (it's sharded and therefore
     * present on the global catalog).
     */
    boost::optional<CollectionMetadata> drainAndApply(OperationContext* opCtx,
                                                      RecoveryRoundId recoveryRound);

    /**
     * Apply the oplog entry to the CollectionMetadata. If disk recovery is taking place it will
     * instead enqueue the entry for recovery and only materialize the results once
     * CacheSynchronizer::drain is called.
     */
    void onOplogEntry(OperationContext* opCtx,
                      Timestamp entryTs,
                      const InvalidateCollectionMetadataOplogEntry& entry);
    void onOplogEntry(OperationContext* opCtx,
                      Timestamp entryTs,
                      const CollectionShardingStateDeltaOplogEntry& entry);

private:
    // The following convention is used to denote what protects what:
    // (M) denotes protection via the _mutex
    stdx::mutex _mutex;
    CancellationSource _cancellationSource;
    SharedSemiFuture<CollectionMetadata> _collMetadata;

    using QueuedItem = std::variant<InvalidateCollectionMetadataOplogEntry,
                                    CollectionShardingStateDeltaOplogEntry>;
    std::queue<std::pair<Timestamp, QueuedItem>> _entriesToApply;  // (M)

    const NamespaceString _nss;
    repl::OpTime _timestampToReadAt;  // (M)
};
}  // namespace mongo
