/**
 * Copyright (C) 2013 10gen Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <map>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/s/chunk.h"
#include "mongo/s/shard.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/ns_targeter.h"

namespace mongo {

    class TargeterStats;

    /**
     * NSTargeter based on a ChunkManager implementation.  Wraps all exception codepaths and
     * returns DatabaseNotFound statuses on applicable failures.
     *
     * Must be initialized before use, and initialization may fail.
     */
    class ChunkManagerTargeter : public NSTargeter {
    public:

        ChunkManagerTargeter();

        /**
         * Initializes the ChunkManagerTargeter with the latest targeting information for the
         * namespace.  May need to block and load information from a remote config server.
         *
         * Returns !OK if the information could not be initialized.
         */
        Status init( const NamespaceString& nss );

        const NamespaceString& getNS() const;

        Status targetDoc( const BSONObj& doc, ShardEndpoint** endpoint ) const;

        Status targetQuery( const BSONObj& query, std::vector<ShardEndpoint*>* endpoints ) const;

        void noteStaleResponse( const ShardEndpoint& endpoint, const BSONObj& staleInfo );

        void noteCouldNotTarget();

        /**
         * Replaces the targeting information with the latest information from the cache.  If this
         * information is stale WRT the noted stale responses or a remote refresh is needed due
         * to a targeting failure, will contact the config servers to reload the metadata.
         *
         * Also see NSTargeter::refreshIfNeeded().
         */
        Status refreshIfNeeded();

        /**
         * Returns the stats. Note that the returned stats object is still owned by this targeter.
         */
        const TargeterStats* getStats() const;

    private:

        // Different ways we can refresh metadata
        // TODO: Improve these ways.
        enum RefreshType {
            // No refresh is needed
            RefreshType_None,
            // The version has gone up, but the collection hasn't been dropped
            RefreshType_RefreshChunkManager,
            // The collection may have been dropped, so we need to reload the db
            RefreshType_ReloadDatabase
        };

        /**
         * Performs an actual refresh from the config server.
         */
        Status refreshNow( RefreshType refreshType );

        NamespaceString _nss;

        // Zero or one of these are filled at all times
        // If sharded, _manager, if unsharded, _primary, on error, neither
        ChunkManagerPtr _manager;
        ShardPtr _primary;

        // Map of shard->remote shard version reported from stale errors
        typedef std::map<std::string, ChunkVersion> ShardVersionMap;
        ShardVersionMap _remoteShardVersions;

        // Stores whether we need to check the remote server on refresh
        bool _needsTargetingRefresh;

        // Represents only the view and not really part of the targeter state.
        mutable boost::scoped_ptr<TargeterStats> _stats;
    };

    struct TargeterStats {
        // Map of chunk shard minKey -> approximate delta. This is used for deciding
        // whether a chunk might need splitting or not.
        std::map<BSONObj, int> chunkSizeDelta;
    };

} // namespace mongo
