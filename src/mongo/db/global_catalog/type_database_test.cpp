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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/global_catalog/type_database_gen.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/shard_ref.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

#include <string>

namespace {

using namespace mongo;
using std::string;

TEST(DatabaseType, Empty) {
    // Constructing from empty BSON must fails
    ASSERT_THROWS(DatabaseType::parse(BSONObj(), IDLParserContext("DatabaseType")),
                  AssertionException);
}

TEST(DatabaseType, Basic) {
    UUID uuid = UUID::gen();
    Timestamp timestamp = Timestamp(1, 1);
    const auto dbObj = BSON(DatabaseType::kDbNameFieldName
                            << "mydb" << DatabaseType::kPrimaryFieldName << "shard"
                            << DatabaseType::kVersionFieldName
                            << BSON("uuid" << uuid << "lastMod" << 0 << "timestamp" << timestamp));

    const auto db = DatabaseType::parse(dbObj, IDLParserContext("DatabaseType"));
    ASSERT_EQUALS(db.getDbName(), DatabaseName::createDatabaseName_forTest(boost::none, "mydb"));
    ASSERT_EQUALS(db.getPrimary(), ShardId{"shard"});
    ASSERT_EQUALS(db.getVersion().getUuid(), uuid);
    ASSERT_EQUALS(db.getVersion().getLastMod(), 0);
}

TEST(DatabaseType, BadType) {
    // Cosntructing from an BSON object with a malformed database must fails
    const auto dbObj = BSON(DatabaseType::kDbNameFieldName << 0);
    ASSERT_THROWS(DatabaseType::parse(dbObj, IDLParserContext("DatabaseType")), AssertionException);
}

TEST(DatabaseType, MissingRequired) {
    // Cosntructing from an BSON object without all the required fields must fails
    const auto dbObj = BSON(DatabaseType::kDbNameFieldName << "mydb");
    ASSERT_THROWS(DatabaseType::parse(dbObj, IDLParserContext("DatabaseType")), AssertionException);
}

TEST(DatabaseType, BasicUUIDPrimary) {
    // A document with a UUID primary should parse correctly and report isUUID().
    UUID primaryUUID = UUID::gen();
    UUID versionUUID = UUID::gen();
    Timestamp timestamp = Timestamp(1, 1);
    BSONObjBuilder dbObjBuilder;
    dbObjBuilder.append(DatabaseType::kDbNameFieldName, "mydb");
    primaryUUID.appendToBuilder(&dbObjBuilder, DatabaseType::kPrimaryFieldName);
    dbObjBuilder.append(DatabaseType::kVersionFieldName,
                        BSON("uuid" << versionUUID << "lastMod" << 0 << "timestamp" << timestamp));
    const auto dbObj = dbObjBuilder.obj();

    const auto db = DatabaseType::parse(dbObj, IDLParserContext("DatabaseType"));
    ASSERT_TRUE(db.getPrimary().isUUID());
    ASSERT_EQUALS(db.getPrimary().getUUID(), primaryUUID);
}

TEST(DatabaseType, StringPrimaryRoundTrip) {
    // A DatabaseType with a string primary serializes and deserializes correctly.
    UUID versionUUID = UUID::gen();
    Timestamp timestamp = Timestamp(2, 2);
    DatabaseType db(DatabaseName::createDatabaseName_forTest(boost::none, "testdb"),
                    ShardRef{std::string{"myShard"}},
                    DatabaseVersion(versionUUID, timestamp));

    const auto serialized = db.toBSON();
    const auto parsed = DatabaseType::parse(serialized, IDLParserContext("DatabaseType"));
    ASSERT_EQUALS(parsed.getPrimary(), ShardId{"myShard"});
}

TEST(DatabaseType, UUIDPrimaryRoundTrip) {
    // A DatabaseType with a UUID primary serializes and deserializes correctly.
    UUID primaryUUID = UUID::gen();
    UUID versionUUID = UUID::gen();
    Timestamp timestamp = Timestamp(3, 3);
    DatabaseType db(DatabaseName::createDatabaseName_forTest(boost::none, "testdb"),
                    ShardRef{primaryUUID},
                    DatabaseVersion(versionUUID, timestamp));

    const auto serialized = db.toBSON();
    const auto parsed = DatabaseType::parse(serialized, IDLParserContext("DatabaseType"));
    ASSERT_TRUE(parsed.getPrimary().isUUID());
    ASSERT_EQUALS(parsed.getPrimary().getUUID(), primaryUUID);
}

}  // unnamed namespace
