/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <cstddef>
#include <cstdint>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_metadata.h"
#include "mongo/db/local_catalog/shard_role_catalog/scoped_collection_metadata.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/service_context.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/uuid.h"

#include <list>
#include <memory>
#include <mutex>
#include <utility>

namespace mongo {

class RangePreserver;

/**
 * Contains filtering metadata for a collection.
 */
class MetadataManager : public std::enable_shared_from_this<MetadataManager> {
public:
    MetadataManager(ServiceContext* serviceContext,
                    NamespaceString nss,
                    CollectionMetadata initialMetadata);
    ~MetadataManager() = default;

    MetadataManager(const MetadataManager&) = delete;
    MetadataManager& operator=(const MetadataManager&) = delete;

    /**
     * Increments the usage counter of the active metadata and returns an RAII object, which
     * corresponds to it.
     *
     * Holding a reference on a particular instance of the metadata means that orphan cleanup is not
     * allowed to run and delete chunks which are covered by that metadata. When the returned
     * ScopedCollectionDescription goes out of scope, the reference counter on the metadata will be
     * decremented and if it reaches to zero, orphan cleanup may proceed.
     */
    std::shared_ptr<ScopedCollectionDescription::Impl> getActiveMetadata(
        const boost::optional<LogicalTime>& atClusterTime, bool preserveRange);

    /**
     * Returns the placement version of the active metadata object.
     */
    ChunkVersion getActivePlacementVersion() {
        stdx::lock_guard<stdx::mutex> lg(_managerLock);
        invariant(!_metadata.empty());
        return _metadata.back()->metadata->getShardPlacementVersion();
    }

    /**
     * If the collection is tracked, returns its UUID.
     * Otherwise, it will return boost::none.
     */
    boost::optional<UUID> getCollectionUuid() const;

    /**
     * Returns the number of CollectionMetadata objects being maintained on behalf of running
     * queries.  The actual number may vary after it returns, so this is really only useful for unit
     * tests.
     */
    size_t numberOfMetadataSnapshots() const;

    /**
     * Returns the number of metadata objects that have been set to boost::none in
     * _retireExpiredMetadata(). The actual number may vary after it returns, so this is really only
     * useful for unit tests.
     */
    int numberOfEmptyMetadataSnapshots() const;

    void setFilteringMetadata(CollectionMetadata newMetadata);

    /**
     * Returns a future marked as ready when all the ongoing queries retaining the range complete
     */
    SharedSemiFuture<void> getOngoingQueriesCompletionFuture(ChunkRange const& range);

    void invalidateRangePreserversOlderThanShardVersion(OperationContext* opCtx,
                                                        const ChunkVersion& shardVersion);
    /**
     * Returns whether the active metadata has routing table.
     */
    bool hasRoutingTable();

private:
    // Management of the _metadata list is implemented in RangePreserver
    friend class RangePreserver;

    /**
     * Represents an instance of what the filtering metadata for this collection was at a particular
     * point in time along with a counter of how many queries are still using it.
     */
    struct CollectionMetadataTracker {
        CollectionMetadataTracker(const CollectionMetadataTracker&) = delete;
        CollectionMetadataTracker& operator=(const CollectionMetadataTracker&) = delete;

        CollectionMetadataTracker(CollectionMetadata inMetadata)
            : metadata(std::move(inMetadata)) {}

        ~CollectionMetadataTracker() {
            invariant(!usageCounter);
            onDestructionPromise.emplaceValue();
        }

        boost::optional<CollectionMetadata> metadata;

        /**
         * Promise that will be signaled when this object is destroyed.
         *
         * In the case where this CollectionMetadataTracker may refer to orphaned documents for one
         * or more ranges, the corresponding futures from this promise are used as barriers to
         * prevent range deletion tasks for those ranges from proceeding until this object is
         * destroyed, to guarantee that ranges aren't deleted while active queries can still access
         * them.
         */
        SharedPromise<void> onDestructionPromise;

        uint32_t usageCounter{0};

        /**
         * This flag indicates whether the MetadataTracker is still valid or if it has already been
         * invalidated during a range deletion process.
         */
        bool valid{true};
    };

    /**
     * Retires any metadata that has fallen out of use, potentially allowing range deletions to
     * proceed which were waiting for active queries using these metadata objects to complete.
     */
    void _retireExpiredMetadata(WithLock);

    /**
     * Pushes current set of chunks, if any, to _metadataInUse, replaces it with newMetadata.
     */
    void _setActiveMetadata(WithLock wl, CollectionMetadata newMetadata);

    /**
     * Finds the most-recently pushed metadata that depends on `range`, or nullptr if none. The
     * result is usable until the lock is released.
     */
    CollectionMetadataTracker* _findNewestOverlappingMetadata(WithLock, ChunkRange const& range);

    /**
     * Internal method to fetch the collection UUID when the `_managerLock` is already acquired.
     * If the collection is tracked, returns its UUID.
     * Otherwise, it will return boost::none.
     */
    boost::optional<UUID> _getCollectionUuidWithLock(WithLock wl) const;

    // ServiceContext from which to obtain instances of global support objects
    ServiceContext* const _serviceContext;

    // Namespace for which this manager object applies
    const NamespaceString _nss;

    // Mutex to protect the state below
    mutable stdx::mutex _managerLock;

    // Contains a list of collection metadata for the same collection uuid, ordered in
    // chronological order based on the refreshes that occurred. The entry at _metadata.back() is
    // the most recent metadata and is what is returned to new queries. The rest are previously
    // active collection metadata instances still in use by active server operations or cursors.
    std::list<std::shared_ptr<CollectionMetadataTracker>> _metadata;
};

}  // namespace mongo
