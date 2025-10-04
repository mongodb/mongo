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

#include "mongo/db/global_catalog/type_shard_collection.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/s/resharding/type_collection_fields_gen.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {


const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("db.coll");
const BSONObj kKeyPattern = BSON("a" << 1);
const BSONObj kDefaultCollation = BSON("locale" << "fr_CA");

TEST(ShardCollectionType, FromBSONEmptyShardKeyFails) {
    ASSERT_THROWS_CODE(
        ShardCollectionType(BSON(ShardCollectionType::kNssFieldName
                                 << kNss.ns_forTest() << ShardCollectionType::kEpochFieldName
                                 << OID::gen() << ShardCollectionType::kTimestampFieldName
                                 << Timestamp(1, 1) << ShardCollectionType::kUuidFieldName
                                 << UUID::gen() << ShardCollectionType::kKeyPatternFieldName
                                 << BSONObj() << ShardCollectionType::kUniqueFieldName << true)),
        DBException,
        ErrorCodes::ShardKeyNotFound);
}

TEST(ShardCollectionType,
     FromBSONEpochMatcheslastRefreshedCollectionPlacementVersionWhenBSONTimestamp) {
    OID epoch = OID::gen();
    Timestamp timestamp(1, 1);

    ShardCollectionType shardCollType(
        BSON(ShardCollectionType::kNssFieldName
             << kNss.ns_forTest() << ShardCollectionType::kEpochFieldName << epoch
             << ShardCollectionType::kTimestampFieldName << timestamp
             << ShardCollectionType::kUuidFieldName << UUID::gen()
             << ShardCollectionType::kKeyPatternFieldName << kKeyPattern
             << ShardCollectionType::kUniqueFieldName << true
             << ShardCollectionType::kLastRefreshedCollectionMajorMinorVersionFieldName
             << Timestamp(123, 45)));
    ASSERT_EQ(epoch, shardCollType.getLastRefreshedCollectionPlacementVersion()->epoch());
    ASSERT_EQ(timestamp,
              shardCollType.getLastRefreshedCollectionPlacementVersion()->getTimestamp());
    ASSERT_EQ(Timestamp(123, 45),
              Timestamp(shardCollType.getLastRefreshedCollectionPlacementVersion()->toLong()));
}

TEST(ShardCollectionType, ToBSONEmptyDefaultCollationNotIncluded) {
    ShardCollectionType shardCollType(
        kNss, OID::gen(), Timestamp(1, 1), UUID::gen(), kKeyPattern, true);
    BSONObj obj = shardCollType.toBSON();

    ASSERT_FALSE(obj.hasField(ShardCollectionType::kDefaultCollationFieldName));

    shardCollType.setDefaultCollation(kDefaultCollation);
    obj = shardCollType.toBSON();

    ASSERT_TRUE(obj.hasField(ShardCollectionType::kDefaultCollationFieldName));
}

TEST(ShardCollectionType, ReshardingFieldsIncluded) {
    ShardCollectionType shardCollType(
        kNss, OID::gen(), Timestamp(1, 1), UUID::gen(), kKeyPattern, true);

    TypeCollectionReshardingFields reshardingFields;
    const auto reshardingUUID = UUID::gen();
    reshardingFields.setReshardingUUID(reshardingUUID);
    shardCollType.setReshardingFields(std::move(reshardingFields));

    BSONObj obj = shardCollType.toBSON();
    ASSERT(obj.hasField(ShardCollectionType::kReshardingFieldsFieldName));

    ShardCollectionType shardCollTypeFromBSON(obj);
    ASSERT(shardCollType.getReshardingFields());
    ASSERT_EQ(reshardingUUID, shardCollType.getReshardingFields()->getReshardingUUID());
}

TEST(ShardCollectionType, AllowMigrationsFieldBackwardsCompatibility) {
    ShardCollectionType shardCollType(
        kNss, OID::gen(), Timestamp(1, 1), UUID::gen(), kKeyPattern, true);
    shardCollType.setAllowMigrations(false);
    ASSERT_EQ(false, shardCollType.toBSON()[ShardCollectionType::kAllowMigrationsFieldName].Bool());

    shardCollType.setAllowMigrations(true);
    ASSERT(shardCollType.toBSON()[ShardCollectionType::kAllowMigrationsFieldName].eoo());
}

}  // namespace
}  // namespace mongo
