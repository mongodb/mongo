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

#include "mongo/platform/random.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace {

// Test whether we should split once data * kSplitCheckInterval > chunkSize (approximately)
PseudoRandom prng(static_cast<int64_t>(time(0)));

// Assume user has 64MB chunkSize setting. It is ok if this assumption is wrong since it is only
// a heuristic.
const int64_t kMaxDataWritten = 64 / Chunk::kSplitTestFactor;

/**
 * Generates a random value for _dataWritten so that a mongos restart wouldn't cause delay in
 * splitting.
 */
int64_t mkDataWritten() {
    return prng.nextInt64(kMaxDataWritten);
}

}  // namespace

ChunkInfo::ChunkInfo(const ChunkType& from)
    : _range(from.getMin(), from.getMax()),
      _shardId(from.getShard()),
      _lastmod(from.getVersion()),
      _history(from.getHistory()),
      _jumbo(from.getJumbo()),
      _dataWritten(mkDataWritten()) {
    invariant(from.validate());
    if (!_history.empty()) {
        invariant(_shardId == _history.front().getShard());
    }
}

const ShardId& ChunkInfo::getShardIdAt(const boost::optional<Timestamp>& ts) const {
    // This chunk was refreshed from FCV 3.6 config server so it doesn't have history
    if (_history.empty()) {
        // TODO: SERVER-34619 - add uassert
        return _shardId;
    }

    // If the tiemstamp is not provided than we return the latest shardid
    if (!ts) {
        invariant(_shardId == _history.front().getShard());
        return _history.front().getShard();
    }

    for (const auto& h : _history) {
        if (h.getValidAfter() <= ts) {
            return h.getShard();
        }
    }

    uasserted(ErrorCodes::StaleChunkHistory,
              str::stream() << "Cant find shardId the chunk belonged to at cluster time "
                            << ts.get().toString());
}

bool ChunkInfo::containsKey(const BSONObj& shardKey) const {
    return getMin().woCompare(shardKey) <= 0 && shardKey.woCompare(getMax()) < 0;
}

uint64_t ChunkInfo::getBytesWritten() const {
    return _dataWritten;
}

uint64_t ChunkInfo::addBytesWritten(uint64_t bytesWrittenIncrement) {
    _dataWritten += bytesWrittenIncrement;
    return _dataWritten;
}

void ChunkInfo::clearBytesWritten() {
    _dataWritten = 0;
}

bool ChunkInfo::shouldSplit(uint64_t desiredChunkSize, bool minIsInf, bool maxIsInf) const {
    // If this chunk is at either end of the range, trigger auto-split at 10% less data written in
    // order to trigger the top-chunk optimization.
    const uint64_t splitThreshold = (minIsInf || maxIsInf)
        ? static_cast<uint64_t>((double)desiredChunkSize * 0.9)
        : desiredChunkSize;

    // Check if there are enough estimated bytes written to warrant a split
    return _dataWritten >= splitThreshold / Chunk::kSplitTestFactor;
}

std::string ChunkInfo::toString() const {
    return str::stream() << ChunkType::shard() << ": " << _shardId << ", " << ChunkType::lastmod()
                         << ": " << _lastmod.toString() << ", " << _range.toString();
}

void ChunkInfo::markAsJumbo() {
    _jumbo = true;
}

}  // namespace mongo
