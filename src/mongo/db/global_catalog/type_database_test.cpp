// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/global_catalog/type_database_gen.h"
#include "mongo/db/sharding_environment/shard_id.h"
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
    ASSERT_EQUALS(db.getPrimary(), "shard");
    ASSERT_EQUALS(db.getVersion().getUuid(), uuid);
    ASSERT_EQUALS(db.getVersion().getLastMod(), 0);
}

TEST(DatabaseType, ShardIdPrimaryRoundTrip) {
    UUID versionUUID = UUID::gen();
    Timestamp timestamp = Timestamp(2, 2);
    DatabaseType db(DatabaseName::createDatabaseName_forTest(boost::none, "testdb"),
                    ShardId("myShard"),
                    DatabaseVersion(versionUUID, timestamp));

    const auto serialized = db.toBSON();
    const auto parsed = DatabaseType::parse(serialized, IDLParserContext("DatabaseType"));
    ASSERT_EQUALS(parsed.getPrimary(), ShardId("myShard"));
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

}  // unnamed namespace
