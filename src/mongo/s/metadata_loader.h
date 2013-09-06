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
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
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
     * The MetadataLoader is responsible for interfacing with the config servers and previous
     * metadata to build new instances of CollectionMetadata.  MetadataLoader is the "builder"
     * class for metadata.
     *
     * CollectionMetadata has both persisted and volatile state (for now) - the persisted 
     * config server chunk state and the volatile pending state which is only tracked locally
     * while a server is the primary.  This requires a two-step loading process - the persisted
     * chunk state *cannot* be loaded in a DBWrite lock while the pending chunk state *must* be.
     * 
     * Example usage: 
     * beforeMetadata = <get latest local metadata>;
     * remoteMetadata = makeCollectionMetadata( beforeMetadata, remoteMetadata );
     * DBWrite lock( ns );
     * afterMetadata = <get latest local metadata>;
     * promotePendingChunks( afterMetadata, remoteMetadata );
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
         * Locking note:
         *    + Must not be called in a DBLock, since this loads over the network
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

        /**
         * Replaces the pending chunks of the remote metadata with the more up-to-date pending
         * chunks of the 'after' metadata (metadata from after the remote load), and removes pending
         * chunks which are now regular chunks.
         *
         * Pending chunks should always correspond to one or zero chunks in the remoteMetadata
         * if the epochs are the same and the remote version is the same or higher, otherwise they
         * are not applicable.
         *
         * Locking note:
         *    + Must be called in a DBLock, to ensure validity of afterMetadata
         *
         * Returns OK if pending chunks correctly follow the rule above or are not applicable 
         * Returns RemoteChangeDetected if pending chunks do not follow the rule above, indicating
         *                              either the config server or us has changed unexpectedly.
         *                              This should only occur with manual editing of the config
         *                              server.
         *
         * TODO:  This is a bit ugly but necessary for now.  If/when pending chunk info is stored on
         * the config server, this should go away.
         */
        Status promotePendingChunks( const CollectionMetadata* afterMetadata,
                                     CollectionMetadata* remoteMetadata ) const;

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
