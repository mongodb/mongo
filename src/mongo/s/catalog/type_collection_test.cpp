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

#include "mongo/s/catalog/type_collection.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {

using unittest::assertGet;

TEST(CollectionType, Empty) {
    StatusWith<CollectionType> status = CollectionType::fromBSON(BSONObj());
    ASSERT_FALSE(status.isOK());
}

TEST(CollectionType, Basic) {
    const OID oid = OID::gen();
    StatusWith<CollectionType> status = CollectionType::fromBSON(
        BSON(CollectionType::kNssFieldName << "db.coll" << CollectionType::kEpochFieldName << oid
                                           << CollectionType::kUpdatedAtFieldName
                                           << Date_t::fromMillisSinceEpoch(1)
                                           << CollectionType::kKeyPatternFieldName << BSON("a" << 1)
                                           << CollectionType::defaultCollation(BSON("locale"
                                                                                    << "fr_CA"))
                                           << CollectionType::unique(true)));
    ASSERT_TRUE(status.isOK());

    CollectionType coll = status.getValue();
    ASSERT_TRUE(coll.validate().isOK());
    ASSERT(coll.getNss() == NamespaceString{"db.coll"});
    ASSERT_EQUALS(coll.getEpoch(), oid);
    ASSERT_EQUALS(coll.getUpdatedAt(), Date_t::fromMillisSinceEpoch(1));
    ASSERT_BSONOBJ_EQ(coll.getKeyPattern().toBSON(), BSON("a" << 1));
    ASSERT_BSONOBJ_EQ(coll.getDefaultCollation(),
                      BSON("locale"
                           << "fr_CA"));
    ASSERT_EQUALS(coll.getUnique(), true);
    ASSERT_EQUALS(coll.getAllowBalance(), true);
    ASSERT_EQUALS(coll.getDropped(), false);
}

TEST(CollectionType, AllFieldsPresent) {
    const OID oid = OID::gen();
    const auto uuid = UUID::gen();
    const auto reshardingUuid = UUID::gen();

    ReshardingFields reshardingFields;
    reshardingFields.setUuid(reshardingUuid);

    StatusWith<CollectionType> status = CollectionType::fromBSON(
        BSON(CollectionType::kNssFieldName
             << "db.coll" << CollectionType::kEpochFieldName << oid
             << CollectionType::kUpdatedAtFieldName << Date_t::fromMillisSinceEpoch(1)
             << CollectionType::kKeyPatternFieldName << BSON("a" << 1)
             << CollectionType::defaultCollation(BSON("locale"
                                                      << "fr_CA"))
             << CollectionType::unique(true) << CollectionType::uuid() << uuid
             << CollectionType::reshardingFields() << reshardingFields.toBSON()));
    ASSERT_TRUE(status.isOK());

    CollectionType coll = status.getValue();
    ASSERT_TRUE(coll.validate().isOK());
    ASSERT(coll.getNss() == NamespaceString{"db.coll"});
    ASSERT_EQUALS(coll.getEpoch(), oid);
    ASSERT_EQUALS(coll.getUpdatedAt(), Date_t::fromMillisSinceEpoch(1));
    ASSERT_BSONOBJ_EQ(coll.getKeyPattern().toBSON(), BSON("a" << 1));
    ASSERT_BSONOBJ_EQ(coll.getDefaultCollation(),
                      BSON("locale"
                           << "fr_CA"));
    ASSERT_EQUALS(coll.getUnique(), true);
    ASSERT_EQUALS(coll.getAllowBalance(), true);
    ASSERT_EQUALS(coll.getDropped(), false);
    ASSERT_TRUE(coll.getUUID());
    ASSERT_EQUALS(*coll.getUUID(), uuid);
    ASSERT(coll.getReshardingFields()->getState() == CoordinatorStateEnum::kUnused);
    ASSERT(coll.getReshardingFields()->getUuid() == reshardingUuid);
}

TEST(CollectionType, EmptyDefaultCollationFailsToParse) {
    const OID oid = OID::gen();
    StatusWith<CollectionType> status = CollectionType::fromBSON(
        BSON(CollectionType::kNssFieldName
             << "db.coll" << CollectionType::kEpochFieldName << oid
             << CollectionType::kUpdatedAtFieldName << Date_t::fromMillisSinceEpoch(1)
             << CollectionType::kKeyPatternFieldName << BSON("a" << 1)
             << CollectionType::defaultCollation(BSONObj()) << CollectionType::unique(true)));
    ASSERT_FALSE(status.isOK());
}

