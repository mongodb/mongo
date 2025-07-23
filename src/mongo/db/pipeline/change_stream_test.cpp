/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/pipeline/change_stream.h"

#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/change_stream_read_mode.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <boost/optional.hpp>

namespace mongo {

TEST(ChangeStreamTest, ReadModes) {
    for (auto readMode :
         {ChangeStreamReadMode::kStrict, ChangeStreamReadMode::kIgnoreRemovedShards}) {
        ChangeStream changeStream(readMode, ChangeStreamType::kAllDatabases, {} /* nss */);
        ASSERT_EQ(readMode, changeStream.getReadMode());
    }
}

TEST(ChangeStreamTest, CollectionLevelChangeStream) {
    boost::optional<NamespaceString> nss =
        NamespaceString::createNamespaceString_forTest("testDB.testCollection");

    ChangeStream changeStream(ChangeStreamReadMode::kStrict, ChangeStreamType::kCollection, nss);

    ASSERT_EQ(ChangeStreamType::kCollection, changeStream.getChangeStreamType());

    auto actualNss = changeStream.getNamespace();
    ASSERT_TRUE(actualNss.has_value());
    ASSERT_EQ(*nss, *actualNss);
    ASSERT_FALSE(actualNss->isDbOnly());
}

DEATH_TEST_REGEX(ChangeStreamTest,
                 CollectionLevelChangeStreamWithoutNamespace,
                 "Tripwire assertion.*10656201") {
    // Not allowed to create a collection-level change stream without an NSS.
    ASSERT_THROWS_CODE(
        ChangeStream(ChangeStreamReadMode::kStrict, ChangeStreamType::kCollection, {}),
        AssertionException,
        10656201);
}

DEATH_TEST_REGEX(ChangeStreamTest,
                 CollectionLevelChangeWithDatabaseOnlyNamespace,
                 "Tripwire assertion.*10656202") {
    // Not allowed to create a collection-level change stream without a DB-only NSS.
    ASSERT_THROWS_CODE(ChangeStream(ChangeStreamReadMode::kStrict,
                                    ChangeStreamType::kCollection,
                                    NamespaceString::createNamespaceString_forTest("testDB")),
                       AssertionException,
                       10656202);
}

TEST(ChangeStreamTest, DatabaseLevelChangeStream) {
    boost::optional<NamespaceString> nss = NamespaceString::createNamespaceString_forTest("testDB");

    ChangeStream changeStream(ChangeStreamReadMode::kStrict, ChangeStreamType::kDatabase, nss);

    ASSERT_EQ(ChangeStreamType::kDatabase, changeStream.getChangeStreamType());

    auto actualNss = changeStream.getNamespace();
    ASSERT_TRUE(actualNss.has_value());
    ASSERT_EQ(*nss, *actualNss);
    ASSERT_TRUE(actualNss->isDbOnly());
}

DEATH_TEST_REGEX(ChangeStreamTest,
                 DatabaseLevelChangeStreamWithoutNamespace,
                 "Tripwire assertion.*10656201") {
    // Not allowed to create a database-level change stream without an NSS.
    ASSERT_THROWS_CODE(ChangeStream(ChangeStreamReadMode::kStrict, ChangeStreamType::kDatabase, {}),
                       AssertionException,
                       10656201);
}

DEATH_TEST_REGEX(ChangeStreamTest,
                 DatabaseLevelChangeStreamWithCollectionNamespace,
                 "Tripwire assertion.*10656202") {
    // Not allowed to create a database-level change stream without a collection NSS.
    ASSERT_THROWS_CODE(
        ChangeStream(ChangeStreamReadMode::kStrict,
                     ChangeStreamType::kDatabase,
                     NamespaceString::createNamespaceString_forTest("testDB.testCollection")),
        AssertionException,
        10656202);
}

TEST(ChangeStreamTest, AllDatabasesChangeStream) {
    boost::optional<NamespaceString> nss;

    ChangeStream changeStream(ChangeStreamReadMode::kStrict, ChangeStreamType::kAllDatabases, nss);

    ASSERT_EQ(ChangeStreamType::kAllDatabases, changeStream.getChangeStreamType());

    ASSERT_FALSE(changeStream.getNamespace().has_value());
}

TEST(ChangeStreamTest, ChangeStreamGetTypeCollection) {
    auto nss = NamespaceString::createNamespaceString_forTest("unittest"_sd, "someCollection"_sd);
    ASSERT_EQ(ChangeStreamType::kCollection, ChangeStream::getChangeStreamType(nss));
}

TEST(ChangeStreamTest, ChangeStreamGetTypeDatabase) {
    auto nss = NamespaceString::makeCollectionlessAggregateNSS(
        NamespaceString::createNamespaceString_forTest("unittest"_sd).dbName());
    ASSERT_TRUE(nss.isCollectionlessAggregateNS());
    ASSERT_EQ(ChangeStreamType::kDatabase, ChangeStream::getChangeStreamType(nss));
}

TEST(ChangeStreamTest, ChangeStreamGetTypeAllDatabases) {
    auto nss = NamespaceString::createNamespaceString_forTest("admin"_sd);
    ASSERT_TRUE(nss.isAdminDB());
    ASSERT_EQ(ChangeStreamType::kAllDatabases, ChangeStream::getChangeStreamType(nss));
}

DEATH_TEST_REGEX(ChangeStreamTest,
                 AllDatabasesChangeStreamWithNamespace,
                 "Tripwire assertion.*10656200") {
    // Not allowed to create an all databases change stream with an NSS.
    ASSERT_THROWS_CODE(ChangeStream(ChangeStreamReadMode::kStrict,
                                    ChangeStreamType::kAllDatabases,
                                    NamespaceString::createNamespaceString_forTest("testDB")),
                       AssertionException,
                       10656200);
}

}  // namespace mongo
