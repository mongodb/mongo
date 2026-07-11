// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/global_catalog/type_shard_collection.h"

#include "mongo/base/error_codes.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

ShardCollectionType::ShardCollectionType(NamespaceString nss,
                                         OID epoch,
                                         Timestamp timestamp,
                                         UUID uuid,
                                         KeyPattern keyPattern,
                                         bool unique)
    : ShardCollectionTypeBase(std::move(nss),
                              std::move(uuid),
                              std::move(keyPattern),
                              unique,
                              std::move(epoch),
                              std::move(timestamp)) {}

ShardCollectionType::ShardCollectionType(const BSONObj& obj) {
    ShardCollectionTypeBase::parseProtected(obj, IDLParserContext("ShardCollectionType"));

    uassert(ErrorCodes::ShardKeyNotFound,
            str::stream() << "Empty shard key. Failed to parse: " << obj.toString(),
            !getKeyPattern().toBSON().isEmpty());
}

BSONObj ShardCollectionType::toBSON() const {
    BSONObj obj = ShardCollectionTypeBase::toBSON();

    // Default collation is not included in the BSON representation of shard collection type, and
    // thus persisted to disk, if it is empty. We therefore explicitly remove default collation from
    // obj, if it is empty.
    if (getDefaultCollation().isEmpty()) {
        obj = obj.removeField(kDefaultCollationFieldName);
    }

    return obj;
}

void ShardCollectionType::setAllowMigrations(bool allowMigrations) {
    if (allowMigrations)
        setPre50CompatibleAllowMigrations(boost::none);
    else
        setPre50CompatibleAllowMigrations(false);
}

boost::optional<ChunkVersion> ShardCollectionType::getLastRefreshedCollectionPlacementVersion()
    const {
    // Last refreshed collection placement version is stored as a timestamp in the BSON
    // representation of shard collection type for legacy reasons. We therefore explicitly convert
    // this timestamp, if it exists, into a chunk version.
    if (!getLastRefreshedCollectionMajorMinorVersion())
        return boost::none;

    Timestamp majorMinor = *getLastRefreshedCollectionMajorMinorVersion();
    return ChunkVersion({getEpoch(), getTimestamp()}, {majorMinor.getSecs(), majorMinor.getInc()});
}

}  // namespace mongo
