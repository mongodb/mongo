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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/metadata_loader.h"

#include <vector>

#include "mongo/client/dbclientinterface.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/repl/replication_coordinator_impl.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/db/s/shard_metadata_util.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/unique_message.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_shard_collection.h"
#include "mongo/s/chunk_diff.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/util/log.h"

namespace mongo {

using std::make_pair;
using std::map;
using std::pair;
using std::string;

namespace {

/**
 * This is an adapter so we can use config diffs - mongos and mongod do them slightly
 * differently.
 *
 * The mongod adapter here tracks only a single shard, and stores ranges by (min, max).
 */
class SCMConfigDiffTracker : public ConfigDiffTracker<CachedChunkInfo> {
public:
    SCMConfigDiffTracker(const std::string& ns,
                         RangeMap* currMap,
                         ChunkVersion* maxVersion,
                         MaxChunkVersionMap* maxShardVersions,
                         const ShardId& currShard)
        : ConfigDiffTracker<CachedChunkInfo>(ns, currMap, maxVersion, maxShardVersions),
          _currShard(currShard) {}

    virtual bool isTracked(const ChunkType& chunk) const {
        return chunk.getShard() == _currShard;
    }

    virtual pair<BSONObj, CachedChunkInfo> rangeFor(OperationContext* opCtx,
                                                    const ChunkType& chunk) const {
        return make_pair(chunk.getMin(), CachedChunkInfo(chunk.getMax(), chunk.getVersion()));
    }

    virtual ShardId shardFor(OperationContext* opCtx, const ShardId& name) const {
        return name;
    }

