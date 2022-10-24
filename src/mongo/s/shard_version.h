/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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
#pragma once

#include "mongo/s/chunk_version.h"
#include "mongo/s/index_version.h"

namespace mongo {

/**
 * This class is used to represent the shard version of a collection.
 *
 * It contains the chunk placement information through the ChunkVersion. This class is used for
 * network requests and the shard versioning protocol.
 *
 */
class ShardVersion {
public:
    /**
     * The name for the shard version information field, which shard-aware commands should include
     * if they want to convey shard version.
     */
    static constexpr StringData kShardVersionField = "shardVersion"_sd;

    ShardVersion(ChunkVersion chunkVersion, boost::optional<CollectionIndexes> indexVersion)
        : _chunkVersion(chunkVersion),
          _indexVersion(indexVersion ? boost::make_optional(indexVersion->indexVersion())
                                     : boost::none) {}

    ShardVersion() : _chunkVersion(ChunkVersion()), _indexVersion(boost::none) {}

    static ShardVersion IGNORED() {
        return ShardVersion(ChunkVersion::IGNORED(),
                            boost::optional<CollectionIndexes>(boost::none));
    }

    static ShardVersion UNSHARDED() {
        return ShardVersion(ChunkVersion::UNSHARDED(),
                            boost::optional<CollectionIndexes>(boost::none));
    }

    static bool isIgnoredVersion(const ShardVersion& version) {
        return version == ShardVersion::IGNORED();
    }

    ChunkVersion placementVersion() const {
        return _chunkVersion;
    }

    boost::optional<Timestamp> indexVersion() const {
        return _indexVersion;
    }

    bool operator==(const ShardVersion& otherVersion) const {
        return _chunkVersion == otherVersion._chunkVersion &&
            _indexVersion == otherVersion._indexVersion;
    }

    bool operator!=(const ShardVersion& otherVersion) const {
        return !(otherVersion == *this);
    }

    static ShardVersion parse(const BSONElement& element);
    void serialize(StringData field, BSONObjBuilder* builder) const;

    std::string toString() const;

private:
    ShardVersion(ChunkVersion chunkVersion, boost::optional<Timestamp> indexVersionTimestamp)
        : _chunkVersion(chunkVersion), _indexVersion(indexVersionTimestamp) {}

    ChunkVersion _chunkVersion;
    boost::optional<Timestamp> _indexVersion;
};

inline std::ostream& operator<<(std::ostream& s, const ShardVersion& v) {
    return s << v.toString();
}

inline StringBuilder& operator<<(StringBuilder& s, const ShardVersion& v) {
    return s << v.toString();
}

}  // namespace mongo
