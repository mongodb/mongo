// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/sharding_environment/shard_id.h"

#include "mongo/base/error_codes.h"

namespace mongo {

const ShardId ShardId::kConfigServerId("config");

bool ShardId::isValid() const {
    return !_shardId.empty();
}

Status ShardId::validate(const ShardId& value) {
    if (!value.isValid()) {
        return {ErrorCodes::NoSuchKey, "Shard ID cannot be empty"};
    }
    return Status::OK();
}

bool ShardId::isShardURL() const {
    // Regular expression for pattern <shard_name>/host:port,host:port, ...
    static const pcre::Regex& shardUrlPattern = [] {
        static pcre::Regex regex(R"([^/]+/([^:]+:\d+)(,[^:]+:\d+)*)");
        return regex;
    }();
    return !!shardUrlPattern.match(_shardId);
}

int ShardId::compare(const ShardId& other) const {
    return _shardId.compare(other._shardId);
}

std::size_t ShardId::Hasher::operator()(const ShardId& shardId) const {
    return std::hash<std::string>()(shardId._shardId);
}

}  // namespace mongo
