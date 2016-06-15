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

#include "mongo/db/s/collection_metadata.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/chunk_diff.h"
#include "mongo/s/chunk_version.h"
#include "mongo/util/log.h"

namespace mongo {

using std::unique_ptr;
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
class SCMConfigDiffTracker : public ConfigDiffTracker<BSONObj> {
public:
    SCMConfigDiffTracker(const ShardId& currShard) : _currShard(currShard) {}

    virtual bool isTracked(const ChunkType& chunk) const {
        return chunk.getShard() == _currShard;
    }

    virtual pair<BSONObj, BSONObj> rangeFor(OperationContext* txn, const ChunkType& chunk) const {
        return make_pair(chunk.getMin(), chunk.getMax());
    }

    virtual ShardId shardFor(OperationContext* txn, const ShardId& name) const {
        return name;
    }

    virtual string nameFrom(const string& shard) const {
        return shard;
    }

private:
    const ShardId _currShard;
};

}  // namespace

//
// MetadataLoader implementation
//

MetadataLoader::MetadataLoader() = default;

MetadataLoader::~MetadataLoader() = default;

Status MetadataLoader::makeCollectionMetadata(OperationContext* txn,
                                              ShardingCatalogClient* catalogClient,
                                              const string& ns,
                                              const string& shard,
                                              const CollectionMetadata* oldMetadata,
                                              CollectionMetadata* metadata) const {
    Status status = _initCollection(txn, catalogClient, ns, shard, metadata);
    if (!status.isOK() || metadata->getKeyPattern().isEmpty()) {
        return status;
    }

    return initChunks(txn, catalogClient, ns, shard, oldMetadata, metadata);
}

Status MetadataLoader::_initCollection(OperationContext* txn,
                                       ShardingCatalogClient* catalogClient,
                                       const string& ns,
                                       const string& shard,
                                       CollectionMetadata* metadata) const {
    auto coll = catalogClient->getCollection(txn, ns);
    if (!coll.isOK()) {
        return coll.getStatus();
    }

    const auto& collInfo = coll.getValue().value;
    if (collInfo.getDropped()) {
        return Status(ErrorCodes::NamespaceNotFound,
                      str::stream() << "could not load metadata, collection " << ns
                                    << " was dropped");
    }

    metadata->_keyPattern = collInfo.getKeyPattern().toBSON();
    metadata->fillKeyPatternFields();
    metadata->_shardVersion = ChunkVersion(0, 0, collInfo.getEpoch());
    metadata->_collVersion = ChunkVersion(0, 0, collInfo.getEpoch());

    return Status::OK();
}

