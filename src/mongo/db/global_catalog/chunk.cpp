// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/global_catalog/chunk.h"

// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/base/error_codes.h"
#include "mongo/bson/bson_field.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <memory>
#include <utility>

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

ChunkInfo::ChunkInfo(const ChunkType& from)
    : _maxKeyString(ShardKeyPattern::toKeyString(from.getRange().getMax())),
      _minKeyString(ShardKeyPattern::toKeyString(from.getRange().getMin())),
      _range(from.getRange()),
      _shardId(from.getShard()),
      _lastmod(from.getVersion()),
      _history(from.getHistory()),
      _jumbo(from.getJumbo()) {
    uassertStatusOK(from.validate());
}

ChunkInfo::ChunkInfo(ChunkRange range,
                     std::string maxKeyString,
                     std::string minKeyString,
                     ShardId shardId,
                     ChunkVersion version,
                     std::vector<ChunkHistory> history,
                     bool jumbo)
    : _maxKeyString(std::move(maxKeyString)),
      _minKeyString(std::move(minKeyString)),
      _range(std::move(range)),
      _shardId(shardId),
      _lastmod(std::move(version)),
      _history(std::move(history)),
      _jumbo(jumbo) {}

const ShardId& ChunkInfo::getShardIdAt(const boost::optional<Timestamp>& ts) const {
    // This chunk was refreshed from FCV 3.6 config server so it doesn't have history
    if (_history.empty()) {
        return _shardId;
    }

    // If the timestamp is not provided than we return the latest shardid
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
              str::stream() << "Cannot find shardId the chunk belonged to at cluster time "
                            << ts.value().toString());
}

void ChunkInfo::throwIfMovedSince(const Timestamp& ts) const {
    uassert(50978, "Chunk has no history entries", !_history.empty());

    const auto& latestValidAfter = _history.front().getValidAfter();
    if (latestValidAfter <= ts) {
        return;
    }

    uassert(ErrorCodes::StaleChunkHistory,
            str::stream() << "Cannot find shardId the chunk belonged to at cluster time "
                          << ts.toString(),
            _history.back().getValidAfter() <= ts);

    uasserted(ErrorCodes::MigrationConflict,
              str::stream() << "Chunk has moved since timestamp: " << ts.toString()
                            << ", most recently at timestamp: " << latestValidAfter.toString());
}

bool ChunkInfo::containsKey(const BSONObj& shardKey) const {
    return _range.containsKey(shardKey);
}

std::string ChunkInfo::toString() const {
    return toBSON().toString();
}

BSONObj ChunkInfo::toBSON() const {
    BSONObjBuilder bob;
    _range.serialize(&bob);
    bob.append("maxKeyString", _maxKeyString);
    bob.append("minKeyString", _minKeyString);
    bob.append("shardId", _shardId);
    _lastmod.serialize("lastmod", &bob);
    bob.append("jumbo", _jumbo.load());

    BSONArrayBuilder historyArr{bob.subarrayStart("history")};
    for (const auto& historyEntry : _history) {
        historyArr.append(historyEntry.toBSON());
    }
    historyArr.doneFast();
    return bob.obj();
}

void ChunkInfo::setJumbo(bool jumbo) {
    _jumbo.store(jumbo);
}

void Chunk::throwIfMoved() const {
    if (!_atClusterTime) {
        return;
    }

    _chunkInfo.throwIfMovedSince(*_atClusterTime);
}

std::string Chunk::toString() const {
    return toBSON().toString();
}

BSONObj Chunk::toBSON() const {
    BSONObjBuilder bob;
    bob.append("chunkInfo", _chunkInfo.toBSON());
    bob.append("atClusterTime", _atClusterTime ? _atClusterTime->toBSON() : BSONObj());
    return bob.obj();
}


}  // namespace mongo
