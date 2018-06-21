/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#include "mongo/db/s/config/sharding_catalog_manager.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(migrationCommitVersionError);

/**
 * Append min, max and version information from chunk to the buffer for logChange purposes.
 */
void appendShortVersion(BufBuilder* b, const ChunkType& chunk) {
    BSONObjBuilder bb(*b);
    bb.append(ChunkType::min(), chunk.getMin());
    bb.append(ChunkType::max(), chunk.getMax());
    if (chunk.isVersionSet())
        chunk.getVersion().addToBSON(bb, ChunkType::lastmod());
    bb.done();
}

BSONArray buildMergeChunksTransactionUpdates(const std::vector<ChunkType>& chunksToMerge,
                                             const ChunkVersion& mergeVersion,
                                             const boost::optional<Timestamp>& validAfter) {
    BSONArrayBuilder updates;

    // Build an update operation to expand the first chunk into the newly merged chunk
    {
        BSONObjBuilder op;
        op.append("op", "u");
        op.appendBool("b", false);  // no upsert
        op.append("ns", ChunkType::ConfigNS.ns());

        // expand first chunk into newly merged chunk
        ChunkType mergedChunk(chunksToMerge.front());
        mergedChunk.setMax(chunksToMerge.back().getMax());

        // fill in additional details for sending through transaction
        mergedChunk.setVersion(mergeVersion);

        invariant(validAfter);
        mergedChunk.setHistory({ChunkHistory(validAfter.get(), mergedChunk.getShard())});

        // add the new chunk information as the update object
        op.append("o", mergedChunk.toConfigBSON());

        // query object
        op.append("o2", BSON(ChunkType::name(mergedChunk.getName())));

        updates.append(op.obj());
    }

    // Build update operations to delete the rest of the chunks to be merged. Remember not
    // to delete the first chunk we're expanding
    for (size_t i = 1; i < chunksToMerge.size(); ++i) {
        BSONObjBuilder op;
        op.append("op", "d");
        op.append("ns", ChunkType::ConfigNS.ns());

        op.append("o", BSON(ChunkType::name(chunksToMerge[i].getName())));

        updates.append(op.obj());
    }

    return updates.arr();
}

BSONArray buildMergeChunksTransactionPrecond(const std::vector<ChunkType>& chunksToMerge,
                                             const ChunkVersion& collVersion) {
    BSONArrayBuilder preCond;

    for (auto chunk : chunksToMerge) {
        BSONObjBuilder b;
        b.append("ns", ChunkType::ConfigNS.ns());
        b.append("q",
                 BSON("query" << BSON(ChunkType::ns(chunk.getNS().ns())
                                      << ChunkType::min(chunk.getMin())
                                      << ChunkType::max(chunk.getMax()))
                              << "orderby"
                              << BSON(ChunkType::lastmod() << -1)));
        b.append("res",
                 BSON(ChunkType::epoch(collVersion.epoch())
                      << ChunkType::shard(chunk.getShard().toString())));
        preCond.append(b.obj());
    }
    return preCond.arr();
}

Status checkChunkIsOnShard(OperationContext* opCtx,
                           const NamespaceString& nss,
                           const BSONObj& min,
                           const BSONObj& max,
                           const ShardId& shard) {
    BSONObj chunkQuery =
        BSON(ChunkType::ns() << nss.ns() << ChunkType::min() << min << ChunkType::max() << max
                             << ChunkType::shard()
                             << shard);

    // Must use local read concern because we're going to perform subsequent writes.
    auto findResponseWith =
        Grid::get(opCtx)->shardRegistry()->getConfigShard()->exhaustiveFindOnConfig(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            repl::ReadConcernLevel::kLocalReadConcern,
            ChunkType::ConfigNS,
            chunkQuery,
            BSONObj(),
            1);
    if (!findResponseWith.isOK()) {
        return findResponseWith.getStatus();
    }

    if (findResponseWith.getValue().docs.empty()) {
        return {ErrorCodes::Error(40165),
                str::stream()
                    << "Could not find the chunk ("
                    << chunkQuery.toString()
                    << ") on the shard. Cannot execute the migration commit with invalid chunks."};
    }

    return Status::OK();
}

