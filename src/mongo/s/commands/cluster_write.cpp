/**
 *    Copyright (C) 2013 MongoDB Inc.
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

#include "mongo/s/commands/cluster_write.h"

#include "mongo/base/status.h"
#include "mongo/client/connpool.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/commands/chunk_manager_targeter.h"
#include "mongo/s/config_server_client.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace {

const uint64_t kTooManySplitPoints = 4;

void toBatchError(const Status& status, BatchedCommandResponse* response) {
    response->clear();
    response->setErrCode(status.code());
    response->setErrMessage(status.reason());
    response->setOk(false);
    dassert(response->isValid(NULL));
}

/**
 * Given a maxChunkSize configuration and the number of chunks in a particular sharded collection,
 * returns an optimal chunk size to use in order to achieve a good ratio between number of chunks
 * and their size.
 */
uint64_t calculateDesiredChunkSize(uint64_t maxChunkSizeBytes, uint64_t numChunks) {
    // Splitting faster in early chunks helps spread out an initial load better
    const uint64_t minChunkSize = 1 << 20;  // 1 MBytes

    if (numChunks <= 1) {
        return 1024;
    } else if (numChunks < 3) {
        return minChunkSize / 2;
    } else if (numChunks < 10) {
        return std::max(maxChunkSizeBytes / 4, minChunkSize);
    } else if (numChunks < 20) {
        return std::max(maxChunkSizeBytes / 2, minChunkSize);
    } else {
        return maxChunkSizeBytes;
    }
}

/**
 * Returns the split point that will result in one of the chunk having exactly one document. Also
 * returns an empty document if the split point cannot be determined.
 *
 * doSplitAtLower - determines which side of the split will have exactly one document. True means
 * that the split point chosen will be closer to the lower bound.
 *
 * NOTE: this assumes that the shard key is not "special"- that is, the shardKeyPattern is simply an
 * ordered list of ascending/descending field names. For example {a : 1, b : -1} is not special, but
 * {a : "hashed"} is.
 */
BSONObj findExtremeKeyForShard(OperationContext* opCtx,
                               const NamespaceString& nss,
                               const ShardId& shardId,
                               const ShardKeyPattern& shardKeyPattern,
                               bool doSplitAtLower) {
    Query q;

    if (doSplitAtLower) {
        q.sort(shardKeyPattern.toBSON());
    } else {
        // need to invert shard key pattern to sort backwards
        // TODO: make a helper in ShardKeyPattern?
        BSONObjBuilder r;

        BSONObjIterator i(shardKeyPattern.toBSON());
        while (i.more()) {
            BSONElement e = i.next();
            uassert(10163, "can only handle numbers here - which i think is correct", e.isNumber());
            r.append(e.fieldName(), -1 * e.number());
        }

        q.sort(r.obj());
    }

    // Find the extreme key
    const auto shardConnStr = [&]() {
        const auto shard =
            uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardId));
        return shard->getConnString();
    }();

    ScopedDbConnection conn(shardConnStr);

    BSONObj end;

    if (doSplitAtLower) {
        // Splitting close to the lower bound means that the split point will be the
        // upper bound. Chunk range upper bounds are exclusive so skip a document to
        // make the lower half of the split end up with a single document.
        std::unique_ptr<DBClientCursor> cursor = conn->query(nss.ns(),
                                                             q,
                                                             1, /* nToReturn */
                                                             1 /* nToSkip */);

        uassert(28736,
                str::stream() << "failed to initialize cursor during auto split due to "
                              << "connection problem with "
                              << conn->getServerAddress(),
                cursor.get() != nullptr);

        if (cursor->more()) {
            end = cursor->next().getOwned();
        }
    } else {
        end = conn->findOne(nss.ns(), q);
    }

    conn.done();

    if (end.isEmpty()) {
        return BSONObj();
    }

    return shardKeyPattern.extractShardKeyFromDoc(end);
}

/**
 * Splits the chunks touched based from the targeter stats if needed.
 */
void splitIfNeeded(OperationContext* opCtx,
                   const NamespaceString& nss,
                   const TargeterStats& stats) {
    auto routingInfoStatus = Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss);
    if (!routingInfoStatus.isOK()) {
        log() << "failed to get collection information for " << nss
              << " while checking for auto-split" << causedBy(routingInfoStatus.getStatus());
        return;
    }

    auto& routingInfo = routingInfoStatus.getValue();

    if (!routingInfo.cm()) {
        return;
    }

    for (auto it = stats.chunkSizeDelta.cbegin(); it != stats.chunkSizeDelta.cend(); ++it) {
        std::shared_ptr<Chunk> chunk;
        try {
            chunk = routingInfo.cm()->findIntersectingChunkWithSimpleCollation(it->first);
        } catch (const AssertionException& ex) {
            warning() << "could not find chunk while checking for auto-split: "
                      << causedBy(redact(ex));
            return;
        }

        updateChunkWriteStatsAndSplitIfNeeded(
            opCtx, routingInfo.cm().get(), chunk.get(), it->second);
    }
}

}  // namespace

