// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/shard_role/shard_catalog/collection_metadata.h"
#include "mongo/db/shard_role/shard_catalog/type_oplog_catalog_metadata_gen.h"
#include "mongo/util/future.h"
#include "mongo/util/out_of_line_executor.h"

#include <queue>

namespace mongo {
/**
 * A class containing all necessary tools to:
 * - Apply oplog entries to the existing CollectionMetadata
 * - Recover the sharding metadata from the authoritative source of information on disk + all oplog
 *   entries that were supposed to be applied before disk recovery finished.
 *
 * The contract that users of the synchronizer should expect are as follows:
 * - The recovery should only be started once there's a guarantee the durable changes are stable on
 *   disk. That is, recovery can start within a critical section so long as no more durable changes
 *   will be done before it is released.
 * - The chosen timestamp may or may not be within a critical section.
 * - The returned metadata is not majority committed and instead should be treated with local read
 *   concern.
 * - The returned metadata may be from a time when the critical section was active.
 *
 * This class is single-shot: one start() + drainAndApply() pair per instance. If drainAndApply()
 * returns boost::none (invalidate), discard this instance and construct a new one for the next
 * round.
 *
 * Expected usage:
 *
 *     CollectionMetadataSynchronizer synchronizer;
 *     synchronizer.start();
 *     synchronizer.getMetadataFuture().get();
 *     if (auto collMetadata = synchronizer.drainAndApply()) {
 *         CSR.install(collMetadata);
 *     }
 */
class [[MONGO_MOD_PARENT_PRIVATE]] CollectionMetadataSynchronizer {
public:
    CollectionMetadataSynchronizer(const NamespaceString& nss, const CancellationToken& cancelToken)
        : _cancellationSource(cancelToken), _nss(nss) {};

    /**
     * Installs the metadata future and kicks majority-wait + on-disk catalog read on 'executor'.
     */
    void start(OperationContext* opCtx, ExecutorPtr executor);

    /**
     * Future installed by start() that resolves with the base CollectionMetadata read from disk.
     */
    SharedSemiFuture<CollectionMetadata> getMetadataFuture() const;

    /**
     * Drain and publish the latest collection metadata state to the caller. Returns boost::none if
     * an invalidate oplog entry was encountered; the caller must discard this instance and start a
     * new synchronizer round.
     *
     * The returned CollectionMetadata should be valid and return whether the collection is
     * currently untracked (no sharding metadata exists) or tracked (it's sharded and therefore
     * present on the global catalog).
     */
    boost::optional<CollectionMetadata> drainAndApply(OperationContext* opCtx);

    /**
     * Enqueues an oplog entry to apply after the disk read, or no-ops if the entry is already
     * covered by _timestampToReadAt. Materialized in drainAndApply().
     */
    void onOplogEntry(Timestamp entryTs, const InvalidateCollectionMetadataOplogEntry& entry);
    void onOplogEntry(Timestamp entryTs, const UpdateCollectionMetadataOplogEntry& entry);

private:
    // The following convention is used to denote what protects what:
    // (M) denotes protection via the _mutex
    mutable std::mutex _mutex;
    CancellationSource _cancellationSource;
    SharedSemiFuture<CollectionMetadata> _collMetadata;

    using QueuedItem =
        std::variant<InvalidateCollectionMetadataOplogEntry, UpdateCollectionMetadataOplogEntry>;
    std::queue<std::pair<Timestamp, QueuedItem>> _entriesToApply;  // (M)

    const NamespaceString _nss;
    repl::OpTime _timestampToReadAt;  // (M)
};
}  // namespace mongo