Status MetadataLoader::initChunks(OperationContext* txn,
                                  ShardingCatalogClient* catalogClient,
                                  const string& ns,
                                  const string& shard,
                                  const CollectionMetadata* oldMetadata,
                                  CollectionMetadata* metadata) const {
    map<ShardId, ChunkVersion> versionMap;  // TODO: use .h defined type

    // Preserve the epoch
    versionMap[shard] = metadata->_shardVersion;
    OID epoch = metadata->getCollVersion().epoch();
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
            warning() << "reloading collection metadata for " << ns << " with new epoch "
                      << epoch.toString() << ", the current epoch is "
                      << oldMetadata->getCollVersion().epoch().toString();
        }
    }


    // Exposes the new metadata's range map and version to the "differ," who
    // would ultimately be responsible of filling them up.
    SCMConfigDiffTracker differ(shard);
    differ.attach(ns, metadata->_chunksMap, metadata->_collVersion, versionMap);

    try {
        std::vector<ChunkType> chunks;
        const auto diffQuery = differ.configDiffQuery();
        Status status = catalogClient->getChunks(
            txn, diffQuery.query, diffQuery.sort, boost::none, &chunks, nullptr);
        if (!status.isOK()) {
            if (status == ErrorCodes::HostUnreachable) {
                // Make our metadata invalid
                metadata->_collVersion = ChunkVersion(0, 0, OID());
                metadata->_chunksMap.clear();
            }
            return status;
        }

        //
        // The diff tracker should always find at least one chunk (the highest chunk we saw
        // last time).  If not, something has changed on the config server (potentially between
        // when we read the collection data and when we read the chunks data).
        //
        int diffsApplied = differ.calculateConfigDiff(txn, chunks);
        if (diffsApplied > 0) {
            // Chunks found, return ok
            LOG(2) << "loaded " << diffsApplied << " chunks into new metadata for " << ns
                   << " with version " << metadata->_collVersion;

            // If the last chunk was moved off of this shard, the shardVersion should be reset to
            // zero (if we did not conduct a full reload and oldMetadata was present,
            // versionMap[shard] was previously set to the oldMetadata's shardVersion for
            // performance gains).
            if (!fullReload && metadata->_chunksMap.size() == 0) {
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

            string errMsg = str::stream()
                << "no chunks found when reloading " << ns << ", previous version was "
                << metadata->_collVersion.toString() << (fullReload ? ", this is a drop" : "");

            warning() << errMsg;

            metadata->_collVersion = ChunkVersion(0, 0, OID());
            metadata->_chunksMap.clear();

            return fullReload ? Status(ErrorCodes::NamespaceNotFound, errMsg)
                              : Status(ErrorCodes::RemoteChangeDetected, errMsg);
        } else {
            // Invalid chunks found, our epoch may have changed because we dropped/recreated
            // the collection.
            string errMsg = str::stream()
                << "invalid chunks found when reloading " << ns << ", previous version was "
                << metadata->_collVersion.toString() << ", this should be rare";
            warning() << errMsg;

            metadata->_collVersion = ChunkVersion(0, 0, OID());
            metadata->_chunksMap.clear();

            return Status(ErrorCodes::RemoteChangeDetected, errMsg);
        }
    } catch (const DBException& e) {
        // We deliberately do not return connPtr to the pool, since it was involved with the
        // error here.
        return Status(ErrorCodes::HostUnreachable,
                      str::stream() << "problem querying chunks metadata" << causedBy(e));
    }
}

Status MetadataLoader::promotePendingChunks(const CollectionMetadata* afterMetadata,
                                            CollectionMetadata* remoteMetadata) const {
    // Ensure pending chunks are applicable
    bool notApplicable = (NULL == afterMetadata || NULL == remoteMetadata) ||
        (afterMetadata->getShardVersion() > remoteMetadata->getShardVersion()) ||
        (afterMetadata->getShardVersion().epoch() != remoteMetadata->getShardVersion().epoch());
    if (notApplicable)
        return Status::OK();

    // The chunks from remoteMetadata are the latest version, and the pending chunks
    // from afterMetadata are the latest version.  If no trickery is afoot, pending chunks
    // should match exactly zero or one loaded chunk.

    remoteMetadata->_pendingMap = afterMetadata->_pendingMap;

    // Resolve our pending chunks against the chunks we've loaded
    for (RangeMap::iterator it = remoteMetadata->_pendingMap.begin();
         it != remoteMetadata->_pendingMap.end();) {
        if (!rangeMapOverlaps(remoteMetadata->_chunksMap, it->first, it->second)) {
            ++it;
            continue;
        }

        // Our pending range overlaps at least one chunk

        if (rangeMapContains(remoteMetadata->_chunksMap, it->first, it->second)) {
            // Chunk was promoted from pending, successful migration
            LOG(2) << "verified chunk " << rangeToString(it->first, it->second)
                   << " was migrated earlier to this shard";

            remoteMetadata->_pendingMap.erase(it++);
        } else {
            // Something strange happened, maybe manual editing of config?
            RangeVector overlap;
            getRangeMapOverlap(remoteMetadata->_chunksMap, it->first, it->second, &overlap);

            string errMsg = str::stream()
                << "the remote metadata changed unexpectedly, pending range "
                << rangeToString(it->first, it->second)
                << " does not exactly overlap loaded chunks " << overlapToString(overlap);

            return Status(ErrorCodes::RemoteChangeDetected, errMsg);
        }
    }

    return Status::OK();
}


}  // namespace mongo
