// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/global_catalog/type_shard_collection_gen.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class [[MONGO_MOD_NEEDS_REPLACEMENT]] ShardCollectionType : private ShardCollectionTypeBase {
public:
    static constexpr auto kAllowMigrationsFieldName = kPre50CompatibleAllowMigrationsFieldName;

    // Make field names accessible.
    using ShardCollectionTypeBase::kDefaultCollationFieldName;
    using ShardCollectionTypeBase::kEnterCriticalSectionCounterFieldName;
    using ShardCollectionTypeBase::kEpochFieldName;
    using ShardCollectionTypeBase::kKeyPatternFieldName;
    using ShardCollectionTypeBase::kLastRefreshedCollectionMajorMinorVersionFieldName;
    using ShardCollectionTypeBase::kNssFieldName;
    using ShardCollectionTypeBase::kRefreshingFieldName;
    using ShardCollectionTypeBase::kReshardingFieldsFieldName;
    using ShardCollectionTypeBase::kTimeseriesFieldsFieldName;
    using ShardCollectionTypeBase::kTimestampFieldName;
    using ShardCollectionTypeBase::kUniqueFieldName;
    using ShardCollectionTypeBase::kUuidFieldName;

    // Make getters and setters accessible.
    using ShardCollectionTypeBase::getDefaultCollation;
    using ShardCollectionTypeBase::getEnterCriticalSectionCounter;
    using ShardCollectionTypeBase::getEpoch;
    using ShardCollectionTypeBase::getKeyPattern;
    using ShardCollectionTypeBase::getNss;
    using ShardCollectionTypeBase::getRefreshing;
    using ShardCollectionTypeBase::getReshardingFields;
    using ShardCollectionTypeBase::getTimeseriesFields;
    using ShardCollectionTypeBase::getTimestamp;
    using ShardCollectionTypeBase::getUnique;
    using ShardCollectionTypeBase::getUnsplittable;
    using ShardCollectionTypeBase::getUuid;
    using ShardCollectionTypeBase::setDefaultCollation;
    using ShardCollectionTypeBase::setEnterCriticalSectionCounter;
    using ShardCollectionTypeBase::setKeyPattern;
    using ShardCollectionTypeBase::setRefreshing;
    using ShardCollectionTypeBase::setReshardingFields;
    using ShardCollectionTypeBase::setTimeseriesFields;
    using ShardCollectionTypeBase::setUnsplittable;

    ShardCollectionType(NamespaceString nss,
                        OID epoch,
                        Timestamp timestamp,
                        UUID uuid,
                        KeyPattern keyPattern,
                        bool unique);

    explicit ShardCollectionType(const BSONObj& obj);

    ShardCollectionType() = default;

    // A wrapper around the IDL generated 'ShardCollectionTypeBase::toBSON' to ensure backwards
    // compatibility.
    BSONObj toBSON() const;

    bool getAllowMigrations() const {
        return getPre50CompatibleAllowMigrations().value_or(true);
    }
    void setAllowMigrations(bool allowMigrations);

    boost::optional<ChunkVersion> getLastRefreshedCollectionPlacementVersion() const;
};

}  // namespace mongo