TEST(CollectionType, MissingDefaultCollationParses) {
    const OID oid = OID::gen();
    StatusWith<CollectionType> status = CollectionType::fromBSON(BSON(
        CollectionType::kNssFieldName
        << "db.coll" << CollectionType::kEpochFieldName << oid
        << CollectionType::kUpdatedAtFieldName << Date_t::fromMillisSinceEpoch(1)
        << CollectionType::kKeyPatternFieldName << BSON("a" << 1) << CollectionType::unique(true)));
    ASSERT_TRUE(status.isOK());

    CollectionType coll = status.getValue();
    ASSERT_TRUE(coll.validate().isOK());
    ASSERT_BSONOBJ_EQ(coll.getDefaultCollation(), BSONObj());
}

TEST(CollectionType, DefaultCollationSerializesCorrectly) {
    const OID oid = OID::gen();
    StatusWith<CollectionType> status = CollectionType::fromBSON(
        BSON(CollectionType::kNssFieldName << "db.coll" << CollectionType::kEpochFieldName << oid
                                           << CollectionType::kUpdatedAtFieldName
                                           << Date_t::fromMillisSinceEpoch(1)
                                           << CollectionType::kKeyPatternFieldName << BSON("a" << 1)
                                           << CollectionType::defaultCollation(BSON("locale"
                                                                                    << "fr_CA"))
                                           << CollectionType::unique(true)));
    ASSERT_TRUE(status.isOK());

    CollectionType coll = status.getValue();
    ASSERT_TRUE(coll.validate().isOK());
    BSONObj serialized = coll.toBSON();
    ASSERT_BSONOBJ_EQ(serialized["defaultCollation"].Obj(),
                      BSON("locale"
                           << "fr_CA"));
}

TEST(CollectionType, MissingDefaultCollationIsNotSerialized) {
    const OID oid = OID::gen();
    StatusWith<CollectionType> status = CollectionType::fromBSON(BSON(
        CollectionType::kNssFieldName
        << "db.coll" << CollectionType::kEpochFieldName << oid
        << CollectionType::kUpdatedAtFieldName << Date_t::fromMillisSinceEpoch(1)
        << CollectionType::kKeyPatternFieldName << BSON("a" << 1) << CollectionType::unique(true)));
    ASSERT_TRUE(status.isOK());

    CollectionType coll = status.getValue();
    ASSERT_TRUE(coll.validate().isOK());
    BSONObj serialized = coll.toBSON();
    ASSERT_FALSE(serialized["defaultCollation"]);
}

TEST(CollectionType, MissingDistributionModeImpliesDistributionModeSharded) {
    const OID oid = OID::gen();
    StatusWith<CollectionType> status = CollectionType::fromBSON(BSON(
        CollectionType::kNssFieldName
        << "db.coll" << CollectionType::kEpochFieldName << oid
        << CollectionType::kUpdatedAtFieldName << Date_t::fromMillisSinceEpoch(1)
        << CollectionType::kKeyPatternFieldName << BSON("a" << 1) << CollectionType::unique(true)));
    ASSERT_TRUE(status.isOK());

    CollectionType coll = status.getValue();
    ASSERT_TRUE(coll.validate().isOK());

    ASSERT(CollectionType::DistributionMode::kSharded == coll.getDistributionMode());

    // Since the distributionMode was not explicitly set, it does not get serialized.
    BSONObj serialized = coll.toBSON();
    ASSERT_FALSE(serialized["distributionMode"]);
}

TEST(CollectionType, DistributionModeUnshardedParses) {
    const OID oid = OID::gen();
    StatusWith<CollectionType> status = CollectionType::fromBSON(
        BSON(CollectionType::kNssFieldName
             << "db.coll" << CollectionType::kEpochFieldName << oid
             << CollectionType::kUpdatedAtFieldName << Date_t::fromMillisSinceEpoch(1)
             << CollectionType::kKeyPatternFieldName << BSON("a" << 1)
             << CollectionType::unique(true) << CollectionType::distributionMode("unsharded")));
    ASSERT_TRUE(status.isOK());

    CollectionType coll = status.getValue();
    ASSERT_TRUE(coll.validate().isOK());

    ASSERT(CollectionType::DistributionMode::kUnsharded == coll.getDistributionMode());

    BSONObj serialized = coll.toBSON();
    ASSERT("unsharded" == serialized["distributionMode"].str());
}