BSONObj makeCommitChunkTransactionCommand(const NamespaceString& nss,
                                          const ChunkType& migratedChunk,
                                          const boost::optional<ChunkType>& controlChunk,
                                          StringData fromShard,
                                          StringData toShard) {

    // Update migratedChunk's version and shard.
    BSONArrayBuilder updates;
    {
        BSONObjBuilder op;
        op.append("op", "u");
        op.appendBool("b", false);  // No upserting
        op.append("ns", ChunkType::ConfigNS.ns());

        BSONObjBuilder n(op.subobjStart("o"));
        n.append(ChunkType::name(), ChunkType::genID(nss, migratedChunk.getMin()));
        migratedChunk.getVersion().addToBSON(n, ChunkType::lastmod());
        n.append(ChunkType::ns(), nss.ns());
        n.append(ChunkType::min(), migratedChunk.getMin());
        n.append(ChunkType::max(), migratedChunk.getMax());
        n.append(ChunkType::shard(), toShard);
        migratedChunk.addHistoryToBSON(n);
        n.done();

        BSONObjBuilder q(op.subobjStart("o2"));
        q.append(ChunkType::name(), ChunkType::genID(nss, migratedChunk.getMin()));
        q.done();

        updates.append(op.obj());
    }

    // If we have a controlChunk, update its chunk version.
    if (controlChunk) {
        BSONObjBuilder op;
        op.append("op", "u");
        op.appendBool("b", false);
        op.append("ns", ChunkType::ConfigNS.ns());

        BSONObjBuilder n(op.subobjStart("o"));
        n.append(ChunkType::name(), ChunkType::genID(nss, controlChunk->getMin()));
        controlChunk->getVersion().addToBSON(n, ChunkType::lastmod());
        n.append(ChunkType::ns(), nss.ns());
        n.append(ChunkType::min(), controlChunk->getMin());
        n.append(ChunkType::max(), controlChunk->getMax());
        n.append(ChunkType::shard(), fromShard);
        controlChunk->addHistoryToBSON(n);
        n.done();

        BSONObjBuilder q(op.subobjStart("o2"));
        q.append(ChunkType::name(), ChunkType::genID(nss, controlChunk->getMin()));
        q.done();

        updates.append(op.obj());
    }

    // Do not give applyOps a write concern. If applyOps tries to wait for replication, it will fail
    // because of the GlobalWrite lock CommitChunkMigration already holds. Replication will not be
    // able to take the lock it requires.
    return BSON("applyOps" << updates.arr());
}

/**
 * Returns a chunk different from the one being migrated or 'none' if one doesn't exist.
 */
boost::optional<ChunkType> getControlChunkForMigrate(OperationContext* opCtx,
                                                     const NamespaceString& nss,
                                                     const ChunkType& migratedChunk,
                                                     const ShardId& fromShard) {
    auto const configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

    BSONObjBuilder queryBuilder;
    queryBuilder << ChunkType::ns(nss.ns());
    queryBuilder << ChunkType::shard(fromShard.toString());
    queryBuilder << ChunkType::min(BSON("$ne" << migratedChunk.getMin()));

    auto status =
        configShard->exhaustiveFindOnConfig(opCtx,
                                            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                            repl::ReadConcernLevel::kLocalReadConcern,
                                            ChunkType::ConfigNS,
                                            queryBuilder.obj(),
                                            {},
                                            1);
    auto response = uassertStatusOK(status);
    if (response.docs.empty()) {
        return boost::none;
    }

    return uassertStatusOK(ChunkType::fromConfigBSON(response.docs.front()));
}

}  // namespace

