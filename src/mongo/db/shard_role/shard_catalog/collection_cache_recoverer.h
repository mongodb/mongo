// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
class [[MONGO_MOD_PARENT_PRIVATE]] CollectionCacheRecoverer {
public:
    // An opaque identifier for the round. Used in order to support concurrent access to the
    // recoverer.
    struct RecoveryRoundId {
    private:
        friend class CollectionCacheRecoverer;
        RecoveryRoundId(repl::OpTime id) : id(id) {};
        repl::OpTime id;
    };

    CollectionCacheRecoverer(const NamespaceString& nss,
                             const CancellationToken& cancelToken,
                             CollectionMetadata existingMetadata)
        : _cancellationSource(cancelToken),
          _collMetadata(
              SemiFuture<CollectionMetadata>::makeReady(std::move(existingMetadata)).share()),
          _nss(nss) {};
    CollectionCacheRecoverer(const NamespaceString& nss, const CancellationToken& cancelToken)
        : _cancellationSource(cancelToken), _nss(nss) {};

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
    void onOplogEntry(Timestamp entryTs, const InvalidateCollectionMetadataOplogEntry& entry);
    void onOplogEntry(Timestamp entryTs, const UpdateCollectionMetadataOplogEntry& entry);

private:
    // The following convention is used to denote what protects what:
    // (M) denotes protection via the _mutex
    std::mutex _mutex;
    CancellationSource _cancellationSource;
    SharedSemiFuture<CollectionMetadata> _collMetadata;

    using QueuedItem =
        std::variant<InvalidateCollectionMetadataOplogEntry, UpdateCollectionMetadataOplogEntry>;
    std::queue<std::pair<Timestamp, QueuedItem>> _entriesToApply;  // (M)

    const NamespaceString _nss;
    repl::OpTime _timestampToReadAt;  // (M)
};
}  // namespace mongo
