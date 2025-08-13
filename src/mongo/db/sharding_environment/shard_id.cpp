/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

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
