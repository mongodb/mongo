// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/change_stream.h"

#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/change_stream_read_mode.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"


namespace mongo {
using namespace std::literals::string_view_literals;

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

    ASSERT_EQ("ChangeStream (type: collection, mode: strict, nss: 'testDB.testCollection')",
              changeStream.toString());
}

DEATH_TEST_REGEX(ChangeStreamTestDeathTest,
                 CollectionLevelChangeStreamWithoutNamespace,
                 "Tripwire assertion.*10656201") {
    // Not allowed to create a collection-level change stream without an NSS.
    ASSERT_THROWS_CODE(
        ChangeStream(ChangeStreamReadMode::kStrict, ChangeStreamType::kCollection, {}),
        AssertionException,
        10656201);
}

DEATH_TEST_REGEX(ChangeStreamTestDeathTest,
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

    ASSERT_EQ("ChangeStream (type: database, mode: strict, nss: 'testDB')",
              changeStream.toString());
}

TEST(ChangeStreamTest, DatabaseLevelChangeStreamOnCollectionLessAggregateNS) {
    boost::optional<NamespaceString> nss =
        NamespaceString::createNamespaceString_forTest("testDB.$cmd.aggregate");

    ChangeStream changeStream(
        ChangeStreamReadMode::kIgnoreRemovedShards, ChangeStreamType::kDatabase, nss);

    ASSERT_EQ(ChangeStreamType::kDatabase, changeStream.getChangeStreamType());

    auto actualNss = changeStream.getNamespace();
    ASSERT_TRUE(actualNss.has_value());
    ASSERT_EQ(NamespaceString::createNamespaceString_forTest("testDB"), *actualNss);
    ASSERT_TRUE(actualNss->isDbOnly());

    ASSERT_EQ("ChangeStream (type: database, mode: ignoreRemovedShards, nss: 'testDB')",
              changeStream.toString());
}

DEATH_TEST_REGEX(ChangeStreamTestDeathTest,
                 DatabaseLevelChangeStreamWithoutNamespace,
                 "Tripwire assertion.*10656201") {
    // Not allowed to create a database-level change stream without an NSS.
    ASSERT_THROWS_CODE(ChangeStream(ChangeStreamReadMode::kStrict, ChangeStreamType::kDatabase, {}),
                       AssertionException,
                       10656201);
}

DEATH_TEST_REGEX(ChangeStreamTestDeathTest,
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

    ChangeStream changeStream(
        ChangeStreamReadMode::kIgnoreRemovedShards, ChangeStreamType::kAllDatabases, nss);

    ASSERT_EQ(ChangeStreamType::kAllDatabases, changeStream.getChangeStreamType());

    ASSERT_FALSE(changeStream.getNamespace().has_value());
    ASSERT_EQ("ChangeStream (type: all-databases, mode: ignoreRemovedShards)",
              changeStream.toString());
}

TEST(ChangeStreamTest, AllDatabasesChangeStreamOnAdminDB) {
    boost::optional<NamespaceString> nss = NamespaceString::createNamespaceString_forTest("admin");

    ChangeStream changeStream(ChangeStreamReadMode::kStrict, ChangeStreamType::kAllDatabases, nss);

    ASSERT_EQ(ChangeStreamType::kAllDatabases, changeStream.getChangeStreamType());

    ASSERT_FALSE(changeStream.getNamespace().has_value());
    ASSERT_EQ("ChangeStream (type: all-databases, mode: strict)", changeStream.toString());
}

DEATH_TEST_REGEX(ChangeStreamTestDeathTest,
                 AllDatabasesChangeStreamWithNamespace,
                 "Tripwire assertion.*10656200") {
    // Not allowed to create an all databases change stream with an NSS.
    ASSERT_THROWS_CODE(ChangeStream(ChangeStreamReadMode::kStrict,
                                    ChangeStreamType::kAllDatabases,
                                    NamespaceString::createNamespaceString_forTest("testDB")),
                       AssertionException,
                       10656200);
}

TEST(ChangeStreamTest, ChangeStreamGetTypeCollection) {
    auto nss = NamespaceString::createNamespaceString_forTest("unittest"sv, "someCollection"sv);
    ASSERT_EQ(ChangeStreamType::kCollection, ChangeStream::getChangeStreamType(nss));
}

TEST(ChangeStreamTest, ChangeStreamGetTypeDatabase) {
    auto nss = NamespaceString::makeCollectionlessAggregateNSS(
        NamespaceString::createNamespaceString_forTest("unittest"sv).dbName());
    ASSERT_TRUE(nss.isCollectionlessAggregateNS());
    ASSERT_EQ(ChangeStreamType::kDatabase, ChangeStream::getChangeStreamType(nss));
}

TEST(ChangeStreamTest, ChangeStreamGetTypeAllDatabases) {
    auto nss = NamespaceString::createNamespaceString_forTest("admin"sv);
    ASSERT_TRUE(nss.isAdminDB());
    ASSERT_EQ(ChangeStreamType::kAllDatabases, ChangeStream::getChangeStreamType(nss));
}

}  // namespace mongo