ClusterWriter::ClusterWriter(bool autoSplit, int timeoutMillis)
    : _autoSplit(autoSplit), _timeoutMillis(timeoutMillis) {}

void ClusterWriter::write(OperationContext* opCtx,
                          const BatchedCommandRequest& origRequest,
                          BatchedCommandResponse* response) {
    // Add _ids to insert request if req'd
    std::unique_ptr<BatchedCommandRequest> idRequest(
        BatchedCommandRequest::cloneWithIds(origRequest));

    const BatchedCommandRequest* request = idRequest ? idRequest.get() : &origRequest;

    const NamespaceString& nss = request->getNS();

    // Config writes and shard writes are done differently
    if (nss.db() == NamespaceString::kConfigDb || nss.db() == NamespaceString::kAdminDb) {
        Grid::get(opCtx)->catalogClient()->writeConfigServerDirect(opCtx, *request, response);
    } else {
        TargeterStats targeterStats;

        {
            ChunkManagerTargeter targeter(request->getTargetingNSS(), &targeterStats);

            Status targetInitStatus = targeter.init(opCtx);
            if (!targetInitStatus.isOK()) {
                toBatchError({targetInitStatus.code(),
                              str::stream() << "unable to target"
                                            << (request->isInsertIndexRequest() ? " index" : "")
                                            << " write op for collection "
                                            << request->getTargetingNSS().toString()
                                            << causedBy(targetInitStatus)},
                             response);
                return;
            }

            BatchWriteExec::executeBatch(opCtx, targeter, *request, response, &_stats);
        }

        if (_autoSplit) {
            splitIfNeeded(opCtx, request->getNS(), targeterStats);
        }
    }
}

const BatchWriteExecStats& ClusterWriter::getStats() {
    return _stats;
}