    virtual string nameFrom(const string& shard) const {
        return shard;
    }

private:
    const ShardId _currShard;
};

}  // namespace

Status MetadataLoader::makeCollectionMetadata(OperationContext* opCtx,
                                              ShardingCatalogClient* catalogClient,
                                              const string& ns,
                                              const string& shard,
                                              const CollectionMetadata* oldMetadata,
                                              CollectionMetadata* metadata) {
    invariant(serverGlobalParams.clusterRole == ClusterRole::ShardServer);

    bool isShardPrimary = false;
    if (repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesForDatabase_UNSAFE(opCtx, ns)) {
        isShardPrimary = true;
    }

    Status initCollectionStatus =
        _initCollection(opCtx, catalogClient, ns, metadata, isShardPrimary);
    if (!initCollectionStatus.isOK()) {
        return initCollectionStatus;
    }

    return _initChunks(opCtx, catalogClient, ns, shard, oldMetadata, metadata, isShardPrimary);
}


Status MetadataLoader::_initCollection(OperationContext* opCtx,
                                       ShardingCatalogClient* catalogClient,
                                       const string& ns,
                                       CollectionMetadata* metadata,
                                       bool isShardPrimary) {
    StatusWith<std::pair<BSONObj, OID>> statusWithShardKeyAndEpoch =
        shardmetadatautil::getCollectionShardKeyAndEpoch(
            opCtx, catalogClient, NamespaceString(ns), isShardPrimary);
    if (!statusWithShardKeyAndEpoch.isOK()) {
        return statusWithShardKeyAndEpoch.getStatus();
    }
    std::pair<BSONObj, OID> shardKeyAndEpoch = statusWithShardKeyAndEpoch.getValue();

    metadata->_keyPattern = shardKeyAndEpoch.first;
    metadata->fillKeyPatternFields();
    metadata->_shardVersion = ChunkVersion(0, 0, shardKeyAndEpoch.second);
    metadata->_collVersion = ChunkVersion(0, 0, shardKeyAndEpoch.second);

    return Status::OK();
}

Status MetadataLoader::_initChunks(OperationContext* opCtx,
                                   ShardingCatalogClient* catalogClient,
                                   const string& ns,
                                   const string& shard,
                                   const CollectionMetadata* oldMetadata,
                                   CollectionMetadata* metadata,
                                   bool isShardPrimary) {
    const OID epoch = metadata->getCollVersion().epoch();

    SCMConfigDiffTracker::MaxChunkVersionMap versionMap;
    versionMap[shard] = metadata->_shardVersion;

    bool fullReload = true;

    // Check to see if we should use the old version or not.
    if (oldMetadata) {
        // If our epochs are compatible, it's useful to use the old metadata for diffs: this leads
        // to a performance gain because not all the chunks must be reloaded, just the ones this
        // shard has not seen -- they will have higher versions than present in oldMetadata.
        if (oldMetadata->getCollVersion().hasEqualEpoch(epoch)) {
            fullReload = false;
            invariant(oldMetadata->isValid());

            versionMap[shard] = oldMetadata->_shardVersion;
            metadata->_collVersion = oldMetadata->_collVersion;

            // TODO: This could be made more efficient if copying not required, but
            // not as frequently reloaded as in mongos.
            metadata->_chunksMap = oldMetadata->_chunksMap;

            LOG(2) << "loading new chunks for collection " << ns
                   << " using old metadata w/ version " << oldMetadata->getShardVersion() << " and "
                   << metadata->_chunksMap.size() << " chunks";
        } else {
            log() << "reloading collection metadata for " << ns << " with new epoch "
                  << epoch.toString() << ", the current epoch is "
                  << oldMetadata->getCollVersion().epoch().toString();
        }
    }

    // Exposes the new metadata's range map and version to the "differ" which would ultimately be
    // responsible for filling them up
    SCMConfigDiffTracker differ(
        ns, &metadata->_chunksMap, &metadata->_collVersion, &versionMap, shard);

    try {
        auto statusWithChunks = shardmetadatautil::getChunks(
            opCtx, catalogClient, NamespaceString(ns), metadata->_collVersion, isShardPrimary);
        if (!statusWithChunks.isOK()) {
            return statusWithChunks.getStatus();
        }
        auto chunks = statusWithChunks.getValue();

        //
        // The diff tracker should always find at least one chunk (the highest chunk we saw
        // last time).  If not, something has changed on the config server (potentially between
        // when we read the collection data and when we read the chunks data).
        //
        int diffsApplied = differ.calculateConfigDiff(opCtx, chunks);
        if (diffsApplied > 0) {
            // Chunks found, return ok
            LOG(2) << "loaded " << diffsApplied << " chunks into new metadata for " << ns
                   << " with version " << metadata->_collVersion;

            // If the last chunk was moved off of this shard, the shardVersion should be reset to
            // zero (if we did not conduct a full reload and oldMetadata was present,
            // versionMap[shard] was previously set to the oldMetadata's shardVersion for
            // performance gains).
            if (!fullReload && metadata->_chunksMap.empty()) {
                versionMap[shard] = ChunkVersion(0, 0, epoch);
            }

            metadata->_shardVersion = versionMap[shard];
            metadata->fillRanges();

            invariant(metadata->isValid());
            return Status::OK();
        } else if (diffsApplied == 0) {
            // No chunks found, the collection is dropping or we're confused
            // If this is a full reload, assume it is a drop for backwards compatibility
            // TODO: drop the config.collections entry *before* the chunks and eliminate this
            // ambiguity
            return {fullReload ? ErrorCodes::NamespaceNotFound : ErrorCodes::RemoteChangeDetected,
                    str::stream() << "No chunks found when reloading " << ns
                                  << ", previous version was "
                                  << metadata->_collVersion.toString()
                                  << (fullReload ? ", this is a drop" : "")};
        } else {
            // Invalid chunks found, our epoch may have changed because we dropped/recreated the
            // collection
            return {ErrorCodes::RemoteChangeDetected,
                    str::stream() << "Invalid chunks found when reloading " << ns
                                  << ", previous version was "
                                  << metadata->_collVersion.toString()
                                  << ", this should be rare"};
        }
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

}  // namespace mongo
