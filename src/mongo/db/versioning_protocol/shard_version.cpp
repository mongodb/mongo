// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/versioning_protocol/shard_version.h"

#include "mongo/db/versioning_protocol/shard_version_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/testing_proctor.h"

#include <string_view>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

ShardVersion ShardVersion::parse(const BSONElement& element) {
    auto parsedVersion = ShardVersionBase::parse(element.Obj(), IDLParserContext("ShardVersion"));
    auto version = parsedVersion.getVersion();
    boost::optional<NamespaceString> parsedNss;
    if (TestingProctor::instance().isInitialized() && TestingProctor::instance().isEnabled()) {
        parsedNss = parsedVersion.getNss();
    }
    ShardVersion sv(ChunkVersion({parsedVersion.getEpoch(), parsedVersion.getTimestamp()},
                                 {version.getSecs(), version.getInc()}),
                    parsedVersion.getPlacementConflictTime(),
                    parsedNss);
    sv._ignoreShardingCatalogUuidMismatch = parsedVersion.getIgnoreCollectionUuidMismatch();
    return sv;
}

void ShardVersion::serialize(std::string_view field, BSONObjBuilder* builder) const {
    builder->append(field, toBSON());
}

std::string ShardVersion::toString() const {
    return placementVersion().toString();
}

BSONObj ShardVersion::toBSON() const {
    ShardVersionBase version;
    version.setGeneration({placementVersion().epoch(), placementVersion().getTimestamp()});
    version.setPlacement(
        Timestamp(placementVersion().majorVersion(), placementVersion().minorVersion()));
    version.setPlacementConflictTime(_placementConflictTime);
    if (MONGO_unlikely(_ignoreShardingCatalogUuidMismatch)) {
        version.setIgnoreCollectionUuidMismatch(_ignoreShardingCatalogUuidMismatch);
    }
    if (TestingProctor::instance().isInitialized() && TestingProctor::instance().isEnabled()) {
        version.setNss(_nss);
    }
    return version.toBSON();
}

}  // namespace mongo
