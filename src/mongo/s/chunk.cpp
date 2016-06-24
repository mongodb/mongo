/**
 *    Copyright (C) 2008-2015 MongoDB Inc.
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

#include "mongo/s/chunk.h"

#include "mongo/client/connpool.h"
#include "mongo/db/commands.h"
#include "mongo/db/lasterror.h"
#include "mongo/platform/random.h"
#include "mongo/s/balancer/balancer.h"
#include "mongo/s/balancer/balancer_configuration.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_util.h"
#include "mongo/s/sharding_raii.h"
#include "mongo/util/log.h"

namespace mongo {

using std::shared_ptr;
using std::unique_ptr;
using std::map;
using std::ostringstream;
using std::set;
using std::string;
using std::stringstream;
using std::vector;

namespace {

const uint64_t kTooManySplitPoints = 4;

}  // namespace

Chunk::Chunk(OperationContext* txn, ChunkManager* manager, const ChunkType& from)
    : _manager(manager), _lastmod(from.getVersion()), _dataWritten(mkDataWritten()) {
    string ns = from.getNS();
    _shardId = from.getShard();

    verify(_lastmod.isSet());

    _min = from.getMin().getOwned();
    _max = from.getMax().getOwned();

    _jumbo = from.getJumbo();

    uassert(10170, "Chunk needs a ns", !ns.empty());
    uassert(13327, "Chunk ns must match server ns", ns == _manager->getns());
    uassert(10172, "Chunk needs a min", !_min.isEmpty());
    uassert(10173, "Chunk needs a max", !_max.isEmpty());
    uassert(10171, "Chunk needs a server", grid.shardRegistry()->getShard(txn, _shardId));
}

Chunk::Chunk(ChunkManager* info,
             const BSONObj& min,
             const BSONObj& max,
             const ShardId& shardId,
             ChunkVersion lastmod,
             uint64_t initialDataWritten)
    : _manager(info),
      _min(min),
      _max(max),
      _shardId(shardId),
      _lastmod(lastmod),
      _jumbo(false),
      _dataWritten(initialDataWritten) {}

int Chunk::mkDataWritten() {
    PseudoRandom r(static_cast<int64_t>(time(0)));
    return r.nextInt32(grid.getBalancerConfiguration()->getMaxChunkSizeBytes() /
                       ChunkManager::SplitHeuristics::splitTestFactor);
}

bool Chunk::containsKey(const BSONObj& shardKey) const {
    return getMin().woCompare(shardKey) <= 0 && shardKey.woCompare(getMax()) < 0;
}

bool Chunk::_minIsInf() const {
    return 0 == _manager->getShardKeyPattern().getKeyPattern().globalMin().woCompare(getMin());
}

bool Chunk::_maxIsInf() const {
    return 0 == _manager->getShardKeyPattern().getKeyPattern().globalMax().woCompare(getMax());
}

BSONObj Chunk::_getExtremeKey(OperationContext* txn, bool doSplitAtLower) const {
    Query q;
    if (doSplitAtLower) {
        q.sort(_manager->getShardKeyPattern().toBSON());
    } else {
        // need to invert shard key pattern to sort backwards
        // TODO: make a helper in ShardKeyPattern?

        BSONObj k = _manager->getShardKeyPattern().toBSON();
        BSONObjBuilder r;

        BSONObjIterator i(k);
        while (i.more()) {
            BSONElement e = i.next();
            uassert(10163, "can only handle numbers here - which i think is correct", e.isNumber());
            r.append(e.fieldName(), -1 * e.number());
        }

        q.sort(r.obj());
    }

    // find the extreme key
    ScopedDbConnection conn(_getShardConnectionString(txn));
    BSONObj end;

    if (doSplitAtLower) {
        // Splitting close to the lower bound means that the split point will be the
        // upper bound. Chunk range upper bounds are exclusive so skip a document to
        // make the lower half of the split end up with a single document.
        unique_ptr<DBClientCursor> cursor = conn->query(_manager->getns(),
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
        end = conn->findOne(_manager->getns(), q);
    }

    conn.done();
    if (end.isEmpty())
        return BSONObj();
    return _manager->getShardKeyPattern().extractShardKeyFromDoc(end);
}

std::vector<BSONObj> Chunk::_determineSplitPoints(OperationContext* txn, bool atMedian) const {
    // If splitting is not obligatory we may return early if there are not enough data we cap the
    // number of objects that would fall in the first half (before the split point) the rationale is
    // we'll find a split point without traversing all the data.
    vector<BSONObj> splitPoints;

    if (atMedian) {
        BSONObj medianKey =
            uassertStatusOK(shardutil::selectMedianKey(txn,
                                                       _shardId,
                                                       NamespaceString(_manager->getns()),
                                                       _manager->getShardKeyPattern(),
                                                       _min,
                                                       _max));
        if (!medianKey.isEmpty()) {
            splitPoints.push_back(medianKey);
        }
    } else {
        uint64_t chunkSize = _manager->getCurrentDesiredChunkSize();

        // Note: One split point for every 1/2 chunk size.
        const uint64_t estNumSplitPoints = _dataWritten / chunkSize * 2;

        if (estNumSplitPoints >= kTooManySplitPoints) {
            // The current desired chunk size will split the chunk into lots of small chunk and at
            // the worst case this can result into thousands of chunks. So check and see if a bigger
            // value can be used.
            chunkSize = std::min(
                _dataWritten, Grid::get(txn)->getBalancerConfiguration()->getMaxChunkSizeBytes());
        }

        splitPoints =
            uassertStatusOK(shardutil::selectChunkSplitPoints(txn,
                                                              _shardId,
                                                              NamespaceString(_manager->getns()),
                                                              _manager->getShardKeyPattern(),
                                                              _min,
                                                              _max,
                                                              chunkSize,
                                                              0,
                                                              MaxObjectPerChunk));
        if (splitPoints.size() <= 1) {
            // No split points means there isn't enough data to split on 1 split point means we have
            // between half the chunk size to full chunk size so we shouldn't split.
            splitPoints.clear();
        }
    }

    return splitPoints;
}

StatusWith<boost::optional<ChunkRange>> Chunk::split(OperationContext* txn,
                                                     SplitPointMode mode,
                                                     size_t* resultingSplits) const {
    size_t dummy;
    if (resultingSplits == NULL) {
        resultingSplits = &dummy;
    }

    bool atMedian = mode == Chunk::atMedian;
    vector<BSONObj> splitPoints = _determineSplitPoints(txn, atMedian);
    if (splitPoints.empty()) {
        string msg;
        if (atMedian) {
            msg = "cannot find median in chunk, possibly empty";
        } else {
            msg = "chunk not full enough to trigger auto-split";
        }

        LOG(1) << msg;
        return Status(ErrorCodes::CannotSplit, msg);
    }

    // We assume that if the chunk being split is the first (or last) one on the collection,
    // this chunk is likely to see more insertions. Instead of splitting mid-chunk, we use
    // the very first (or last) key as a split point.
    // This heuristic is skipped for "special" shard key patterns that are not likely to
    // produce monotonically increasing or decreasing values (e.g. hashed shard keys).
    if (mode == Chunk::autoSplitInternal &&
        KeyPattern::isOrderedKeyPattern(_manager->getShardKeyPattern().toBSON())) {
        if (_minIsInf()) {
            BSONObj key = _getExtremeKey(txn, true);
            if (!key.isEmpty()) {
                splitPoints[0] = key.getOwned();
            }
        } else if (_maxIsInf()) {
            BSONObj key = _getExtremeKey(txn, false);
            if (!key.isEmpty()) {
                splitPoints.pop_back();
                splitPoints.push_back(key);
            }
        }
    }

    // Normally, we'd have a sound split point here if the chunk is not empty.
    // It's also a good place to sanity check.
    if (_min == splitPoints.front()) {
        string msg(str::stream() << "not splitting chunk " << toString() << ", split point "
                                 << splitPoints.front()
                                 << " is exactly on chunk bounds");
        log() << msg;
        return Status(ErrorCodes::CannotSplit, msg);
    }

    if (_max == splitPoints.back()) {
        string msg(str::stream() << "not splitting chunk " << toString() << ", split point "
                                 << splitPoints.back()
                                 << " is exactly on chunk bounds");
        log() << msg;
        return Status(ErrorCodes::CannotSplit, msg);
    }

    auto splitStatus = shardutil::splitChunkAtMultiplePoints(txn,
                                                             _shardId,
                                                             NamespaceString(_manager->getns()),
                                                             _manager->getShardKeyPattern(),
                                                             _manager->getVersion(),
                                                             _min,
                                                             _max,
                                                             splitPoints);
    if (!splitStatus.isOK()) {
        return splitStatus.getStatus();
    }

    _manager->reload(txn);

    *resultingSplits = splitPoints.size();
    return splitStatus.getValue();
}

bool Chunk::splitIfShould(OperationContext* txn, long dataWritten) {
    LastError::Disabled d(&LastError::get(cc()));

    try {
        _dataWritten += dataWritten;
        uint64_t splitThreshold = _manager->getCurrentDesiredChunkSize();
        if (_minIsInf() || _maxIsInf()) {
            splitThreshold = static_cast<uint64_t>((double)splitThreshold * 0.9);
        }

        if (_dataWritten < splitThreshold / ChunkManager::SplitHeuristics::splitTestFactor) {
            return false;
        }

        if (!_manager->_splitHeuristics._splitTickets.tryAcquire()) {
            LOG(1) << "won't auto split because not enough tickets: " << _manager->getns();
            return false;
        }

        TicketHolderReleaser releaser(&(_manager->_splitHeuristics._splitTickets));

        const auto balancerConfig = Grid::get(txn)->getBalancerConfiguration();

        Status refreshStatus = balancerConfig->refreshAndCheck(txn);
        if (!refreshStatus.isOK()) {
            warning() << "Unable to refresh balancer settings" << causedBy(refreshStatus);
            return false;
        }

        bool shouldAutoSplit = balancerConfig->getShouldAutoSplit();
        if (!shouldAutoSplit) {
            return false;
        }

        LOG(1) << "about to initiate autosplit: " << *this << " dataWritten: " << _dataWritten
               << " splitThreshold: " << splitThreshold;

        size_t splitCount = 0;
        auto splitStatus = split(txn, Chunk::autoSplitInternal, &splitCount);
        if (!splitStatus.isOK()) {
            // Split would have issued a message if we got here. This means there wasn't enough
            // data to split, so don't want to try again until considerable more data
            _dataWritten = 0;
            return false;
        }

        if (_maxIsInf() || _minIsInf()) {
            // we don't want to reset _dataWritten since we kind of want to check the other side
            // right away
        } else {
            // we're splitting, so should wait a bit
            _dataWritten = 0;
        }

        bool shouldBalance = balancerConfig->shouldBalanceForAutoSplit();

        if (shouldBalance) {
            auto collStatus = grid.catalogClient(txn)->getCollection(txn, _manager->getns());
            if (!collStatus.isOK()) {
                warning() << "Auto-split for " << _manager->getns()
                          << " failed to load collection metadata"
                          << causedBy(collStatus.getStatus());
                return false;
            }

            shouldBalance = collStatus.getValue().value.getAllowBalance();
        }

        const auto suggestedMigrateChunk = std::move(splitStatus.getValue());

        log() << "autosplitted " << _manager->getns() << " shard: " << toString() << " into "
              << (splitCount + 1) << " (splitThreshold " << splitThreshold << ")"
              << (suggestedMigrateChunk ? "" : (string) " (migrate suggested" +
                          (shouldBalance ? ")" : ", but no migrations allowed)"));

        // Top chunk optimization - try to move the top chunk out of this shard to prevent the hot
        // spot from staying on a single shard. This is based on the assumption that succeeding
        // inserts will fall on the top chunk.
        if (suggestedMigrateChunk && shouldBalance) {
            const NamespaceString nss(_manager->getns());

            // We need to use the latest chunk manager (after the split) in order to have the most
            // up-to-date view of the chunk we are about to move
            auto scopedCM = uassertStatusOK(ScopedChunkManager::getExisting(txn, nss));
            auto suggestedChunk =
                scopedCM.cm()->findIntersectingChunk(txn, suggestedMigrateChunk->getMin());

            ChunkType chunkToMove;
            chunkToMove.setNS(nss.ns());
            chunkToMove.setShard(suggestedChunk->getShardId());
            chunkToMove.setMin(suggestedChunk->getMin());
            chunkToMove.setMax(suggestedChunk->getMax());
            chunkToMove.setVersion(suggestedChunk->getLastmod());

            Status rebalanceStatus = Balancer::get(txn)->rebalanceSingleChunk(txn, chunkToMove);
            if (!rebalanceStatus.isOK()) {
                msgassertedNoTraceWithStatus(10412, rebalanceStatus);
            }

            _manager->reload(txn);
        }

        return true;
    } catch (const DBException& e) {
        // TODO: Make this better - there are lots of reasons a split could fail
        // Random so that we don't sync up with other failed splits
        _dataWritten = mkDataWritten();

        // if the collection lock is taken (e.g. we're migrating), it is fine for the split to fail.
        warning() << "could not autosplit collection " << _manager->getns() << causedBy(e);
        return false;
    }
}

ConnectionString Chunk::_getShardConnectionString(OperationContext* txn) const {
    const auto shard = grid.shardRegistry()->getShard(txn, getShardId());
    return shard->getConnString();
}

void Chunk::appendShortVersion(const char* name, BSONObjBuilder& b) const {
    BSONObjBuilder bb(b.subobjStart(name));
    bb.append(ChunkType::min(), _min);
    bb.append(ChunkType::max(), _max);
    bb.done();
}

bool Chunk::operator==(const Chunk& s) const {
    return _min.woCompare(s._min) == 0 && _max.woCompare(s._max) == 0;
}

string Chunk::toString() const {
    stringstream ss;
    ss << ChunkType::ns() << ": " << _manager->getns() << ", " << ChunkType::shard() << ": "
       << _shardId << ", " << ChunkType::DEPRECATED_lastmod() << ": " << _lastmod.toString() << ", "
       << ChunkType::min() << ": " << _min << ", " << ChunkType::max() << ": " << _max;
    return ss.str();
}

void Chunk::markAsJumbo(OperationContext* txn) const {
    // set this first
    // even if we can't set it in the db
    // at least this mongos won't try and keep moving
    _jumbo = true;

    const string chunkName = ChunkType::genID(_manager->getns(), _min);

    auto status =
        grid.catalogClient(txn)->updateConfigDocument(txn,
                                                      ChunkType::ConfigNS,
                                                      BSON(ChunkType::name(chunkName)),
                                                      BSON("$set" << BSON(ChunkType::jumbo(true))),
                                                      false,
                                                      ShardingCatalogClient::kMajorityWriteConcern);
    if (!status.isOK()) {
        warning() << "couldn't set jumbo for chunk: " << chunkName << causedBy(status.getStatus());
    }
}

}  // namespace mongo
