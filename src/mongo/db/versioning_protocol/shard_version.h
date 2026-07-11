// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/util/modules.h"

#include <iosfwd>
#include <string>
#include <string_view>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

/**
 * This class is used to represent the shard version of a collection. Objects of this class can be
 * constructed through the ShardVersionFactory.
 *
 * It contains the chunk placement information through the ChunkVersion. This class is used for
 * network requests and the shard versioning protocol.
 *
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] ShardVersion {
public:
    /**
     * The name for the shard version information field, which shard-aware commands should include
     * if they want to convey shard version.
     */
    static constexpr std::string_view kShardVersionField = "shardVersion"sv;

    ShardVersion() : _chunkVersion(ChunkVersion()) {}

    static ShardVersion UNTRACKED() {
        return ShardVersion(ChunkVersion::UNTRACKED(), boost::none, boost::none);
    }

    static bool isPlacementVersionIgnored(const ShardVersion& version) {
        return version.placementVersion() == ChunkVersion::IGNORED();
    }

    ChunkVersion placementVersion() const {
        return _chunkVersion;
    }

    // TODO (SERVER-115178): Remove once v9.0 branches out
    boost::optional<LogicalTime> placementConflictTime_DEPRECATED() const {
        return _placementConflictTime;
    }

    // TODO (SERVER-115178): Remove once v9.0 branches out
    void setPlacementConflictTime_DEPRECATED(LogicalTime conflictTime) {
        _placementConflictTime.emplace(std::move(conflictTime));
    }

    void setPlacementVersionIgnored() {
        _chunkVersion = ChunkVersion::IGNORED();
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
    void serialize(std::string_view field, BSONObjBuilder* builder) const;

    std::string toString() const;
    BSONObj toBSON() const;

    const boost::optional<NamespaceString>& getNSS() const {
        return _nss;
    }

private:
    ShardVersion(ChunkVersion chunkVersion,
                 const boost::optional<LogicalTime>& placementConflictTime,
                 boost::optional<NamespaceString> nss)
        : _chunkVersion(chunkVersion),
          _placementConflictTime(placementConflictTime),
          _nss(std::move(nss)) {}
    friend class ShardVersionFactory;

    ChunkVersion _chunkVersion;
    boost::optional<LogicalTime> _placementConflictTime;

    // When set to true, shards will ignore collection UUID mismatches between the sharding catalog
    // and their local catalog.
    bool _ignoreShardingCatalogUuidMismatch = false;

    boost::optional<NamespaceString> _nss;
};

inline std::ostream& operator<<(std::ostream& s, const ShardVersion& v) {
    return s << v.toString();
}

inline StringBuilder& operator<<(StringBuilder& s, const ShardVersion& v) {
    return s << v.toString();
}

}  // namespace mongo
