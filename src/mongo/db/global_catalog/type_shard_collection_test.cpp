// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/global_catalog/type_shard_collection.h"

#include "mongo/base/error_codes.h"
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
