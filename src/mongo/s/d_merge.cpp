/**
 *    Copyright (C) 2013-2015 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include <vector>

#include "mongo/db/client.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/s/catalog/catalog_manager.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/chunk.h"
#include "mongo/s/config.h"
#include "mongo/s/catalog/dist_lock_manager.h"
#include "mongo/s/grid.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::shared_ptr;
using std::string;
using mongoutils::str::stream;

static Status runApplyOpsCmd(const std::vector<ChunkType>&,
                             const ChunkVersion&,
                             const ChunkVersion&);

static BSONObj buildMergeLogEntry(const std::vector<ChunkType>&,
                                  const ChunkVersion&,
                                  const ChunkVersion&);

bool mergeChunks(OperationContext* txn,
                 const NamespaceString& nss,
                 const BSONObj& minKey,
                 const BSONObj& maxKey,
                 const OID& epoch,
                 string* errMsg) {
    // Get the distributed lock
    string whyMessage = stream() << "merging chunks in " << nss.ns() << " from " << minKey << " to "
                                 << maxKey;
    auto scopedDistLock = grid.catalogManager()->getDistLockManager()->lock(nss.ns(), whyMessage);

    if (!scopedDistLock.isOK()) {
        *errMsg = stream() << "could not acquire collection lock for " << nss.ns()
                           << " to merge chunks in [" << minKey << "," << maxKey << ")"
                           << causedBy(scopedDistLock.getStatus());

        warning() << *errMsg;
        return false;
    }

    ShardingState* shardingState = ShardingState::get(txn);

    //
    // We now have the collection lock, refresh metadata to latest version and sanity check
    //

    ChunkVersion shardVersion;
    Status status = shardingState->refreshMetadataNow(txn, nss.ns(), &shardVersion);

    if (!status.isOK()) {
        *errMsg = str::stream() << "could not merge chunks, failed to refresh metadata for "
                                << nss.ns() << causedBy(status.reason());

        warning() << *errMsg;
        return false;
    }

    if (epoch.isSet() && shardVersion.epoch() != epoch) {
        *errMsg = stream() << "could not merge chunks, collection " << nss.ns() << " has changed"
                           << " since merge was sent"
                           << "(sent epoch : " << epoch.toString()
                           << ", current epoch : " << shardVersion.epoch().toString() << ")";

        warning() << *errMsg;
        return false;
    }

    shared_ptr<CollectionMetadata> metadata = shardingState->getCollectionMetadata(nss.ns());

    if (!metadata || metadata->getKeyPattern().isEmpty()) {
        *errMsg = stream() << "could not merge chunks, collection " << nss.ns()
                           << " is not sharded";

        warning() << *errMsg;
        return false;
    }

    dassert(metadata->getShardVersion().equals(shardVersion));

    if (!metadata->isValidKey(minKey) || !metadata->isValidKey(maxKey)) {
        *errMsg = stream() << "could not merge chunks, the range " << rangeToString(minKey, maxKey)
                           << " is not valid"
                           << " for collection " << nss.ns() << " with key pattern "
                           << metadata->getKeyPattern();

        warning() << *errMsg;
        return false;
    }

    //
    // Get merged chunk information
    //

    ChunkVersion mergeVersion = metadata->getCollVersion();
    mergeVersion.incMinor();

    std::vector<ChunkType> chunksToMerge;

    ChunkType itChunk;
    itChunk.setMin(minKey);
    itChunk.setMax(minKey);
    itChunk.setNS(nss.ns());
    itChunk.setShard(shardingState->getShardName());

    while (itChunk.getMax().woCompare(maxKey) < 0 &&
           metadata->getNextChunk(itChunk.getMax(), &itChunk)) {
        chunksToMerge.push_back(itChunk);
    }

    if (chunksToMerge.empty()) {
        *errMsg = stream() << "could not merge chunks, collection " << nss.ns()
                           << " range starting at " << minKey << " and ending at " << maxKey
                           << " does not belong to shard " << shardingState->getShardName();

        warning() << *errMsg;
        return false;
    }

    //
    // Validate the range starts and ends at chunks and has no holes, error if not valid
    //

    BSONObj firstDocMin = chunksToMerge.front().getMin();
    BSONObj firstDocMax = chunksToMerge.front().getMax();
    // minKey is inclusive
    bool minKeyInRange = rangeContains(firstDocMin, firstDocMax, minKey);

    if (!minKeyInRange) {
        *errMsg = stream() << "could not merge chunks, collection " << nss.ns()
                           << " range starting at " << minKey << " does not belong to shard "
                           << shardingState->getShardName();

        warning() << *errMsg;
        return false;
    }

    BSONObj lastDocMin = chunksToMerge.back().getMin();
    BSONObj lastDocMax = chunksToMerge.back().getMax();
    // maxKey is exclusive
    bool maxKeyInRange = lastDocMin.woCompare(maxKey) < 0 && lastDocMax.woCompare(maxKey) >= 0;

    if (!maxKeyInRange) {
        *errMsg = stream() << "could not merge chunks, collection " << nss.ns()
                           << " range ending at " << maxKey << " does not belong to shard "
                           << shardingState->getShardName();

        warning() << *errMsg;
        return false;
    }

    bool validRangeStartKey = firstDocMin.woCompare(minKey) == 0;
    bool validRangeEndKey = lastDocMax.woCompare(maxKey) == 0;

    if (!validRangeStartKey || !validRangeEndKey) {
        *errMsg = stream() << "could not merge chunks, collection " << nss.ns()
                           << " does not contain a chunk "
                           << (!validRangeStartKey ? "starting at " + minKey.toString() : "")
                           << (!validRangeStartKey && !validRangeEndKey ? " or " : "")
                           << (!validRangeEndKey ? "ending at " + maxKey.toString() : "");

        warning() << *errMsg;
        return false;
    }

    if (chunksToMerge.size() == 1) {
        *errMsg = stream() << "could not merge chunks, collection " << nss.ns()
                           << " already contains chunk for " << rangeToString(minKey, maxKey);

        warning() << *errMsg;
        return false;
    }

    // Look for hole in range
    for (size_t i = 1; i < chunksToMerge.size(); ++i) {
        if (chunksToMerge[i - 1].getMax().woCompare(chunksToMerge[i].getMin()) != 0) {
            *errMsg =
                stream() << "could not merge chunks, collection " << nss.ns()
                         << " has a hole in the range " << rangeToString(minKey, maxKey) << " at "
                         << rangeToString(chunksToMerge[i - 1].getMax(), chunksToMerge[i].getMin());

            warning() << *errMsg;
            return false;
        }
    }

    //
    // Run apply ops command
    //
    Status applyOpsStatus = runApplyOpsCmd(chunksToMerge, shardVersion, mergeVersion);
    if (!applyOpsStatus.isOK()) {
        warning() << applyOpsStatus;
        return false;
    }

    //
    // Install merged chunk metadata
    //

    {
        ScopedTransaction transaction(txn, MODE_IX);
        Lock::DBLock writeLk(txn->lockState(), nss.db(), MODE_IX);
        Lock::CollectionLock collLock(txn->lockState(), nss.ns(), MODE_X);

        shardingState->mergeChunks(txn, nss.ns(), minKey, maxKey, mergeVersion);
    }

    //
    // Log change
    //

    BSONObj mergeLogEntry = buildMergeLogEntry(chunksToMerge, shardVersion, mergeVersion);

    grid.catalogManager()->logChange(
        txn->getClient()->clientAddress(true), "merge", nss.ns(), mergeLogEntry);

    return true;
}

//
// Utilities for building BSONObjs for applyOps and change logging
//

BSONObj buildMergeLogEntry(const std::vector<ChunkType>& chunksToMerge,
                           const ChunkVersion& currShardVersion,
                           const ChunkVersion& newMergedVersion) {
    BSONObjBuilder logDetailB;

    BSONArrayBuilder mergedB(logDetailB.subarrayStart("merged"));

    for (const ChunkType& chunkToMerge : chunksToMerge) {
        mergedB.append(chunkToMerge.toBSON());
    }

    mergedB.done();

    currShardVersion.addToBSON(logDetailB, "prevShardVersion");
    newMergedVersion.addToBSON(logDetailB, "mergedVersion");

    return logDetailB.obj();
}


BSONObj buildOpMergeChunk(const ChunkType& mergedChunk) {
    BSONObjBuilder opB;

    // Op basics
    opB.append("op", "u");
    opB.appendBool("b", false);  // no upserting
    opB.append("ns", ChunkType::ConfigNS);

    // New object
    opB.append("o", mergedChunk.toBSON());

    // Query object
    opB.append("o2", BSON(ChunkType::name(mergedChunk.getName())));

    return opB.obj();
}

BSONObj buildOpRemoveChunk(const ChunkType& chunkToRemove) {
    BSONObjBuilder opB;

    // Op basics
    opB.append("op", "d");  // delete
    opB.append("ns", ChunkType::ConfigNS);

    opB.append("o", BSON(ChunkType::name(chunkToRemove.getName())));

    return opB.obj();
}

BSONArray buildOpPrecond(const string& ns,
                         const string& shardName,
                         const ChunkVersion& shardVersion) {
    BSONArrayBuilder preCond;
    BSONObjBuilder condB;
    condB.append("ns", ChunkType::ConfigNS);
    condB.append("q",
                 BSON("query" << BSON(ChunkType::ns(ns)) << "orderby"
                              << BSON(ChunkType::DEPRECATED_lastmod() << -1)));
    {
        BSONObjBuilder resB(condB.subobjStart("res"));
        shardVersion.addToBSON(resB, ChunkType::DEPRECATED_lastmod());
        resB.done();
    }
    preCond.append(condB.obj());
    return preCond.arr();
}

Status runApplyOpsCmd(const std::vector<ChunkType>& chunksToMerge,
                      const ChunkVersion& currShardVersion,
                      const ChunkVersion& newMergedVersion) {
    BSONArrayBuilder updatesB;

    // The chunk we'll be "expanding" is the first chunk
    const ChunkType& firstChunk = chunksToMerge.front();

    // Fill in details not tracked by metadata
    ChunkType mergedChunk(firstChunk);
    mergedChunk.setName(Chunk::genID(firstChunk.getNS(), firstChunk.getMin()));
    mergedChunk.setMax(chunksToMerge.back().getMax());
    mergedChunk.setVersion(newMergedVersion);

    updatesB.append(buildOpMergeChunk(mergedChunk));

    // Don't remove chunk we're expanding
    for (size_t i = 1; i < chunksToMerge.size(); ++i) {
        ChunkType chunkToMerge(chunksToMerge[i]);
        chunkToMerge.setName(Chunk::genID(chunkToMerge.getNS(), chunkToMerge.getMin()));
        updatesB.append(buildOpRemoveChunk(chunkToMerge));
    }

    BSONArray preCond = buildOpPrecond(firstChunk.getNS(), firstChunk.getShard(), currShardVersion);
    return grid.catalogManager()->applyChunkOpsDeprecated(updatesB.arr(), preCond);
}
}
