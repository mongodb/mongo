/**
 *    Copyright (C) 2012 10gen Inc.
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
 */

#pragma once

#include <string>

#include "mongo/base/status.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/jsobj.h"

namespace mongo {

    class CollectionMetadata;
    class CollectionType;
    class DBClientCursor;

    /**
     * The MetadataLoader is responsible for interfacing with the config servers and obtaining
     * the data that CollectionMetadatas are made of. Effectively, the loader is the "builder"
     * class for that metadata.
     *
     * The loader will go out of its way to try to fetch the smaller amount possible of data
     * from the config server without sacrificing the freshness and accuracy of the metadata it
     * builds. (See ConfigDiffTracker class.)
     *
     * The class is not thread safe.
     */
    class MetadataLoader {
    public:

        /**
         * Takes a connection string to the config servers to be used for loading data. Note
         * that we make no restrictions about which connection string that is, including
         * CUSTOM, which we rely on in testing.
         */
        explicit MetadataLoader( const ConnectionString& configLoc );

        ~MetadataLoader();

        /**
         * Fills a new metadata instance representing the chunkset of the collection 'ns'
         * (or its entirety, if not sharded) that lives on 'shard' with data from the config server.
         * Optionally, uses an 'oldMetadata' for the same 'ns'/'shard'; the contents of
         * 'oldMetadata' can help reducing the amount of data read from the config servers.
         *
         * OK on success.
         *
         * Failure return values:
         * Abnormal:
         * @return FailedToParse if there was an error parsing the remote config data
         * Normal:
         * @return NamespaceNotFound if the collection no longer exists
         * @return HostUnreachable if there was an error contacting the config servers
         * @return RemoteChangeDetected if the data loaded was modified by another operation
         */
        Status makeCollectionMetadata( const string& ns,
                                       const string& shard,
                                       const CollectionMetadata* oldMetadata,
                                       CollectionMetadata* metadata ) const;

    private:
        ConnectionString _configLoc;

        /**
         * Returns OK and fills in the internal state of 'metadata' with general collection
         * information, not including chunks.
         *
         * If information about the collection can be accessed or is invalid, returns:
         * @return NamespaceNotFound if the collection no longer exists
         * @return FailedToParse if there was an error parsing the remote config data
         * @return HostUnreachable if there was an error contacting the config servers
         * @return RemoteChangeDetected if the collection doc loaded is unexpectedly different
         *
         */
        Status initCollection( const string& ns,
                               const string& shard,
                               CollectionMetadata* metadata ) const;

        /**
         * Returns OK and fills in the chunk state of 'metadata' to portray the chunks of the
         * collection 'ns' that sit in 'shard'. If provided, uses the contents of 'oldMetadata'
         * as a base (see description in initCollection above).
         *
         * If information about the chunks can be accessed or is invalid, returns:
         * @return HostUnreachable if there was an error contacting the config servers
         * @return RemoteChangeDetected if the chunks loaded are unexpectedly different
         *
         * For backwards compatibility,
         * @return NamespaceNotFound if there are no chunks loaded and an epoch change is detected
         * TODO: @return FailedToParse
         */
        Status initChunks( const string& ns,
                           const string& shard,
                           const CollectionMetadata* oldMetadata,
                           CollectionMetadata* metadata ) const;
    };

} // namespace mongo
