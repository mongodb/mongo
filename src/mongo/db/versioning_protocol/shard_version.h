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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/versioning_protocol/chunk_version.h"

#include <iosfwd>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * This class is used to represent the shard version of a collection. Objects of this class can be
 * constructed through the ShardVersionFactory.
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

    ShardVersion() : _chunkVersion(ChunkVersion()) {}

    static ShardVersion UNSHARDED() {
        return ShardVersion(ChunkVersion::UNSHARDED(), boost::none);
    }

    static bool isPlacementVersionIgnored(const ShardVersion& version) {
        return version.placementVersion() == ChunkVersion::IGNORED();
    }

    ChunkVersion placementVersion() const {
        return _chunkVersion;
    }

    boost::optional<LogicalTime> placementConflictTime() const {
        return _placementConflictTime;
    }

    void setPlacementVersionIgnored() {
        _chunkVersion = ChunkVersion::IGNORED();
    }

    void setPlacementConflictTime(LogicalTime conflictTime) {
        _placementConflictTime.emplace(std::move(conflictTime));
    }

    bool getIgnoreShardingCatalogUuidMismatch() const {
        return _ignoreShardingCatalogUuidMismatch;
    }

    void setIgnoreShardingCatalogUuidMismatch() {
        _ignoreShardingCatalogUuidMismatch = true;
    }

    bool operator==(const ShardVersion& otherVersion) const {
        return _chunkVersion == otherVersion._chunkVersion;
    }

    bool operator!=(const ShardVersion& otherVersion) const {
        return !(otherVersion == *this);
    }

    static ShardVersion parse(const BSONElement& element);
    void serialize(StringData field, BSONObjBuilder* builder) const;

    std::string toString() const;

private:
    ShardVersion(ChunkVersion chunkVersion,
                 const boost::optional<LogicalTime>& placementConflictTime)
        : _chunkVersion(chunkVersion), _placementConflictTime(placementConflictTime) {}

    friend class ShardVersionFactory;

    ChunkVersion _chunkVersion;
    boost::optional<LogicalTime> _placementConflictTime;

    // When set to true, shards will ignore collection UUID mismatches between the sharding catalog
    // and their local catalog.
    bool _ignoreShardingCatalogUuidMismatch = false;
};

inline std::ostream& operator<<(std::ostream& s, const ShardVersion& v) {
    return s << v.toString();
}

inline StringBuilder& operator<<(StringBuilder& s, const ShardVersion& v) {
    return s << v.toString();
}

}  // namespace mongo
