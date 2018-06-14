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

#include "mongo/db/s/chunk_splitter.h"

#include "mongo/client/dbclientcursor.h"
#include "mongo/client/query.h"
#include "mongo/db/client.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/split_chunk.h"
#include "mongo/db/s/split_vector.h"
#include "mongo/db/service_context.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/config_server_client.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

/**
 * Constructs the default options for the thread pool used to schedule splits.
 */
ThreadPool::Options makeDefaultThreadPoolOptions() {
    ThreadPool::Options options;
    options.poolName = "ChunkSplitter";
    options.minThreads = 0;
    options.maxThreads = 20;

    // Ensure all threads have a client
    options.onCreateThread = [](const std::string& threadName) {
        Client::initThread(threadName.c_str());
    };
    return options;
}

/**
 * Attempts to split the chunk described by min/maxKey at the split points provided.
 */
Status splitChunkAtMultiplePoints(OperationContext* opCtx,
                                  const ShardId& shardId,
                                  const NamespaceString& nss,
                                  const ShardKeyPattern& shardKeyPattern,
                                  const ChunkVersion& collectionVersion,
                                  const ChunkRange& chunkRange,
                                  const std::vector<BSONObj>& splitPoints) {
    invariant(!splitPoints.empty());

    const size_t kMaxSplitPoints = 8192;

    if (splitPoints.size() > kMaxSplitPoints) {
        return {ErrorCodes::BadValue,
                str::stream() << "Cannot split chunk in more than " << kMaxSplitPoints
                              << " parts at a time."};
    }

    const auto status = splitChunk(opCtx,
                                   nss,
                                   shardKeyPattern.toBSON(),
                                   chunkRange,
                                   splitPoints,
                                   shardId.toString(),
                                   collectionVersion.epoch());

    return status.getStatus().withContext("split failed");
}

/**
 * Attempts to move the chunk specified by minKey away from its current shard.
 */
void moveChunk(OperationContext* opCtx, const NamespaceString& nss, const BSONObj& minKey) {
    // We need to have the most up-to-date view of the chunk we are about to move.
    const auto routingInfo =
        uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));

    uassert(ErrorCodes::NamespaceNotSharded,
            "Could not move chunk. Collection is no longer sharded",
            routingInfo.cm());

    const auto suggestedChunk = routingInfo.cm()->findIntersectingChunkWithSimpleCollation(minKey);

    ChunkType chunkToMove;
    chunkToMove.setNS(nss);
    chunkToMove.setShard(suggestedChunk.getShardId());
    chunkToMove.setMin(suggestedChunk.getMin());
    chunkToMove.setMax(suggestedChunk.getMax());
    chunkToMove.setVersion(suggestedChunk.getLastmod());

    uassertStatusOK(configsvr_client::rebalanceChunk(opCtx, chunkToMove));
}

/**
 * Returns the split point that will result in one of the chunks having exactly one document. Also
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
                               const ShardKeyPattern& shardKeyPattern,
                               bool doSplitAtLower) {
    Query q;

    if (doSplitAtLower) {
        q.sort(shardKeyPattern.toBSON());
    } else {
        // need to invert shard key pattern to sort backwards
        BSONObjBuilder r;

        BSONObjIterator i(shardKeyPattern.toBSON());
        while (i.more()) {
            BSONElement e = i.next();
            uassert(40617, "can only handle numbers here - which i think is correct", e.isNumber());
            r.append(e.fieldName(), -1 * e.number());
        }

        q.sort(r.obj());
    }

    DBDirectClient client(opCtx);

    BSONObj end;

    if (doSplitAtLower) {
        // Splitting close to the lower bound means that the split point will be the
        // upper bound. Chunk range upper bounds are exclusive so skip a document to
        // make the lower half of the split end up with a single document.
        std::unique_ptr<DBClientCursor> cursor = client.query(nss.ns(),
                                                              q,
                                                              1, /* nToReturn */
                                                              1 /* nToSkip */);

        uassert(40618,
                str::stream() << "failed to initialize cursor during auto split due to "
                              << "connection problem with "
                              << client.getServerAddress(),
                cursor.get() != nullptr);

        if (cursor->more()) {
            end = cursor->next().getOwned();
        }
    } else {
        end = client.findOne(nss.ns(), q);
    }

    if (end.isEmpty()) {
        return BSONObj();
    }

    return shardKeyPattern.extractShardKeyFromDoc(end);
}

