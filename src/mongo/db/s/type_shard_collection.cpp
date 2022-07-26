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

#include "mongo/platform/basic.h"

#include "mongo/db/s/type_shard_collection.h"

namespace mongo {

ShardCollectionType::ShardCollectionType(NamespaceString nss,
                                         OID epoch,
                                         Timestamp timestamp,
                                         UUID uuid,
                                         KeyPattern keyPattern,
                                         bool unique)
    : ShardCollectionTypeBase(std::move(nss),
                              std::move(epoch),
                              std::move(timestamp),
                              std::move(uuid),
                              std::move(keyPattern),
                              unique) {}

ShardCollectionType::ShardCollectionType(const BSONObj& obj) {
    ShardCollectionTypeBase::parseProtected(IDLParserContext("ShardCollectionType"), obj);

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

boost::optional<ChunkVersion> ShardCollectionType::getLastRefreshedCollectionVersion() const {
    // Last refreshed collection version is stored as a timestamp in the BSON representation of
    // shard collection type for legacy reasons. We therefore explicitly convert this timestamp, if
    // it exists, into a chunk version.
    if (!getLastRefreshedCollectionMajorMinorVersion())
        return boost::none;

    Timestamp majorMinor = *getLastRefreshedCollectionMajorMinorVersion();
    return ChunkVersion({getEpoch(), getTimestamp()}, {majorMinor.getSecs(), majorMinor.getInc()});
}

}  // namespace mongo