Status ShardingCatalogManager::commitChunkSplit(OperationContext* opCtx,
                                                const NamespaceString& nss,
                                                const OID& requestEpoch,
                                                const ChunkRange& range,
                                                const std::vector<BSONObj>& splitPoints,
                                                const std::string& shardName) {
    // Take _kChunkOpLock in exclusive mode to prevent concurrent chunk splits, merges, and
    // migrations
    // TODO(SERVER-25359): Replace with a collection-specific lock map to allow splits/merges/
    // move chunks on different collections to proceed in parallel
    Lock::ExclusiveLock lk(opCtx->lockState(), _kChunkOpLock);

    std::string errmsg;

    // Get the max chunk version for this namespace.
    auto findStatus = Grid::get(opCtx)->shardRegistry()->getConfigShard()->exhaustiveFindOnConfig(
        opCtx,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        repl::ReadConcernLevel::kLocalReadConcern,
        ChunkType::ConfigNS,
        BSON("ns" << nss.ns()),
        BSON(ChunkType::lastmod << -1),
        1);

    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    const auto& chunksVector = findStatus.getValue().docs;
    if (chunksVector.empty()) {
        errmsg = str::stream() << "splitChunk cannot split chunk " << range.toString()
                               << ". Collection '" << nss.ns()
                               << "' no longer either exists, is sharded, or has chunks";
        return {ErrorCodes::IllegalOperation, errmsg};
    }

    ChunkVersion collVersion = ChunkVersion::fromBSON(chunksVector.front(), ChunkType::lastmod());

    // Return an error if collection epoch does not match epoch of request.
    if (collVersion.epoch() != requestEpoch) {
        errmsg = str::stream() << "splitChunk cannot split chunk " << range.toString()
                               << ". Collection '" << nss.ns() << "' was dropped and re-created."
                               << " Current epoch: " << collVersion.epoch()
                               << ", cmd epoch: " << requestEpoch;
        return {ErrorCodes::StaleEpoch, errmsg};
    }

    // Find the chunk history.
    const auto origChunk = _findChunkOnConfig(opCtx, nss, range.getMin());
    if (!origChunk.isOK()) {
        return origChunk.getStatus();
    }

    std::vector<ChunkType> newChunks;

    ChunkVersion currentMaxVersion = collVersion;

    auto startKey = range.getMin();
    auto newChunkBounds(splitPoints);
    newChunkBounds.push_back(range.getMax());

    BSONArrayBuilder updates;

    for (const auto& endKey : newChunkBounds) {
        // Verify the split points are all within the chunk
        if (endKey.woCompare(range.getMax()) != 0 && !range.containsKey(endKey)) {
            return {ErrorCodes::InvalidOptions,
                    str::stream() << "Split key " << endKey << " not contained within chunk "
                                  << range.toString()};
        }

        // Verify the split points came in increasing order
        if (endKey.woCompare(startKey) < 0) {
            return {
                ErrorCodes::InvalidOptions,
                str::stream() << "Split keys must be specified in strictly increasing order. Key "
                              << endKey
                              << " was specified after "
                              << startKey
                              << "."};
        }

        // Verify that splitPoints are not repeated
        if (endKey.woCompare(startKey) == 0) {
            return {ErrorCodes::InvalidOptions,
                    str::stream() << "Split on lower bound of chunk "
                                  << ChunkRange(startKey, endKey).toString()
                                  << "is not allowed"};
        }

        // verify that splits don't create too-big shard keys
        Status shardKeySizeStatus = ShardKeyPattern::checkShardKeySize(endKey);
        if (!shardKeySizeStatus.isOK()) {
            return shardKeySizeStatus;
        }

        // verify that splits don't use disallowed BSON object format
        Status shardKeyStorageStatus =
            ShardKeyPattern::checkShardKeyIsValidForMetadataStorage(endKey);
        if (!shardKeyStorageStatus.isOK()) {
            return shardKeyStorageStatus;
        }

        // splits only update the 'minor' portion of version
        currentMaxVersion.incMinor();

        // build an update operation against the chunks collection of the config database
        // with upsert true
        BSONObjBuilder op;
        op.append("op", "u");
        op.appendBool("b", true);
        op.append("ns", ChunkType::ConfigNS.ns());

        // add the modified (new) chunk information as the update object
        BSONObjBuilder n(op.subobjStart("o"));
        n.append(ChunkType::name(), ChunkType::genID(nss, startKey));
        currentMaxVersion.addToBSON(n, ChunkType::lastmod());
        n.append(ChunkType::ns(), nss.ns());
        n.append(ChunkType::min(), startKey);
        n.append(ChunkType::max(), endKey);
        n.append(ChunkType::shard(), shardName);

        origChunk.getValue().addHistoryToBSON(n);

        n.done();

        // add the chunk's _id as the query part of the update statement
        BSONObjBuilder q(op.subobjStart("o2"));
        q.append(ChunkType::name(), ChunkType::genID(nss, startKey));
        q.done();

        updates.append(op.obj());

        // remember this chunk info for logging later
        ChunkType chunk;
        chunk.setMin(startKey);
        chunk.setMax(endKey);
        chunk.setVersion(currentMaxVersion);

        newChunks.push_back(std::move(chunk));

        startKey = endKey;
    }

    BSONArrayBuilder preCond;
    {
        BSONObjBuilder b;
        b.append("ns", ChunkType::ConfigNS.ns());
        b.append("q",
                 BSON("query" << BSON(ChunkType::ns(nss.ns()) << ChunkType::min() << range.getMin()
                                                              << ChunkType::max()
                                                              << range.getMax())
                              << "orderby"
                              << BSON(ChunkType::lastmod() << -1)));
        {
            BSONObjBuilder bb(b.subobjStart("res"));
            bb.append(ChunkType::epoch(), requestEpoch);
            bb.append(ChunkType::shard(), shardName);
        }
        preCond.append(b.obj());
    }

    // apply the batch of updates to local metadata.
    Status applyOpsStatus = Grid::get(opCtx)->catalogClient()->applyChunkOpsDeprecated(
        opCtx,
        updates.arr(),
        preCond.arr(),
        nss,
        currentMaxVersion,
        WriteConcernOptions(),
        repl::ReadConcernLevel::kLocalReadConcern);
    if (!applyOpsStatus.isOK()) {
        return applyOpsStatus;
    }

    // log changes
    BSONObjBuilder logDetail;
    {
        BSONObjBuilder b(logDetail.subobjStart("before"));
        b.append(ChunkType::min(), range.getMin());
        b.append(ChunkType::max(), range.getMax());
        collVersion.addToBSON(b, ChunkType::lastmod());
    }

    if (newChunks.size() == 2) {
        appendShortVersion(&logDetail.subobjStart("left"), newChunks[0]);
        appendShortVersion(&logDetail.subobjStart("right"), newChunks[1]);

        Grid::get(opCtx)
            ->catalogClient()
            ->logChange(opCtx, "split", nss.ns(), logDetail.obj(), WriteConcernOptions())
            .transitional_ignore();
    } else {
        BSONObj beforeDetailObj = logDetail.obj();
        BSONObj firstDetailObj = beforeDetailObj.getOwned();
        const int newChunksSize = newChunks.size();

        for (int i = 0; i < newChunksSize; i++) {
            BSONObjBuilder chunkDetail;
            chunkDetail.appendElements(beforeDetailObj);
            chunkDetail.append("number", i + 1);
            chunkDetail.append("of", newChunksSize);
            appendShortVersion(&chunkDetail.subobjStart("chunk"), newChunks[i]);

            Grid::get(opCtx)
                ->catalogClient()
                ->logChange(
                    opCtx, "multi-split", nss.ns(), chunkDetail.obj(), WriteConcernOptions())
                .transitional_ignore();
        }
    }

    return applyOpsStatus;
}

