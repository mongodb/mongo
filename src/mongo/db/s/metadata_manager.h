/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <list>
#include <memory>

#include "mongo/base/disallow_copying.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/db/service_context.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/util/concurrency/notification.h"

#include "mongo/stdx/memory.h"

namespace mongo {

class ScopedCollectionMetadata;

class MetadataManager {
    MONGO_DISALLOW_COPYING(MetadataManager);

public:
    MetadataManager(ServiceContext* sc, NamespaceString nss);
    ~MetadataManager();

    /**
     * An ActiveMetadata must be set before this function can be called.
     *
     * Increments the usage counter of the active metadata and returns an RAII object, which
     * contains the currently active metadata.  When the usageCounter goes to zero, the RAII
     * object going out of scope will call _removeMetadata.
     */
    ScopedCollectionMetadata getActiveMetadata();

    /**
     * Uses the contents of the specified metadata as a way to purge any pending chunks.
     */
    void refreshActiveMetadata(std::unique_ptr<CollectionMetadata> newMetadata);

    /**
     * Puts the specified range on the list of chunks, which are being received so that the range
     * deleter process will not clean the partially migrated data.
     */
    void beginReceive(const ChunkRange& range);

    /**
     * Removes a range from the list of chunks, which are being received. Used externally to
     * indicate that a chunk migration failed.
     */
    void forgetReceive(const ChunkRange& range);

    /**
     * Gets copy of the set of chunk ranges which are being received for this collection. This
     * method is intended for testing purposes only and should not be used in any production code.
     */
    RangeMap getCopyOfReceivingChunks();

    /**
    * Adds a new range to be cleaned up.
    * The newly introduced range must not overlap with the existing ranges.
    */
    std::shared_ptr<Notification<Status>> addRangeToClean(const ChunkRange& range);

    /**
     * Calls removeRangeToClean with Status::OK.
     */
    void removeRangeToClean(const ChunkRange& range) {
        removeRangeToClean(range, Status::OK());
    }

    /**
     * Removes the specified range from the ranges to be cleaned up.
     * The specified deletionStatus will be returned to callers waiting
     * on whether the deletion succeeded or failed.
     */
    void removeRangeToClean(const ChunkRange& range, Status deletionStatus);

    /**
     * Gets copy of the set of chunk ranges which are scheduled for cleanup.
     * Converts RangeToCleanMap to RangeMap.
     */
    RangeMap getCopyOfRangesToClean();

    /**
     * Appends information on all the chunk ranges in rangesToClean to builder.
     */
    void append(BSONObjBuilder* builder);

    /**
     * Returns true if _rangesToClean is not empty.
     */
    bool hasRangesToClean();

    /**
     * Returns true if the exact range is in _rangesToClean.
     */
    bool isInRangesToClean(const ChunkRange& range);

    /**
     * Gets and returns, but does not remove, a single ChunkRange from _rangesToClean.
     * Should not be called if _rangesToClean is empty: it will hit an invariant.
     */
    ChunkRange getNextRangeToClean();

private:
    friend class ScopedCollectionMetadata;

    struct CollectionMetadataTracker {
    public:
        /**
         * Creates a new CollectionMetadataTracker, with the usageCounter initialized to zero.
         */
        CollectionMetadataTracker(std::unique_ptr<CollectionMetadata> m);

        std::unique_ptr<CollectionMetadata> metadata;

        uint32_t usageCounter{0};
    };

    // Class for the value of the _rangesToClean map. Used because callers of addRangeToClean
    // sometimes need to wait until a range is deleted. Thus, complete(Status) is called
    // when the range is deleted from _rangesToClean in removeRangeToClean(), letting callers
    // of addRangeToClean know if the deletion succeeded or failed.
    class RangeToCleanDescriptor {
    public:
        /**
         * Initializes a RangeToCleanDescriptor with an empty notification.
         */
        RangeToCleanDescriptor(BSONObj max)
            : _max(max.getOwned()), _notification(std::make_shared<Notification<Status>>()) {}

        /**
         * Gets the maximum value of the range to be deleted.
         */
        const BSONObj& getMax() const {
            return _max;
        }

        // See comment on _notification.
        std::shared_ptr<Notification<Status>> getNotification() {
            return _notification;
        }

        /**
         * Sets the status on _notification. This will tell threads
         * waiting on the value of status that the deletion succeeded or failed.
         */
        void complete(Status status) {
            _notification->set(status);
        }

    private:
        // The maximum value of the range to be deleted.
        BSONObj _max;

        // This _notification will be set with a value indicating whether the deletion
        // succeeded or failed.
        std::shared_ptr<Notification<Status>> _notification;
    };

    /**
     * Removes the CollectionMetadata stored in the tracker from the _metadataInUse
     * list (if it's there).
     */
    void _removeMetadata_inlock(CollectionMetadataTracker* metadataTracker);

    std::shared_ptr<Notification<Status>> _addRangeToClean_inlock(const ChunkRange& range);

    void _removeRangeToClean_inlock(const ChunkRange& range, Status deletionStatus);

    RangeMap _getCopyOfRangesToClean_inlock();

    void _setActiveMetadata_inlock(std::unique_ptr<CollectionMetadata> newMetadata);

    const NamespaceString _nss;

    // ServiceContext from which to obtain instances of global support objects.
    ServiceContext* _serviceContext;

    // Mutex to protect the state below
    stdx::mutex _managerLock;

    // Holds the collection metadata, which is currently active
    std::unique_ptr<CollectionMetadataTracker> _activeMetadataTracker;

    // Holds collection metadata instances, which have previously been active, but are still in use
    // by still active server operations or cursors
    std::list<std::unique_ptr<CollectionMetadataTracker>> _metadataInUse;

    // Chunk ranges which are currently assumed to be transferred to the shard. Indexed by the min
    // key of the range.
    RangeMap _receivingChunks;

    // Set of ranges to be deleted. Indexed by the min key of the range.
    typedef BSONObjIndexedMap<RangeToCleanDescriptor> RangeToCleanMap;
    RangeToCleanMap _rangesToClean;
};

class ScopedCollectionMetadata {
    MONGO_DISALLOW_COPYING(ScopedCollectionMetadata);

public:
    /**
     * Creates an empty ScopedCollectionMetadata. Using the default constructor means that no
     * metadata is available.
     */
    ScopedCollectionMetadata();

    ~ScopedCollectionMetadata();

    ScopedCollectionMetadata(ScopedCollectionMetadata&& other);
    ScopedCollectionMetadata& operator=(ScopedCollectionMetadata&& other);

    /**
     * Dereferencing the ScopedCollectionMetadata will dereference the internal CollectionMetadata.
     */
    CollectionMetadata* operator->();
    CollectionMetadata* getMetadata();

    /**
     * True if the ScopedCollectionMetadata stores a metadata (is not empty)
     */
    operator bool() const;

private:
    friend ScopedCollectionMetadata MetadataManager::getActiveMetadata();

    /**
     * Increments the counter in the CollectionMetadataTracker.
     */
    ScopedCollectionMetadata(MetadataManager* manager,
                             MetadataManager::CollectionMetadataTracker* tracker);

    /**
     * Decrements the usageCounter and conditionally makes a call to _removeMetadata on
     * the tracker if the count has reached zero.
     */
    void _decrementUsageCounter();

    MetadataManager* _manager{nullptr};
    MetadataManager::CollectionMetadataTracker* _tracker{nullptr};
};

}  // namespace mongo
