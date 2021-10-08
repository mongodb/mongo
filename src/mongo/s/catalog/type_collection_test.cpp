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
    ASSERT_THROWS(CollectionType(BSONObj()), DBException);
}

TEST(CollectionType, Basic) {
    const OID oid = OID::gen();
    const Timestamp timestamp(1, 1);
    CollectionType coll(BSON(CollectionType::kNssFieldName
                             << "db.coll" << CollectionType::kEpochFieldName << oid
                             << CollectionType::kTimestampFieldName << timestamp
                             << CollectionType::kUpdatedAtFieldName
                             << Date_t::fromMillisSinceEpoch(1)
                             << CollectionType::kKeyPatternFieldName << BSON("a" << 1)
                             << CollectionType::kDefaultCollationFieldName
                             << BSON("locale"
                                     << "fr_CA")
                             << CollectionType::kUniqueFieldName << true));

    ASSERT(coll.getNss() == NamespaceString{"db.coll"});
    ASSERT_EQUALS(coll.getEpoch(), oid);
    ASSERT_EQUALS(coll.getTimestamp(), timestamp);
    ASSERT_EQUALS(coll.getUpdatedAt(), Date_t::fromMillisSinceEpoch(1));
    ASSERT_BSONOBJ_EQ(coll.getKeyPattern().toBSON(), BSON("a" << 1));
    ASSERT_BSONOBJ_EQ(coll.getDefaultCollation(),
                      BSON("locale"
                           << "fr_CA"));
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
        << CollectionType::kDefaultCollationFieldName
        << BSON("locale"
                << "fr_CA")
        << CollectionType::kUniqueFieldName << true << CollectionType::kUuidFieldName << uuid
        << CollectionType::kReshardingFieldsFieldName << reshardingFields.toBSON()));

    ASSERT(coll.getNss() == NamespaceString{"db.coll"});
    ASSERT_EQUALS(coll.getEpoch(), oid);
    ASSERT_EQUALS(coll.getTimestamp(), timestamp);
    ASSERT_EQUALS(coll.getUpdatedAt(), Date_t::fromMillisSinceEpoch(1));
    ASSERT_BSONOBJ_EQ(coll.getKeyPattern().toBSON(), BSON("a" << 1));
    ASSERT_BSONOBJ_EQ(coll.getDefaultCollation(),
                      BSON("locale"
                           << "fr_CA"));
    ASSERT_EQUALS(coll.getUnique(), true);
    ASSERT_EQUALS(coll.getAllowBalance(), true);
    ASSERT_EQUALS(coll.getUuid(), uuid);
    ASSERT(coll.getReshardingFields()->getState() == CoordinatorStateEnum::kUnused);
    ASSERT(coll.getReshardingFields()->getReshardingUUID() == reshardingUuid);
}

TEST(CollectionType, MissingDefaultCollationParses) {
    const OID oid = OID::gen();
    const Timestamp timestamp(1, 1);
    CollectionType coll(BSON(
        CollectionType::kNssFieldName
        << "db.coll" << CollectionType::kEpochFieldName << oid
        << CollectionType::kTimestampFieldName << timestamp << CollectionType::kUpdatedAtFieldName
        << Date_t::fromMillisSinceEpoch(1) << CollectionType::kKeyPatternFieldName << BSON("a" << 1)
        << CollectionType::kUniqueFieldName << true));
    ASSERT_BSONOBJ_EQ(coll.getDefaultCollation(), BSONObj());
}

TEST(CollectionType, DefaultCollationSerializesCorrectly) {
    const OID oid = OID::gen();
    const Timestamp timestamp(1, 1);
    CollectionType coll(BSON(CollectionType::kNssFieldName
                             << "db.coll" << CollectionType::kEpochFieldName << oid
                             << CollectionType::kTimestampFieldName << timestamp
                             << CollectionType::kUpdatedAtFieldName
                             << Date_t::fromMillisSinceEpoch(1)
                             << CollectionType::kKeyPatternFieldName << BSON("a" << 1)
                             << CollectionType::kDefaultCollationFieldName
                             << BSON("locale"
                                     << "fr_CA")
                             << CollectionType::kUniqueFieldName << true));
    BSONObj serialized = coll.toBSON();
    ASSERT_BSONOBJ_EQ(serialized["defaultCollation"].Obj(),
                      BSON("locale"
                           << "fr_CA"));
}

TEST(CollectionType, Pre22Format) {
    CollectionType coll(BSON("_id"
                             << "db.coll" << CollectionType::kTimestampFieldName << Timestamp(1, 1)
                             << "lastmod" << Date_t::fromMillisSinceEpoch(1) << "dropped" << false
                             << "key" << BSON("a" << 1) << "unique" << false));

    ASSERT(coll.getNss() == NamespaceString{"db.coll"});
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