Status ShardingCatalogManager::commitChunkMerge(OperationContext* opCtx,
                                                const NamespaceString& nss,
                                                const OID& requestEpoch,
                                                const std::vector<BSONObj>& chunkBoundaries,
                                                const std::string& shardName,
                                                const boost::optional<Timestamp>& validAfter) {
    // This method must never be called with empty chunks to merge
    invariant(!chunkBoundaries.empty());

    // Take _kChunkOpLock in exclusive mode to prevent concurrent chunk splits, merges, and
    // migrations
    // TODO(SERVER-25359): Replace with a collection-specific lock map to allow splits/merges/
    // move chunks on different collections to proceed in parallel
    Lock::ExclusiveLock lk(opCtx->lockState(), _kChunkOpLock);

    if (!validAfter) {
        return {ErrorCodes::IllegalOperation, "chunk operation requires validAfter timestamp"};
    }
    // Get the chunk with the highest version for this namespace
    auto findStatus = Grid::get(opCtx)->shardRegistry()->getConfigShard()->exhaustiveFindOnConfig(
        opCtx,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        repl::ReadConcernLevel::kLocalReadConcern,
        ChunkType::ConfigNS,
        BSON("ns" << nss.ns()),
        BSON(ChunkType::lastmod << -1),
        1);

    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    const auto& chunksVector = findStatus.getValue().docs;
    if (chunksVector.empty())
        return {ErrorCodes::IllegalOperation,
                "collection does not exist, isn't sharded, or has no chunks"};

    ChunkVersion collVersion = ChunkVersion::fromBSON(chunksVector.front(), ChunkType::lastmod());

    // Return an error if epoch of chunk does not match epoch of request
    if (collVersion.epoch() != requestEpoch) {
        return {ErrorCodes::StaleEpoch,
                "epoch of chunk does not match epoch of request. This most likely means "
                "that the collection was dropped and re-created."};
    }

    // Build chunks to be merged
    std::vector<ChunkType> chunksToMerge;

    ChunkType itChunk;
    itChunk.setMax(chunkBoundaries.front());
    itChunk.setNS(nss);
    itChunk.setShard(shardName);

    // Do not use the first chunk boundary as a max bound while building chunks
    for (size_t i = 1; i < chunkBoundaries.size(); ++i) {
        itChunk.setMin(itChunk.getMax());

        // Ensure the chunk boundaries are strictly increasing
        if (chunkBoundaries[i].woCompare(itChunk.getMin()) <= 0) {
            return {
                ErrorCodes::InvalidOptions,
                str::stream()
                    << "Chunk boundaries must be specified in strictly increasing order. Boundary "
                    << chunkBoundaries[i]
                    << " was specified after "
                    << itChunk.getMin()
                    << "."};
        }

        itChunk.setMax(chunkBoundaries[i]);
        chunksToMerge.push_back(itChunk);
    }

    ChunkVersion mergeVersion = collVersion;
    mergeVersion.incMinor();

    auto updates = buildMergeChunksTransactionUpdates(chunksToMerge, mergeVersion, validAfter);
    auto preCond = buildMergeChunksTransactionPrecond(chunksToMerge, collVersion);

    // apply the batch of updates to local metadata
    Status applyOpsStatus = Grid::get(opCtx)->catalogClient()->applyChunkOpsDeprecated(
        opCtx,
        updates,
        preCond,
        nss,
        mergeVersion,
        WriteConcernOptions(),
        repl::ReadConcernLevel::kLocalReadConcern);
    if (!applyOpsStatus.isOK()) {
        return applyOpsStatus;
    }

    // log changes
    BSONObjBuilder logDetail;
    {
        BSONArrayBuilder b(logDetail.subarrayStart("merged"));
        for (auto chunkToMerge : chunksToMerge) {
            b.append(chunkToMerge.toConfigBSON());
        }
    }
    collVersion.addToBSON(logDetail, "prevShardVersion");
    mergeVersion.addToBSON(logDetail, "mergedVersion");

    Grid::get(opCtx)
        ->catalogClient()
        ->logChange(opCtx, "merge", nss.ns(), logDetail.obj(), WriteConcernOptions())
        .transitional_ignore();

    return applyOpsStatus;
}

