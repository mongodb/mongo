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
#include <vector>

#include "mongo/base/status.h"

namespace mongo {

class ShardingCatalogClient;
class ChunkType;
class CollectionMetadata;
class CollectionType;
class NamespaceString;
class OID;
class OperationContext;

/**
 * The MetadataLoader is responsible for interfacing with the config servers and previous
 * metadata to build new instances of CollectionMetadata.  MetadataLoader is the "builder"
 * class for metadata.
 *
 * CollectionMetadata has both persisted and volatile state (for now) - the persisted
 * config server chunk state and the volatile pending state which is only tracked locally
 * while a server is the primary.  This requires a two-step loading process - the persisted
 * chunk state *cannot* be loaded in a DBLock lock while the pending chunk state *must* be.
 *
 * Example usage:
 * beforeMetadata = <get latest local metadata>;
 * remoteMetadata = makeCollectionMetadata( beforeMetadata, remoteMetadata );
 * DBLock lock(txn, dbname, MODE_X);
 * afterMetadata = <get latest local metadata>;
 *
 * The loader will go out of its way to try to fetch the smaller amount possible of data
 * from the config server without sacrificing the freshness and accuracy of the metadata it
 * builds. (See ConfigDiffTracker class.)
 *
 */
class MetadataLoader {
public:
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
    static Status makeCollectionMetadata(OperationContext* txn,
                                         ShardingCatalogClient* catalogClient,
                                         const std::string& ns,
                                         const std::string& shard,
                                         const CollectionMetadata* oldMetadata,
                                         CollectionMetadata* metadata);

private:
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
    static Status _initCollection(OperationContext* txn,
                                  ShardingCatalogClient* catalogClient,
                                  const std::string& ns,
                                  const std::string& shard,
                                  CollectionMetadata* metadata);

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
    static Status _initChunks(OperationContext* txn,
                              ShardingCatalogClient* catalogClient,
                              const std::string& ns,
                              const std::string& shard,
                              const CollectionMetadata* oldMetadata,
                              CollectionMetadata* metadata);


    /**
     * Takes a vector of 'chunks' and updates the config.chunks.ns collection specified by 'nss'.
     * Any chunk documents in config.chunks.ns that overlap with a chunk in 'chunks' is removed
     * as the new chunk document is inserted. If the epoch of any chunk in 'chunks' does not match
     * 'currEpoch', the chunk metadata is dropped.
     *
     * @nss - the regular collection namespace for which chunk metadata is being updated.
     * @chunks - a range of chunks retrieved from the config server, sorted in ascending chunk
     * version order.
     * @currEpoch - what this shard server knows to be the collection epoch.
     *
     * Returns:
     * - OK if not primary and no writes are needed.
     * - RemoteChangeDetected if the chunk version epoch of any chunk in 'chunks' is different than
     * 'currEpoch'
     * - Other errors in writes/reads to the config.chunks.ns collection fails.
     */
    static Status _writeNewChunksIfPrimary(OperationContext* txn,
                                           const NamespaceString& nss,
                                           const std::vector<ChunkType>& chunks,
                                           const OID& currEpoch);
};

}  // namespace mongo
