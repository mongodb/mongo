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
#include "mongo/db/s/collection_metadata.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/stdx/memory.h"

namespace mongo {

class ScopedCollectionMetadata;

class MetadataManager {
    MONGO_DISALLOW_COPYING(MetadataManager);

public:
    MetadataManager();
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
     * Changes the active metadata and if there are current users of the current metadata,
     * puts it in the _metadataInUse set.
     */
    void setActiveMetadata(std::unique_ptr<CollectionMetadata> newMetadata);

    /**
    * Adds a new range to be cleaned up.
    * The newly introduced range must not overlap with the existing ranges.
    */
    void addRangeToClean(const ChunkRange& range);

    /**
     * Removes the specified range from the ranges to be cleaned up.
     */
    void removeRangeToClean(const ChunkRange& range);

    /**
     * Gets copy of _rangesToClean map (see below).
     */
    std::map<BSONObj, ChunkRange> getCopyOfRanges();

    /*
     * Appends information on all the chunk ranges in rangesToClean to builder.
     */
    void append(BSONObjBuilder* builder);

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

    /**
     * Removes the CollectionMetadata stored in the tracker from the _metadataInUse
     * list (if it's there).
     */
    void _removeMetadata_inlock(CollectionMetadataTracker* metadataTracker);

    void _addRangeToClean_inlock(const ChunkRange& range);
    void _removeRangeToClean_inlock(const ChunkRange& range);

    std::unique_ptr<CollectionMetadataTracker> _activeMetadataTracker;

    std::list<std::unique_ptr<CollectionMetadataTracker>> _metadataInUse;

    // Contains the information of which ranges of sharding keys need to
    // be deleted from the shard. The map is from the minimum value of the
    // range to be deleted (e.g. BSON("key" << 0)) to the entire chunk range.
    std::map<BSONObj, ChunkRange> _rangesToClean;

    stdx::mutex _managerLock;
};

class ScopedCollectionMetadata {
    MONGO_DISALLOW_COPYING(ScopedCollectionMetadata);

public:
    /**
     * Creates an empty ScopedCollectionMetadata. Using the default constructor means that no
     * metadata is available.
     */
    ScopedCollectionMetadata();

    /**
     * Decrements the usageCounter and conditionally makes a call to _removeMetadata on
     * the tracker if the count has reached zero.
     */
    ~ScopedCollectionMetadata();

    ScopedCollectionMetadata(ScopedCollectionMetadata&& other);
    ScopedCollectionMetadata& operator=(ScopedCollectionMetadata&& other);

    /**
     * Dereferencing the ScopedCollectionMetadata will dereference the internal CollectionMetadata.
     */
    CollectionMetadata* operator->();
    CollectionMetadata* getMetadata();

    /** True if the ScopedCollectionMetadata stores a metadata (is not empty) */
    operator bool() const;

private:
    friend ScopedCollectionMetadata MetadataManager::getActiveMetadata();

    /**
     * Increments the counter in the CollectionMetadataTracker.
     */
    ScopedCollectionMetadata(MetadataManager* manager,
                             MetadataManager::CollectionMetadataTracker* tracker);

    MetadataManager* _manager{nullptr};
    MetadataManager::CollectionMetadataTracker* _tracker{nullptr};
};

}  // namespace mongo