StatusWith<BSONObj> ShardingCatalogManager::commitChunkMigration(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const ChunkType& migratedChunk,
    const OID& collectionEpoch,
    const ShardId& fromShard,
    const ShardId& toShard,
    const boost::optional<Timestamp>& validAfter) {

    auto const configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

    // Take _kChunkOpLock in exclusive mode to prevent concurrent chunk splits, merges, and
    // migrations.
    //
    // ConfigSvrCommitChunkMigration commands must be run serially because the new ChunkVersions
    // for migrated chunks are generated within the command and must be committed to the database
    // before another chunk commit generates new ChunkVersions in the same manner.
    //
    // TODO(SERVER-25359): Replace with a collection-specific lock map to allow splits/merges/
    // move chunks on different collections to proceed in parallel.
    // (Note: This is not needed while we have a global lock, taken here only for consistency.)
    Lock::ExclusiveLock lk(opCtx->lockState(), _kChunkOpLock);

    if (!validAfter) {
        return {ErrorCodes::IllegalOperation, "chunk operation requires validAfter timestamp"};
    }

    // Must use local read concern because we will perform subsequent writes.
    auto findResponse =
        configShard->exhaustiveFindOnConfig(opCtx,
                                            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                            repl::ReadConcernLevel::kLocalReadConcern,
                                            ChunkType::ConfigNS,
                                            BSON("ns" << nss.ns()),
                                            BSON(ChunkType::lastmod << -1),
                                            1);
    if (!findResponse.isOK()) {
        return findResponse.getStatus();
    }

    if (MONGO_FAIL_POINT(migrationCommitVersionError)) {
        uassert(ErrorCodes::StaleEpoch,
                "failpoint 'migrationCommitVersionError' generated error",
                false);
    }

    const auto chunksVector = std::move(findResponse.getValue().docs);
    if (chunksVector.empty()) {
        return {ErrorCodes::IncompatibleShardingMetadata,
                str::stream() << "Tried to find max chunk version for collection '" << nss.ns()
                              << ", but found no chunks"};
    }

    const auto swChunk = ChunkType::fromConfigBSON(chunksVector.front());
    if (!swChunk.isOK()) {
        return swChunk.getStatus();
    }

    const auto currentCollectionVersion = swChunk.getValue().getVersion();

    // It is possible for a migration to end up running partly without the protection of the
    // distributed lock if the config primary stepped down since the start of the migration and
    // failed to recover the migration. Check that the collection has not been dropped and recreated
    // since the migration began, unbeknown to the shard when the command was sent.
    if (currentCollectionVersion.epoch() != collectionEpoch) {
        return {ErrorCodes::StaleEpoch,
                str::stream() << "The collection '" << nss.ns()
                              << "' has been dropped and recreated since the migration began."
                                 " The config server's collection version epoch is now '"
                              << currentCollectionVersion.epoch().toString()
                              << "', but the shard's is "
                              << collectionEpoch.toString()
                              << "'. Aborting migration commit for chunk ("
                              << migratedChunk.getRange().toString()
                              << ")."};
    }

    // Check that migratedChunk is where it should be, on fromShard.
    auto migratedOnShard =
        checkChunkIsOnShard(opCtx, nss, migratedChunk.getMin(), migratedChunk.getMax(), fromShard);
    if (!migratedOnShard.isOK()) {
        return migratedOnShard;
    }

    auto controlChunk = getControlChunkForMigrate(opCtx, nss, migratedChunk, fromShard);

    // Find the chunk history.
    const auto origChunk = _findChunkOnConfig(opCtx, nss, migratedChunk.getMin());
    if (!origChunk.isOK()) {
        return origChunk.getStatus();
    }

    // Generate the new versions of migratedChunk and controlChunk. Migrating chunk's minor version
    // will be 0.
    ChunkType newMigratedChunk = migratedChunk;
    newMigratedChunk.setShard(toShard);
    newMigratedChunk.setVersion(ChunkVersion(
        currentCollectionVersion.majorVersion() + 1, 0, currentCollectionVersion.epoch()));

    // Copy the complete history.
    auto newHistory = origChunk.getValue().getHistory();
    const int kHistorySecs = 10;

    invariant(validAfter);

    // Update the history of the migrated chunk.
    // Drop the history that is too old (10 seconds of history for now).
    // TODO SERVER-33831 to update the old history removal policy.
    while (!newHistory.empty() &&
           newHistory.back().getValidAfter().getSecs() + kHistorySecs <
               validAfter.get().getSecs()) {
        newHistory.pop_back();
    }

    if (!newHistory.empty() && newHistory.front().getValidAfter() >= validAfter.get()) {
        return {ErrorCodes::IncompatibleShardingMetadata,
                str::stream() << "The chunk history for '"
                              << ChunkType::genID(nss, migratedChunk.getMin())
                              << " is corrupted. The last validAfter "
                              << newHistory.back().getValidAfter().toString()
                              << " is greater or equal to the new validAfter "
                              << validAfter.get().toString()};
    }
    newHistory.emplace(newHistory.begin(), ChunkHistory(validAfter.get(), toShard));
    newMigratedChunk.setHistory(std::move(newHistory));

    // Control chunk's minor version will be 1 (if control chunk is present).
    boost::optional<ChunkType> newControlChunk = boost::none;
    if (controlChunk) {
        // Find the chunk history.
        const auto origControlChunk = _findChunkOnConfig(opCtx, nss, controlChunk->getMin());
        if (!origControlChunk.isOK()) {
            return origControlChunk.getStatus();
        }

        newControlChunk = origControlChunk.getValue();
        newControlChunk->setVersion(ChunkVersion(
            currentCollectionVersion.majorVersion() + 1, 1, currentCollectionVersion.epoch()));
    }

    auto command = makeCommitChunkTransactionCommand(
        nss, newMigratedChunk, newControlChunk, fromShard.toString(), toShard.toString());

    StatusWith<Shard::CommandResponse> applyOpsCommandResponse =
        configShard->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            nss.db().toString(),
            command,
            Shard::RetryPolicy::kIdempotent);

    if (!applyOpsCommandResponse.isOK()) {
        return applyOpsCommandResponse.getStatus();
    }

    if (!applyOpsCommandResponse.getValue().commandStatus.isOK()) {
        return applyOpsCommandResponse.getValue().commandStatus;
    }

    BSONObjBuilder result;
    newMigratedChunk.getVersion().appendWithFieldForCommands(&result, "migratedChunkVersion");
    if (controlChunk) {
        newControlChunk->getVersion().appendWithFieldForCommands(&result, "controlChunkVersion");
    }

    return result.obj();
}