/**
 * Checks if autobalance is enabled on the current sharded collection.
 */
bool isAutoBalanceEnabled(OperationContext* opCtx,
                          const NamespaceString& nss,
                          BalancerConfiguration* balancerConfig) {
    if (!balancerConfig->shouldBalanceForAutoSplit())
        return false;

    auto collStatus = Grid::get(opCtx)->catalogClient()->getCollection(opCtx, nss);
    if (!collStatus.isOK()) {
        log() << "Auto-split for " << nss << " failed to load collection metadata"
              << causedBy(redact(collStatus.getStatus()));
        return false;
    }

    return collStatus.getValue().value.getAllowBalance();
}

const auto getChunkSplitter = ServiceContext::declareDecoration<ChunkSplitter>();

}  // namespace

ChunkSplitter::ChunkSplitter() : _threadPool(makeDefaultThreadPoolOptions()) {
    _threadPool.startup();
}

ChunkSplitter::~ChunkSplitter() {
    _threadPool.shutdown();
    _threadPool.join();
}

ChunkSplitter& ChunkSplitter::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

ChunkSplitter& ChunkSplitter::get(ServiceContext* serviceContext) {
    return getChunkSplitter(serviceContext);
}

void ChunkSplitter::setReplicaSetMode(bool isPrimary) {
    stdx::lock_guard<stdx::mutex> scopedLock(_mutex);
    _isPrimary = isPrimary;
}

void ChunkSplitter::onStepUp() {
    stdx::lock_guard<stdx::mutex> lg(_mutex);
    if (_isPrimary) {
        return;
    }
    _isPrimary = true;

    // log() << "The ChunkSplitter has started and will accept autosplit tasks.";
    // TODO: Re-enable this log line when auto split is actively running on shards.
}

void ChunkSplitter::onStepDown() {
    stdx::lock_guard<stdx::mutex> lg(_mutex);
    if (!_isPrimary) {
        return;
    }
    _isPrimary = false;

    // log() << "The ChunkSplitter has stopped and will no longer run new autosplit tasks. Any "
    //       << "autosplit tasks that have already started will be allowed to finish.";
    // TODO: Re-enable this log when auto split is actively running on shards.
}

void ChunkSplitter::trySplitting(const NamespaceString& nss,
                                 const BSONObj& min,
                                 const BSONObj& max,
                                 long dataWritten) {
    if (!_isPrimary) {
        return;
    }
    uassertStatusOK(_threadPool.schedule([ this, nss, min, max, dataWritten ]() noexcept {
        _runAutosplit(nss, min, max, dataWritten);
    }));
}

