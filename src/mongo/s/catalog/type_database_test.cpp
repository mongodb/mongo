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

#include "mongo/base/status_with.h"
#include "mongo/db/jsobj.h"
#include "mongo/s/catalog/type_database_gen.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

namespace {

using namespace mongo;
using std::string;

TEST(DatabaseType, Empty) {
    // Constructing from empty BSON must fails
    ASSERT_THROWS(DatabaseType::parse(IDLParserContext("DatabaseType"), BSONObj()),
                  AssertionException);
}

TEST(DatabaseType, Basic) {
    UUID uuid = UUID::gen();
    Timestamp timestamp = Timestamp(1, 1);
    const auto dbObj =
        BSON(DatabaseType::kNameFieldName
             << "mydb" << DatabaseType::kPrimaryFieldName << "shard"
             << DatabaseType::kShardedFieldName << true << DatabaseType::kVersionFieldName
             << BSON("uuid" << uuid << "lastMod" << 0 << "timestamp" << timestamp));

    const auto db = DatabaseType::parse(IDLParserContext("DatabaseType"), dbObj);
    ASSERT_EQUALS(db.getName(), "mydb");
    ASSERT_EQUALS(db.getPrimary(), "shard");
    ASSERT_TRUE(db.getSharded());
    ASSERT_EQUALS(db.getVersion().getUuid(), uuid);
    ASSERT_EQUALS(db.getVersion().getLastMod(), 0);
}

TEST(DatabaseType, BadType) {
    // Cosntructing from an BSON object with a malformed database must fails
    const auto dbObj = BSON(DatabaseType::kNameFieldName << 0);
    ASSERT_THROWS(DatabaseType::parse(IDLParserContext("DatabaseType"), dbObj), AssertionException);
}

TEST(DatabaseType, MissingRequired) {
    // Cosntructing from an BSON object without all the required fields must fails
    const auto dbObj = BSON(DatabaseType::kNameFieldName << "mydb");
    ASSERT_THROWS(DatabaseType::parse(IDLParserContext("DatabaseType"), dbObj), AssertionException);
}

}  // unnamed namespace