StatusWith<ChunkType> ShardingCatalogManager::_findChunkOnConfig(OperationContext* opCtx,
                                                                 const NamespaceString& nss,
                                                                 const BSONObj& key) {
    auto const configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

    auto findResponse =
        configShard->exhaustiveFindOnConfig(opCtx,
                                            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                            repl::ReadConcernLevel::kLocalReadConcern,
                                            ChunkType::ConfigNS,
                                            BSON(ChunkType::name << ChunkType::genID(nss, key)),
                                            BSONObj(),
                                            1);

    if (!findResponse.isOK()) {
        return findResponse.getStatus();
    }

    const auto origChunks = std::move(findResponse.getValue().docs);
    if (origChunks.size() != 1) {
        return {ErrorCodes::IncompatibleShardingMetadata,
                str::stream() << "Tried to find the chunk for '" << ChunkType::genID(nss, key)
                              << ", but found no chunks"};
    }

    return ChunkType::fromConfigBSON(origChunks.front());
}

StatusWith<ChunkVersion> ShardingCatalogManager::_findCollectionVersion(
    OperationContext* opCtx, const NamespaceString& nss, const OID& collectionEpoch) {
    auto const configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

    // Must use local read concern because we will perform subsequent writes.
    auto findResponse =
        configShard->exhaustiveFindOnConfig(opCtx,
                                            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                            repl::ReadConcernLevel::kLocalReadConcern,
                                            ChunkType::ConfigNS,
                                            BSON("ns" << nss.ns()),
                                            BSON(ChunkType::lastmod << -1),
                                            1);
    if (!findResponse.isOK()) {
        return findResponse.getStatus();
    }

    const auto chunksVector = std::move(findResponse.getValue().docs);
    if (chunksVector.empty()) {
        return {ErrorCodes::IncompatibleShardingMetadata,
                str::stream() << "Tried to find max chunk version for collection '" << nss.ns()
                              << ", but found no chunks"};
    }

    const auto swChunk = ChunkType::fromConfigBSON(chunksVector.front());
    if (!swChunk.isOK()) {
        return swChunk.getStatus();
    }

    const auto currentCollectionVersion = swChunk.getValue().getVersion();

    // It is possible for a migration to end up running partly without the protection of the
    // distributed lock if the config primary stepped down since the start of the migration and
    // failed to recover the migration. Check that the collection has not been dropped and recreated
    // since the migration began, unbeknown to the shard when the command was sent.
    if (currentCollectionVersion.epoch() != collectionEpoch) {
        return {ErrorCodes::StaleEpoch,
                str::stream() << "The collection '" << nss.ns()
                              << "' has been dropped and recreated since the migration began."
                                 " The config server's collection version epoch is now '"
                              << currentCollectionVersion.epoch().toString()
                              << "', but the shard's is "
                              << collectionEpoch.toString()
                              << "'."};
    }

    return currentCollectionVersion;
}

}  // namespace mongo
