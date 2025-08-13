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

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/global_catalog/type_shard_collection_gen.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/util/uuid.h"

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class ShardCollectionType : private ShardCollectionTypeBase {
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