void ChunkSplitter::_runAutosplit(const NamespaceString& nss,
                                  const BSONObj& min,
                                  const BSONObj& max,
                                  long dataWritten) {
    if (!_isPrimary) {
        return;
    }

    try {
        const auto opCtx = cc().makeOperationContext();
        const auto routingInfo = uassertStatusOK(
            Grid::get(opCtx.get())->catalogCache()->getCollectionRoutingInfo(opCtx.get(), nss));

        const auto cm = routingInfo.cm();
        uassert(ErrorCodes::NamespaceNotSharded,
                "Could not split chunk. Collection is no longer sharded",
                cm);

        const auto chunk = cm->findIntersectingChunkWithSimpleCollation(min);
        const auto& shardKeyPattern = cm->getShardKeyPattern();

        const auto balancerConfig = Grid::get(opCtx.get())->getBalancerConfiguration();
        // Ensure we have the most up-to-date balancer configuration
        uassertStatusOK(balancerConfig->refreshAndCheck(opCtx.get()));

        if (!balancerConfig->getShouldAutoSplit()) {
            return;
        }

        const uint64_t maxChunkSizeBytes = balancerConfig->getMaxChunkSizeBytes();

        LOG(1) << "about to initiate autosplit: " << redact(chunk.toString())
               << " dataWritten since last check: " << dataWritten
               << " maxChunkSizeBytes: " << maxChunkSizeBytes;

        auto splitPoints = uassertStatusOK(splitVector(opCtx.get(),
                                                       nss,
                                                       shardKeyPattern.toBSON(),
                                                       chunk.getMin(),
                                                       chunk.getMax(),
                                                       false,
                                                       boost::none,
                                                       boost::none,
                                                       boost::none,
                                                       maxChunkSizeBytes));

        if (splitPoints.size() <= 1) {
            // No split points means there isn't enough data to split on; 1 split point means we
            // have between half the chunk size to full chunk size so there is no need to split yet
            return;
        }

        // We assume that if the chunk being split is the first (or last) one on the collection,
        // this chunk is likely to see more insertions. Instead of splitting mid-chunk, we use the
        // very first (or last) key as a split point.
        //
        // This heuristic is skipped for "special" shard key patterns that are not likely to produce
        // monotonically increasing or decreasing values (e.g. hashed shard keys).

        // Keeps track of the minKey of the top chunk after the split so we can migrate the chunk.
        BSONObj topChunkMinKey;
        const auto skpGlobalMin = shardKeyPattern.getKeyPattern().globalMin();
        const auto skpGlobalMax = shardKeyPattern.getKeyPattern().globalMax();
        if (KeyPattern::isOrderedKeyPattern(shardKeyPattern.toBSON())) {
            if (skpGlobalMin.woCompare(min) == 0) {
                // MinKey is infinity (This is the first chunk on the collection)
                BSONObj key = findExtremeKeyForShard(opCtx.get(), nss, shardKeyPattern, true);
                if (!key.isEmpty()) {
                    splitPoints.front() = key.getOwned();
                    topChunkMinKey = skpGlobalMin;
                }
            } else if (skpGlobalMax.woCompare(max) == 0) {
                // MaxKey is infinity (This is the last chunk on the collection)
                BSONObj key = findExtremeKeyForShard(opCtx.get(), nss, shardKeyPattern, false);
                if (!key.isEmpty()) {
                    splitPoints.back() = key.getOwned();
                    topChunkMinKey = key.getOwned();
                }
            }
        }

        uassertStatusOK(splitChunkAtMultiplePoints(opCtx.get(),
                                                   chunk.getShardId(),
                                                   nss,
                                                   shardKeyPattern,
                                                   cm->getVersion(),
                                                   ChunkRange(min, max),
                                                   splitPoints));

        const bool shouldBalance = isAutoBalanceEnabled(opCtx.get(), nss, balancerConfig);

        log() << "autosplitted " << nss << " chunk: " << redact(chunk.toString()) << " into "
              << (splitPoints.size() + 1) << " parts (maxChunkSizeBytes " << maxChunkSizeBytes
              << ")"
              << (topChunkMinKey.isEmpty() ? "" : " (top chunk migration suggested" +
                          (std::string)(shouldBalance ? ")" : ", but no migrations allowed)"));

        // Balance the resulting chunks if the autobalance option is enabled and if we split at the
        // first or last chunk on the collection as part of top chunk optimization.
        if (!shouldBalance || topChunkMinKey.isEmpty()) {
            return;
        }

        // Tries to move the top chunk out of the shard to prevent the hot spot from staying on a
        // single shard. This is based on the assumption that succeeding inserts will fall on the
        // top chunk.
        moveChunk(opCtx.get(), nss, topChunkMinKey);
    } catch (const DBException& ex) {
        log() << "Unable to auto-split chunk " << redact(ChunkRange(min, max).toString())
              << " in nss " << nss << causedBy(redact(ex.toStatus()));
    } catch (const std::exception& e) {
        log() << "caught exception while splitting chunk: " << redact(e.what());
    }
}

}  // namespace mongo
