// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/global_catalog/type_collection.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/time_support.h"

#include <boost/optional/optional.hpp>

namespace mongo {
namespace {


TEST(CollectionType, Empty) {
    ASSERT_THROWS(CollectionType(BSONObj()), DBException);
}

TEST(CollectionType, Basic) {
    const OID oid = OID::gen();
    const UUID uuid = UUID::gen();
    const Timestamp timestamp(1, 1);
    CollectionType coll(
        BSON(CollectionType::kNssFieldName
             << "db.coll" << CollectionType::kEpochFieldName << oid
             << CollectionType::kUuidFieldName << uuid << CollectionType::kTimestampFieldName
             << timestamp << CollectionType::kUpdatedAtFieldName << Date_t::fromMillisSinceEpoch(1)
             << CollectionType::kKeyPatternFieldName << BSON("a" << 1)
             << CollectionType::kDefaultCollationFieldName << BSON("locale" << "fr_CA")
             << CollectionType::kUniqueFieldName << true));

    ASSERT(coll.getNss() == NamespaceString::createNamespaceString_forTest("db.coll"));
    ASSERT_EQUALS(coll.getEpoch(), oid);
    ASSERT_EQUALS(coll.getUuid(), uuid);
    ASSERT_EQUALS(coll.getTimestamp(), timestamp);
    ASSERT_EQUALS(coll.getUpdatedAt(), Date_t::fromMillisSinceEpoch(1));
    ASSERT_BSONOBJ_EQ(coll.getKeyPattern().toBSON(), BSON("a" << 1));
    ASSERT_BSONOBJ_EQ(coll.getDefaultCollation(), BSON("locale" << "fr_CA"));
    ASSERT_EQUALS(coll.getUnique(), true);
    ASSERT_EQUALS(coll.getAllowBalance(), true);
}

TEST(CollectionType, AllFieldsPresent) {
    const OID oid = OID::gen();
    const auto uuid = UUID::gen();
    const Timestamp timestamp(1, 1);
    const auto reshardingUuid = UUID::gen();

    ReshardingFields reshardingFields;
    reshardingFields.setReshardingUUID(reshardingUuid);

    CollectionType coll(BSON(
        CollectionType::kNssFieldName
        << "db.coll" << CollectionType::kEpochFieldName << oid
        << CollectionType::kTimestampFieldName << timestamp << CollectionType::kUpdatedAtFieldName
        << Date_t::fromMillisSinceEpoch(1) << CollectionType::kKeyPatternFieldName << BSON("a" << 1)
        << CollectionType::kDefaultCollationFieldName << BSON("locale" << "fr_CA")
        << CollectionType::kUniqueFieldName << true << CollectionType::kUuidFieldName << uuid
        << CollectionType::kReshardingFieldsFieldName << reshardingFields.toBSON()));

    ASSERT(coll.getNss() == NamespaceString::createNamespaceString_forTest("db.coll"));
    ASSERT_EQUALS(coll.getEpoch(), oid);
    ASSERT_EQUALS(coll.getTimestamp(), timestamp);
    ASSERT_EQUALS(coll.getUpdatedAt(), Date_t::fromMillisSinceEpoch(1));
    ASSERT_BSONOBJ_EQ(coll.getKeyPattern().toBSON(), BSON("a" << 1));
    ASSERT_BSONOBJ_EQ(coll.getDefaultCollation(), BSON("locale" << "fr_CA"));
    ASSERT_EQUALS(coll.getUnique(), true);
    ASSERT_EQUALS(coll.getAllowBalance(), true);
    ASSERT_EQUALS(coll.getUuid(), uuid);
    ASSERT(coll.getReshardingFields()->getState() == CoordinatorStateEnum::kUnused);
    ASSERT(coll.getReshardingFields()->getReshardingUUID() == reshardingUuid);
}

TEST(CollectionType, MissingDefaultCollationParses) {
    const OID oid = OID::gen();
    const Timestamp timestamp(1, 1);
    CollectionType coll(
        BSON(CollectionType::kNssFieldName
             << "db.coll" << CollectionType::kEpochFieldName << oid
             << CollectionType::kUuidFieldName << UUID::gen() << CollectionType::kTimestampFieldName
             << timestamp << CollectionType::kUpdatedAtFieldName << Date_t::fromMillisSinceEpoch(1)
             << CollectionType::kKeyPatternFieldName << BSON("a" << 1)
             << CollectionType::kUniqueFieldName << true));
    ASSERT_BSONOBJ_EQ(coll.getDefaultCollation(), BSONObj());
}

TEST(CollectionType, DefaultCollationSerializesCorrectly) {
    const OID oid = OID::gen();
    const Timestamp timestamp(1, 1);
    CollectionType coll(
        BSON(CollectionType::kNssFieldName
             << "db.coll" << CollectionType::kEpochFieldName << oid
             << CollectionType::kUuidFieldName << UUID::gen() << CollectionType::kTimestampFieldName
             << timestamp << CollectionType::kUpdatedAtFieldName << Date_t::fromMillisSinceEpoch(1)
             << CollectionType::kKeyPatternFieldName << BSON("a" << 1)
             << CollectionType::kDefaultCollationFieldName << BSON("locale" << "fr_CA")
             << CollectionType::kUniqueFieldName << true));
    BSONObj serialized = coll.toBSON();
    ASSERT_BSONOBJ_EQ(serialized["defaultCollation"].Obj(), BSON("locale" << "fr_CA"));
}

TEST(CollectionType, Pre22Format) {
    CollectionType coll(BSON("_id" << "db.coll" << CollectionType::kTimestampFieldName
                                   << Timestamp(1, 1) << CollectionType::kUuidFieldName
                                   << UUID::gen() << "lastmod" << Date_t::fromMillisSinceEpoch(1)
                                   << "dropped" << false << "key" << BSON("a" << 1) << "unique"
                                   << false));

    ASSERT(coll.getNss() == NamespaceString::createNamespaceString_forTest("db.coll"));
    ASSERT(!coll.getEpoch().isSet());
    ASSERT_EQUALS(coll.getUpdatedAt(), Date_t::fromMillisSinceEpoch(1));
    ASSERT_BSONOBJ_EQ(coll.getKeyPattern().toBSON(), BSON("a" << 1));
    ASSERT_EQUALS(coll.getUnique(), false);
    ASSERT_EQUALS(coll.getAllowBalance(), true);
}

TEST(CollectionType, InvalidNamespace) {
    ASSERT_THROWS(CollectionType(BSON(CollectionType::kNssFieldName
                                      << "foo\\bar.coll" << CollectionType::kEpochFieldName
                                      << OID::gen() << CollectionType::kTimestampFieldName
                                      << Timestamp(1, 1) << CollectionType::kUpdatedAtFieldName
                                      << Date_t::fromMillisSinceEpoch(1)
                                      << CollectionType::kKeyPatternFieldName << BSON("a" << 1)
                                      << CollectionType::kUniqueFieldName << true)),
                  DBException);
}

TEST(CollectionType, BadNamespaceType) {
    ASSERT_THROWS(CollectionType(BSON(CollectionType::kNssFieldName
                                      << 1 << CollectionType::kEpochFieldName << OID::gen()
                                      << CollectionType::kTimestampFieldName << Timestamp(1, 1)
                                      << CollectionType::kUpdatedAtFieldName
                                      << Date_t::fromMillisSinceEpoch(1)
                                      << CollectionType::kKeyPatternFieldName << BSON("a" << 1)
                                      << CollectionType::kUniqueFieldName << true)),
                  DBException);
}

}  // namespace
}  // namespace mongo