TEST(CollectionType, DistributionModeShardedParses) {
    const OID oid = OID::gen();
    StatusWith<CollectionType> status = CollectionType::fromBSON(
        BSON(CollectionType::kNssFieldName
             << "db.coll" << CollectionType::kEpochFieldName << oid
             << CollectionType::kUpdatedAtFieldName << Date_t::fromMillisSinceEpoch(1)
             << CollectionType::kKeyPatternFieldName << BSON("a" << 1)
             << CollectionType::unique(true) << CollectionType::distributionMode("sharded")));
    ASSERT_TRUE(status.isOK());

    CollectionType coll = status.getValue();
    ASSERT_TRUE(coll.validate().isOK());

    ASSERT(CollectionType::DistributionMode::kSharded == coll.getDistributionMode());

    BSONObj serialized = coll.toBSON();
    ASSERT("sharded" == serialized["distributionMode"].str());
}

TEST(CollectionType, UnknownDistributionModeFailsToParse) {
    const OID oid = OID::gen();
    StatusWith<CollectionType> status = CollectionType::fromBSON(
        BSON(CollectionType::kNssFieldName
             << "db.coll" << CollectionType::kEpochFieldName << oid
             << CollectionType::kUpdatedAtFieldName << Date_t::fromMillisSinceEpoch(1)
             << CollectionType::kKeyPatternFieldName << BSON("a" << 1)
             << CollectionType::unique(true) << CollectionType::distributionMode("badvalue")));
    ASSERT_EQUALS(ErrorCodes::FailedToParse, status.getStatus());
}

TEST(CollectionType, HasSameOptionsReturnsTrueIfBothDistributionModesExplicitlySetToUnsharded) {
    const auto collType1 = uassertStatusOK(CollectionType::fromBSON(
        BSON(CollectionType::kNssFieldName
             << "db.coll" << CollectionType::kEpochFieldName << OID::gen()
             << CollectionType::kUpdatedAtFieldName << Date_t::fromMillisSinceEpoch(1)
             << CollectionType::kKeyPatternFieldName << BSON("a" << 1)
             << CollectionType::unique(true) << CollectionType::distributionMode("unsharded"))));

    const auto collType2 = uassertStatusOK(CollectionType::fromBSON(
        BSON(CollectionType::kNssFieldName
             << "db.coll" << CollectionType::kEpochFieldName << OID::gen()
             << CollectionType::kUpdatedAtFieldName << Date_t::fromMillisSinceEpoch(1)
             << CollectionType::kKeyPatternFieldName << BSON("a" << 1)
             << CollectionType::unique(true) << CollectionType::distributionMode("unsharded"))));

    ASSERT(collType1.hasSameOptions(collType2));
    ASSERT(collType2.hasSameOptions(collType1));
}

TEST(CollectionType, HasSameOptionsReturnsTrueIfBothDistributionModesExplicitlySetToSharded) {
    const auto collType1 = uassertStatusOK(CollectionType::fromBSON(
        BSON(CollectionType::kNssFieldName
             << "db.coll" << CollectionType::kEpochFieldName << OID::gen()
             << CollectionType::kUpdatedAtFieldName << Date_t::fromMillisSinceEpoch(1)
             << CollectionType::kKeyPatternFieldName << BSON("a" << 1)
             << CollectionType::unique(true) << CollectionType::distributionMode("sharded"))));

    const auto collType2 = uassertStatusOK(CollectionType::fromBSON(
        BSON(CollectionType::kNssFieldName
             << "db.coll" << CollectionType::kEpochFieldName << OID::gen()
             << CollectionType::kUpdatedAtFieldName << Date_t::fromMillisSinceEpoch(1)
             << CollectionType::kKeyPatternFieldName << BSON("a" << 1)
             << CollectionType::unique(true) << CollectionType::distributionMode("sharded"))));

    ASSERT(collType1.hasSameOptions(collType2));
    ASSERT(collType2.hasSameOptions(collType1));
}

TEST(
    CollectionType,
    HasSameOptionsReturnsFalseIfOneDistributionModeExplicitlySetToUnshardedAndOtherExplicitlySetToSharded) {
    const auto collType1 = uassertStatusOK(CollectionType::fromBSON(
        BSON(CollectionType::kNssFieldName
             << "db.coll" << CollectionType::kEpochFieldName << OID::gen()
             << CollectionType::kUpdatedAtFieldName << Date_t::fromMillisSinceEpoch(1)
             << CollectionType::kKeyPatternFieldName << BSON("a" << 1)
             << CollectionType::unique(true) << CollectionType::distributionMode("unsharded"))));

    const auto collType2 = uassertStatusOK(CollectionType::fromBSON(
        BSON(CollectionType::kNssFieldName
             << "db.coll" << CollectionType::kEpochFieldName << OID::gen()
             << CollectionType::kUpdatedAtFieldName << Date_t::fromMillisSinceEpoch(1)
             << CollectionType::kKeyPatternFieldName << BSON("a" << 1)
             << CollectionType::unique(true) << CollectionType::distributionMode("sharded"))));

    ASSERT(!collType1.hasSameOptions(collType2));
    ASSERT(!collType2.hasSameOptions(collType1));
}

