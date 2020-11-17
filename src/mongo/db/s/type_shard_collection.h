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

#include "mongo/db/s/type_shard_collection_gen.h"

namespace mongo {

class ShardCollectionType : private ShardCollectionTypeBase {
public:
    // Make field names accessible.
    using ShardCollectionTypeBase::kDefaultCollationFieldName;
    using ShardCollectionTypeBase::kEnterCriticalSectionCounterFieldName;
    using ShardCollectionTypeBase::kEpochFieldName;
    using ShardCollectionTypeBase::kKeyPatternFieldName;
    using ShardCollectionTypeBase::kLastRefreshedCollectionVersionFieldName;
    using ShardCollectionTypeBase::kNssFieldName;
    using ShardCollectionTypeBase::kPre50CompatibleAllowMigrationsFieldName;
    using ShardCollectionTypeBase::kRefreshingFieldName;
    using ShardCollectionTypeBase::kReshardingFieldsFieldName;
    using ShardCollectionTypeBase::kUniqueFieldName;
    using ShardCollectionTypeBase::kUuidFieldName;

    // Make getters and setters accessible.
    using ShardCollectionTypeBase::getDefaultCollation;
    using ShardCollectionTypeBase::getEnterCriticalSectionCounter;
    using ShardCollectionTypeBase::getEpoch;
    using ShardCollectionTypeBase::getKeyPattern;
    using ShardCollectionTypeBase::getLastRefreshedCollectionVersion;
    using ShardCollectionTypeBase::getNss;
    using ShardCollectionTypeBase::getRefreshing;
    using ShardCollectionTypeBase::getReshardingFields;
    using ShardCollectionTypeBase::getTimestamp;
    using ShardCollectionTypeBase::getUnique;
    using ShardCollectionTypeBase::getUuid;
    using ShardCollectionTypeBase::setDefaultCollation;
    using ShardCollectionTypeBase::setEnterCriticalSectionCounter;
    using ShardCollectionTypeBase::setEpoch;
    using ShardCollectionTypeBase::setKeyPattern;
    using ShardCollectionTypeBase::setLastRefreshedCollectionVersion;
    using ShardCollectionTypeBase::setNss;
    using ShardCollectionTypeBase::setRefreshing;
    using ShardCollectionTypeBase::setReshardingFields;
    using ShardCollectionTypeBase::setUnique;
    using ShardCollectionTypeBase::setUuid;

    ShardCollectionType(
        NamespaceString nss, OID epoch, UUID uuid, KeyPattern keyPattern, bool unique);

    ShardCollectionType(NamespaceString nss,
                        OID epoch,
                        boost::optional<Timestamp> creationTime,
                        UUID uuid,
                        KeyPattern keyPattern,
                        bool unique);

    explicit ShardCollectionType(const BSONObj& obj);

    ShardCollectionType() = default;

    // A wrapper around the IDL generated 'ShardCollectionTypeBase::toBSON' to ensure backwards
    // compatibility.
    BSONObj toBSON() const;

    bool getAllowMigrations() const {
        return getPre50CompatibleAllowMigrations().get_value_or(true);
    }
    void setAllowMigrations(bool allowMigrations);
};

}  // namespace mongo