void updateChunkWriteStatsAndSplitIfNeeded(OperationContext* opCtx,
                                           ChunkManager* manager,
                                           Chunk* chunk,
                                           long dataWritten) {
    // Disable lastError tracking so that any errors, which occur during auto-split do not get
    // bubbled up on the client connection doing a write.
    LastError::Disabled d(&LastError::get(cc()));

    const auto balancerConfig = Grid::get(opCtx)->getBalancerConfiguration();

    const bool minIsInf =
        (0 == manager->getShardKeyPattern().getKeyPattern().globalMin().woCompare(chunk->getMin()));
    const bool maxIsInf =
        (0 == manager->getShardKeyPattern().getKeyPattern().globalMax().woCompare(chunk->getMax()));

    const uint64_t chunkBytesWritten = chunk->addBytesWritten(dataWritten);

    const uint64_t desiredChunkSize =
        calculateDesiredChunkSize(balancerConfig->getMaxChunkSizeBytes(), manager->numChunks());

    if (!chunk->shouldSplit(desiredChunkSize, minIsInf, maxIsInf)) {
        return;
    }

    const NamespaceString nss(manager->getns());

    if (!manager->_autoSplitThrottle._splitTickets.tryAcquire()) {
        LOG(1) << "won't auto split because not enough tickets: " << nss;
        return;
    }

    TicketHolderReleaser releaser(&(manager->_autoSplitThrottle._splitTickets));

    const ChunkRange chunkRange(chunk->getMin(), chunk->getMax());

    try {
        // Ensure we have the most up-to-date balancer configuration
        uassertStatusOK(balancerConfig->refreshAndCheck(opCtx));

        if (!balancerConfig->getShouldAutoSplit()) {
            return;
        }

        LOG(1) << "about to initiate autosplit: " << redact(chunk->toString())
               << " dataWritten: " << chunkBytesWritten
               << " desiredChunkSize: " << desiredChunkSize;

        const uint64_t chunkSizeToUse = [&]() {
            const uint64_t estNumSplitPoints = chunkBytesWritten / desiredChunkSize * 2;

            if (estNumSplitPoints >= kTooManySplitPoints) {
                // The current desired chunk size will split the chunk into lots of small chunk and
                // at the worst case this can result into thousands of chunks. So check and see if a
                // bigger value can be used.
                return std::min(chunkBytesWritten, balancerConfig->getMaxChunkSizeBytes());
            } else {
                return desiredChunkSize;
            }
        }();

        auto splitPoints =
            uassertStatusOK(shardutil::selectChunkSplitPoints(opCtx,
                                                              chunk->getShardId(),
                                                              nss,
                                                              manager->getShardKeyPattern(),
                                                              chunkRange,
                                                              chunkSizeToUse,
                                                              boost::none));

        if (splitPoints.size() <= 1) {
            // No split points means there isn't enough data to split on; 1 split point means we
            // have
            // between half the chunk size to full chunk size so there is no need to split yet
            chunk->clearBytesWritten();
            return;
        }

        if (minIsInf || maxIsInf) {
            // We don't want to reset _dataWritten since we want to check the other side right away
        } else {
            // We're splitting, so should wait a bit
            chunk->clearBytesWritten();
        }

        // We assume that if the chunk being split is the first (or last) one on the collection,
        // this chunk is likely to see more insertions. Instead of splitting mid-chunk, we use the
        // very first (or last) key as a split point.
        //
        // This heuristic is skipped for "special" shard key patterns that are not likely to produce
        // monotonically increasing or decreasing values (e.g. hashed shard keys).
        if (KeyPattern::isOrderedKeyPattern(manager->getShardKeyPattern().toBSON())) {
            if (minIsInf) {
                BSONObj key = findExtremeKeyForShard(
                    opCtx, nss, chunk->getShardId(), manager->getShardKeyPattern(), true);
                if (!key.isEmpty()) {
                    splitPoints.front() = key.getOwned();
                }
            } else if (maxIsInf) {
                BSONObj key = findExtremeKeyForShard(
                    opCtx, nss, chunk->getShardId(), manager->getShardKeyPattern(), false);
                if (!key.isEmpty()) {
                    splitPoints.back() = key.getOwned();
                }
            }
        }

        const auto suggestedMigrateChunk =
            uassertStatusOK(shardutil::splitChunkAtMultiplePoints(opCtx,
                                                                  chunk->getShardId(),
                                                                  nss,
                                                                  manager->getShardKeyPattern(),
                                                                  manager->getVersion(),
                                                                  chunkRange,
                                                                  splitPoints));

        // Balance the resulting chunks if the option is enabled and if the shard suggested a chunk
        // to balance
        const bool shouldBalance = [&]() {
            if (!balancerConfig->shouldBalanceForAutoSplit())
                return false;

            auto collStatus =
                Grid::get(opCtx)->catalogClient()->getCollection(opCtx, manager->getns());
            if (!collStatus.isOK()) {
                log() << "Auto-split for " << nss << " failed to load collection metadata"
                      << causedBy(redact(collStatus.getStatus()));
                return false;
            }

            return collStatus.getValue().value.getAllowBalance();
        }();

        log() << "autosplitted " << nss << " chunk: " << redact(chunk->toString()) << " into "
              << (splitPoints.size() + 1) << " parts (desiredChunkSize " << desiredChunkSize << ")"
              << (suggestedMigrateChunk ? "" : (std::string) " (migrate suggested" +
                          (shouldBalance ? ")" : ", but no migrations allowed)"));

        // Reload the chunk manager after the split
        auto routingInfo = uassertStatusOK(
            Grid::get(opCtx)->catalogCache()->getShardedCollectionRoutingInfoWithRefresh(opCtx,
                                                                                         nss));

        if (!shouldBalance || !suggestedMigrateChunk) {
            return;
        }

        // Top chunk optimization - try to move the top chunk out of this shard to prevent the hot
        // spot from staying on a single shard. This is based on the assumption that succeeding
        // inserts will fall on the top chunk.

        // We need to use the latest chunk manager (after the split) in order to have the most
        // up-to-date view of the chunk we are about to move
        auto suggestedChunk = routingInfo.cm()->findIntersectingChunkWithSimpleCollation(
            suggestedMigrateChunk->getMin());

        ChunkType chunkToMove;
        chunkToMove.setNS(nss.ns());
        chunkToMove.setShard(suggestedChunk->getShardId());
        chunkToMove.setMin(suggestedChunk->getMin());
        chunkToMove.setMax(suggestedChunk->getMax());
        chunkToMove.setVersion(suggestedChunk->getLastmod());

        uassertStatusOK(configsvr_client::rebalanceChunk(opCtx, chunkToMove));

        // Ensure the collection gets reloaded because of the move
        Grid::get(opCtx)->catalogCache()->invalidateShardedCollection(nss);
    } catch (const DBException& ex) {
        chunk->clearBytesWritten();

        if (ErrorCodes::isStaleShardingError(ErrorCodes::Error(ex.getCode()))) {
            log() << "Unable to auto-split chunk " << redact(chunkRange.toString()) << causedBy(ex)
                  << ", going to invalidate routing table entry for " << nss;
            Grid::get(opCtx)->catalogCache()->invalidateShardedCollection(nss);
        }
    }
}

}  // namespace mongo