TEST(CollectionType,
     HasSameOptionsReturnsTrueIfOneDistributionModeExplicitlySetToShardedAndOtherIsNotSet) {
    const auto collType1 = uassertStatusOK(CollectionType::fromBSON(
        BSON(CollectionType::kNssFieldName
             << "db.coll" << CollectionType::kEpochFieldName << OID::gen()
             << CollectionType::kUpdatedAtFieldName << Date_t::fromMillisSinceEpoch(1)
             << CollectionType::kKeyPatternFieldName << BSON("a" << 1)
             << CollectionType::unique(true) << CollectionType::distributionMode("sharded"))));

    const auto collType2 = uassertStatusOK(CollectionType::fromBSON(
        BSON(CollectionType::kNssFieldName << "db.coll" << CollectionType::kEpochFieldName
                                           << OID::gen() << CollectionType::kUpdatedAtFieldName
                                           << Date_t::fromMillisSinceEpoch(1)
                                           << CollectionType::kKeyPatternFieldName << BSON("a" << 1)
                                           << CollectionType::unique(true))));

    ASSERT(collType1.hasSameOptions(collType2));
    ASSERT(collType2.hasSameOptions(collType1));
}

TEST(CollectionType, HasSameOptionsReturnsTrueIfNeitherDistributionModeExplicitlySet) {
    const auto collType1 = uassertStatusOK(CollectionType::fromBSON(
        BSON(CollectionType::kNssFieldName << "db.coll" << CollectionType::kEpochFieldName
                                           << OID::gen() << CollectionType::kUpdatedAtFieldName
                                           << Date_t::fromMillisSinceEpoch(1)
                                           << CollectionType::kKeyPatternFieldName << BSON("a" << 1)
                                           << CollectionType::unique(true))));

    const auto collType2 = uassertStatusOK(CollectionType::fromBSON(
        BSON(CollectionType::kNssFieldName << "db.coll" << CollectionType::kEpochFieldName
                                           << OID::gen() << CollectionType::kUpdatedAtFieldName
                                           << Date_t::fromMillisSinceEpoch(1)
                                           << CollectionType::kKeyPatternFieldName << BSON("a" << 1)
                                           << CollectionType::unique(true))));

    ASSERT(collType1.hasSameOptions(collType2));
    ASSERT(collType2.hasSameOptions(collType1));
}

TEST(CollectionType, Pre22Format) {
    CollectionType coll = assertGet(
        CollectionType::fromBSON(BSON("_id"
                                      << "db.coll"
                                      << "lastmod" << Date_t::fromMillisSinceEpoch(1) << "dropped"
                                      << false << "key" << BSON("a" << 1) << "unique" << false)));

    ASSERT(coll.getNss() == NamespaceString{"db.coll"});
    ASSERT(!coll.getEpoch().isSet());
    ASSERT_EQUALS(coll.getUpdatedAt(), Date_t::fromMillisSinceEpoch(1));
    ASSERT_BSONOBJ_EQ(coll.getKeyPattern().toBSON(), BSON("a" << 1));
    ASSERT_EQUALS(coll.getUnique(), false);
    ASSERT_EQUALS(coll.getAllowBalance(), true);
    ASSERT_EQUALS(coll.getDropped(), false);
}

TEST(CollectionType, InvalidCollectionNamespace) {
    const OID oid = OID::gen();
    StatusWith<CollectionType> result = CollectionType::fromBSON(BSON(
        CollectionType::kNssFieldName
        << "foo\\bar.coll" << CollectionType::kEpochFieldName << oid
        << CollectionType::kUpdatedAtFieldName << Date_t::fromMillisSinceEpoch(1)
        << CollectionType::kKeyPatternFieldName << BSON("a" << 1) << CollectionType::unique(true)));
    ASSERT_NOT_OK(result.getStatus());
}

TEST(CollectionType, BadType) {
    const OID oid = OID::gen();
    StatusWith<CollectionType> result = CollectionType::fromBSON(
        BSON(CollectionType::kNssFieldName
             << 1 << CollectionType::kEpochFieldName << oid << CollectionType::kUpdatedAtFieldName
             << Date_t::fromMillisSinceEpoch(1) << CollectionType::kKeyPatternFieldName
             << BSON("a" << 1) << CollectionType::unique(true)));
    ASSERT_NOT_OK(result.getStatus());
}

}  // namespace
}  // namespace mongo
