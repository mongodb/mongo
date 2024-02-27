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

#include <memory>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

#include "mongo/base/status.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/optime.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_component.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/duration.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {

class NamespaceStringTest : public unittest::Test {
protected:
    NamespaceString makeNamespaceString(boost::optional<TenantId> tenantId, StringData ns) {
        return NamespaceString(tenantId, ns);
    }

    NamespaceString makeNamespaceString(const DatabaseName& dbName) {
        return NamespaceString(dbName);
    }

    NamespaceString makeNamespaceString(const DatabaseName& dbName, StringData coll) {
        return NamespaceString(dbName, coll);
    }

    NamespaceString makeNamespaceString(StringData dbName, StringData coll) {
        return NamespaceString(boost::none, dbName, coll);
    }

    NamespaceString makeNamespaceString(boost::optional<TenantId> tenantId,
                                        StringData db,
                                        StringData coll) {
        return NamespaceString(tenantId, db, coll);
    }
};

namespace {

using namespace fmt::literals;

TEST_F(NamespaceStringTest, createNamespaceString_forTest) {
    TenantId tenantId(OID::gen());
    ASSERT_EQ(NamespaceString::createNamespaceString_forTest(boost::none, "ns"),
              makeNamespaceString(boost::none, "ns"));
    ASSERT_EQ(NamespaceString::createNamespaceString_forTest(tenantId, "ns"),
              makeNamespaceString(tenantId, "ns"));

    ASSERT_EQ(NamespaceString::createNamespaceString_forTest(boost::none, "db", "coll"),
              makeNamespaceString(boost::none, "db", "coll"));
    ASSERT_EQ(NamespaceString::createNamespaceString_forTest(tenantId, "db", "coll"),
              makeNamespaceString(tenantId, "db", "coll"));

    DatabaseName dbName = DatabaseName::createDatabaseName_forTest(tenantId, "foo");
    ASSERT_EQ(NamespaceString::createNamespaceString_forTest(dbName, "coll"),
              makeNamespaceString(dbName, "coll"));
}

TEST_F(NamespaceStringTest, CheckNamespaceStringLogAttrs) {
    TenantId tenantId(OID::gen());
    DatabaseName dbName = DatabaseName::createDatabaseName_forTest(tenantId, "foo");
    NamespaceString nss = makeNamespaceString(dbName, "bar");

    startCapturingLogMessages();
    LOGV2(7311500, "Msg nss:", logAttrs(nss));

    std::string nssAsString = str::stream() << *(nss.tenantId()) << '_' << nss.ns_forTest();

    ASSERT_EQUALS(
        1, countBSONFormatLogLinesIsSubset(BSON("attr" << BSON("namespace" << nssAsString))));
    stopCapturingLogMessages();
}

TEST_F(NamespaceStringTest, Oplog) {
    ASSERT(!NamespaceString::oplog("a"));
    ASSERT(!NamespaceString::oplog("a.b"));

    ASSERT(NamespaceString::oplog("local.oplog.rs"));
    ASSERT(NamespaceString::oplog("local.oplog.foo"));
    ASSERT(NamespaceString::oplog("local.oplog.$main"));
    ASSERT(NamespaceString::oplog("local.oplog.$foo"));
}

TEST_F(NamespaceStringTest, CheckNamespaceStringConstructor) {
    ASSERT_THROWS_CODE(
        makeNamespaceString(boost::none,
                            "WhileThisDatabaseNameExceedsTheMaximumLengthForDatabaseNamesof63"),
        AssertionException,
        ErrorCodes::InvalidNamespace);
}

TEST_F(NamespaceStringTest, ListCollectionsCursorNS) {
    ASSERT(
        makeNamespaceString(boost::none, "test.$cmd.listCollections").isListCollectionsCursorNS());

    ASSERT(!makeNamespaceString(boost::none, "test.foo").isListCollectionsCursorNS());
    ASSERT(!makeNamespaceString(boost::none, "test.foo.$cmd.listCollections")
                .isListCollectionsCursorNS());
    ASSERT(!makeNamespaceString(boost::none, "test.$cmd.").isListCollectionsCursorNS());
    ASSERT(!makeNamespaceString(boost::none, "test.$cmd.foo.").isListCollectionsCursorNS());
    ASSERT(!makeNamespaceString(boost::none, "test.$cmd.listCollections.")
                .isListCollectionsCursorNS());
    ASSERT(!makeNamespaceString(boost::none, "test.$cmd.listIndexes").isListCollectionsCursorNS());
    ASSERT(
        !makeNamespaceString(boost::none, "test.$cmd.listIndexes.foo").isListCollectionsCursorNS());
}

TEST_F(NamespaceStringTest, IsCollectionlessCursorNamespace) {
    ASSERT_TRUE(makeNamespaceString(boost::none, "test.$cmd.aggregate.foo")
                    .isCollectionlessCursorNamespace());
    ASSERT_TRUE(makeNamespaceString(boost::none, "test.$cmd.listIndexes.foo")
                    .isCollectionlessCursorNamespace());
    ASSERT_TRUE(makeNamespaceString(boost::none, "test.$cmd.otherCommand.foo")
                    .isCollectionlessCursorNamespace());
    ASSERT_TRUE(makeNamespaceString(boost::none, "test.$cmd.listCollections")
                    .isCollectionlessCursorNamespace());
    ASSERT_TRUE(makeNamespaceString(boost::none, "test.$cmd.otherCommand")
                    .isCollectionlessCursorNamespace());
    ASSERT_TRUE(
        makeNamespaceString(boost::none, "test.$cmd.aggregate").isCollectionlessCursorNamespace());
    ASSERT_TRUE(makeNamespaceString(boost::none, "test.$cmd.listIndexes")
                    .isCollectionlessCursorNamespace());

    ASSERT_FALSE(makeNamespaceString(boost::none, "test.foo").isCollectionlessCursorNamespace());
    ASSERT_FALSE(makeNamespaceString(boost::none, "test.$cmd").isCollectionlessCursorNamespace());

    ASSERT_FALSE(
        makeNamespaceString(boost::none, "$cmd.aggregate.foo").isCollectionlessCursorNamespace());
    ASSERT_FALSE(
        makeNamespaceString(boost::none, "$cmd.listCollections").isCollectionlessCursorNamespace());
}

TEST_F(NamespaceStringTest, IsLegalClientSystemNamespace) {
    // Generally speaking *.system.* collections are not valid client namespaces,
    ASSERT_FALSE(
        makeNamespaceString(boost::none, "admin.system.randomness").isLegalClientSystemNS());
    ASSERT_FALSE(
        makeNamespaceString(boost::none, "test.system.randomness").isLegalClientSystemNS());

    // *.system.buckets.* is permitted, provided the collection name is valid.
    ASSERT_TRUE(
        makeNamespaceString(boost::none, "test.system.buckets.1234").isLegalClientSystemNS());
    ASSERT_TRUE(
        makeNamespaceString(boost::none, "test.system.buckets.abcde").isLegalClientSystemNS());
    ASSERT_FALSE(
        makeNamespaceString(boost::none, "test.system.buckets..1234").isLegalClientSystemNS());
    ASSERT_FALSE(
        makeNamespaceString(boost::none, "test.system.buckets.a234$").isLegalClientSystemNS());
    ASSERT_FALSE(makeNamespaceString(boost::none, "test.system.buckets.").isLegalClientSystemNS());

    // admin.system.new_users may have been created during a migration from MongoDB 2.4-2.6
    // It is no longer used by mongod, however clients with adequate permissions should still
    // be able to drop it, perform backups, etc..
    ASSERT_TRUE(makeNamespaceString(boost::none, "admin.system.new_users").isLegalClientSystemNS());

    // *.system.users is always permitted because in 2.4 and earlier's auth schema,
    // users were stored in an individual database's db.system.users collection.
    ASSERT_TRUE(makeNamespaceString(boost::none, "admin.system.users").isLegalClientSystemNS());
    ASSERT_TRUE(makeNamespaceString(boost::none, "test.system.users").isLegalClientSystemNS());

    // By contrast, .system.roles is only allowed in the admin database.
    ASSERT_TRUE(makeNamespaceString(boost::none, "admin.system.roles").isLegalClientSystemNS());
    ASSERT_FALSE(makeNamespaceString(boost::none, "test.system.roles").isLegalClientSystemNS());
}

TEST_F(NamespaceStringTest, IsDropPendingNamespace) {
    ASSERT_TRUE(
        makeNamespaceString(boost::none, "test.system.drop.0i0t-1.foo").isDropPendingNamespace());
    ASSERT_TRUE(makeNamespaceString(boost::none, "test.system.drop.1234567i8t9.foo")
                    .isDropPendingNamespace());
    ASSERT_TRUE(
        makeNamespaceString(boost::none, "test.system.drop.1234.foo").isDropPendingNamespace());
    ASSERT_TRUE(makeNamespaceString(boost::none, "test.system.drop.foo").isDropPendingNamespace());

    ASSERT_FALSE(makeNamespaceString(boost::none, "test.system.drop").isDropPendingNamespace());
    ASSERT_FALSE(makeNamespaceString(boost::none, "test.drop.1234.foo").isDropPendingNamespace());
    ASSERT_FALSE(makeNamespaceString(boost::none, "test.drop.foo").isDropPendingNamespace());
    ASSERT_FALSE(makeNamespaceString(boost::none, "test.foo").isDropPendingNamespace());
    ASSERT_FALSE(makeNamespaceString(boost::none, "test.$cmd").isDropPendingNamespace());

    ASSERT_FALSE(makeNamespaceString(boost::none, "$cmd.aggregate.foo").isDropPendingNamespace());
    ASSERT_FALSE(makeNamespaceString(boost::none, "$cmd.listCollections").isDropPendingNamespace());
}

TEST_F(NamespaceStringTest, MakeDropPendingNamespace) {
    ASSERT_EQUALS(
        makeNamespaceString(boost::none, "test.system.drop.0i0t-1.foo"),
        makeNamespaceString(boost::none, "test.foo").makeDropPendingNamespace(repl::OpTime()));
    ASSERT_EQUALS(
        makeNamespaceString(boost::none, "test.system.drop.1234567i8t9.foo"),
        makeNamespaceString(boost::none, "test.foo")
            .makeDropPendingNamespace(repl::OpTime(Timestamp(Seconds(1234567), 8U), 9LL)));

    std::string collName(NamespaceString::MaxNsCollectionLen, 't');
    NamespaceString nss = makeNamespaceString(boost::none, "test", collName);
    ASSERT_EQUALS(makeNamespaceString(boost::none, "test.system.drop.1234567i8t9." + collName),
                  nss.makeDropPendingNamespace(repl::OpTime(Timestamp(Seconds(1234567), 8U), 9LL)));
}

TEST_F(NamespaceStringTest, GetDropPendingNamespaceOpTime) {
    // Null optime is acceptable.
    ASSERT_EQUALS(
        repl::OpTime(),
        unittest::assertGet(makeNamespaceString(boost::none, "test.system.drop.0i0t-1.foo")
                                .getDropPendingNamespaceOpTime()));

    // Valid optime.
    ASSERT_EQUALS(
        repl::OpTime(Timestamp(Seconds(1234567), 8U), 9LL),
        unittest::assertGet(makeNamespaceString(boost::none, "test.system.drop.1234567i8t9.foo")
                                .getDropPendingNamespaceOpTime()));

    // Original collection name is optional.
    ASSERT_EQUALS(
        repl::OpTime(Timestamp(Seconds(1234567), 8U), 9LL),
        unittest::assertGet(makeNamespaceString(boost::none, "test.system.drop.1234567i8t9")
                                .getDropPendingNamespaceOpTime()));

    // No system.drop. prefix.
    ASSERT_EQUALS(
        ErrorCodes::BadValue,
        makeNamespaceString(boost::none, "test.1234.foo").getDropPendingNamespaceOpTime());

    // Missing 'i' separator.
    ASSERT_EQUALS(ErrorCodes::FailedToParse,
                  makeNamespaceString(boost::none, "test.system.drop.1234t8.foo")
                      .getDropPendingNamespaceOpTime());

    // Missing 't' separator.
    ASSERT_EQUALS(ErrorCodes::FailedToParse,
                  makeNamespaceString(boost::none, "test.system.drop.1234i56.foo")
                      .getDropPendingNamespaceOpTime());

    // Timestamp seconds is not a number.
    ASSERT_EQUALS(ErrorCodes::FailedToParse,
                  makeNamespaceString(boost::none, "test.system.drop.wwwi56t123.foo")
                      .getDropPendingNamespaceOpTime());

    // Timestamp increment is not a number.
    ASSERT_EQUALS(ErrorCodes::FailedToParse,
                  makeNamespaceString(boost::none, "test.system.drop.1234iaaat123.foo")
                      .getDropPendingNamespaceOpTime());

    // Timestamp increment must be an unsigned number.
    ASSERT_EQUALS(ErrorCodes::FailedToParse,
                  makeNamespaceString(boost::none, "test.system.drop.1234i-100t123.foo")
                      .getDropPendingNamespaceOpTime());

    // Term is not a number.
    ASSERT_EQUALS(ErrorCodes::FailedToParse,
                  makeNamespaceString(boost::none, "test.system.drop.1234i111taaa.foo")
                      .getDropPendingNamespaceOpTime());
}

TEST_F(NamespaceStringTest, CollectionComponentValidNamesWithNamespaceString) {
    ASSERT(NamespaceString::validCollectionComponent(makeNamespaceString(boost::none, "a.b")));
    ASSERT(!NamespaceString::validCollectionComponent(makeNamespaceString(boost::none, "a.")));
    ASSERT_THROWS_CODE(
        NamespaceString::validCollectionComponent(makeNamespaceString(boost::none, "a..foo")),
        AssertionException,
        ErrorCodes::InvalidNamespace);
    ASSERT(NamespaceString::validCollectionComponent(makeNamespaceString(boost::none, "a.b.")));
}

TEST_F(NamespaceStringTest, CollectionValidNames) {
    ASSERT(NamespaceString::validCollectionName("a"));
    ASSERT(NamespaceString::validCollectionName("a.b"));
    ASSERT(NamespaceString::validCollectionName("a."));    // TODO: should this change?
    ASSERT(NamespaceString::validCollectionName("a.b."));  // TODO: should this change?
    ASSERT(!NamespaceString::validCollectionName(".a"));
    ASSERT(!NamespaceString::validCollectionName("$a"));
    ASSERT(!NamespaceString::validCollectionName("a$b"));
    ASSERT(!NamespaceString::validCollectionName(""));
    ASSERT(!NamespaceString::validCollectionName("a\0b"_sd));
}

TEST_F(NamespaceStringTest, DbForSharding) {
    ASSERT_EQ(makeNamespaceString("foo", "bar").db_forSharding(), "foo");
    ASSERT_EQ(makeNamespaceString("foo", "").db_forSharding(), "foo");
}

TEST_F(NamespaceStringTest, nsToDatabase1) {
    ASSERT_EQUALS("foo", nsToDatabaseSubstring("foo.bar"));
    ASSERT_EQUALS("foo", nsToDatabaseSubstring("foo"));
    ASSERT_EQUALS("foo", nsToDatabase("foo.bar"));
    ASSERT_EQUALS("foo", nsToDatabase("foo"));
    ASSERT_EQUALS("foo", nsToDatabase(std::string("foo.bar")));
    ASSERT_EQUALS("foo", nsToDatabase(std::string("foo")));
}

TEST_F(NamespaceStringTest, NamespaceStringParse1) {
    NamespaceString ns = makeNamespaceString(boost::none, "a.b");
    ASSERT_EQUALS(std::string("a"), ns.db_forTest());
    ASSERT_EQUALS(std::string("b"), ns.coll());
}

TEST_F(NamespaceStringTest, NamespaceStringParse2) {
    NamespaceString ns = makeNamespaceString(boost::none, "a.b.c");
    ASSERT_EQUALS(std::string("a"), ns.db_forTest());
    ASSERT_EQUALS(std::string("b.c"), ns.coll());
}

TEST_F(NamespaceStringTest, NamespaceStringParse3) {
    NamespaceString ns = makeNamespaceString(boost::none, "abc");
    ASSERT_EQUALS(std::string("abc"), ns.db_forTest());
    ASSERT_EQUALS(std::string(""), ns.coll());
}

TEST_F(NamespaceStringTest, NamespaceStringParse4) {
    NamespaceString ns = makeNamespaceString(boost::none, "abc.");
    ASSERT_EQUALS(std::string("abc"), ns.db_forTest());
    ASSERT(ns.coll().empty());
}

TEST_F(NamespaceStringTest, NamespaceStringParse5) {
    NamespaceString ns = makeNamespaceString(boost::none, "abc", "");
    ASSERT_EQUALS(std::string("abc"), ns.db_forTest());
    ASSERT(ns.coll().empty());
}

TEST_F(NamespaceStringTest, makeListCollectionsNSIsCorrect) {
    NamespaceString ns = NamespaceString::makeListCollectionsNSS(
        DatabaseName::createDatabaseName_forTest(boost::none, "DB"));
    ASSERT_EQUALS("DB", ns.db_forTest());
    ASSERT_EQUALS("$cmd.listCollections", ns.coll());
    ASSERT(ns.isValid());
    ASSERT(NamespaceString::isValid(ns.ns_forTest()));
    ASSERT(ns.isListCollectionsCursorNS());
}

TEST_F(NamespaceStringTest, EmptyNSStringReturnsEmptyColl) {
    NamespaceString nss{};
    ASSERT_TRUE(nss.isEmpty());
    ASSERT_EQ(nss.coll(), StringData{});
}

TEST_F(NamespaceStringTest, EmptyNSStringReturnsEmptyDb) {
    NamespaceString nss{};
    ASSERT_TRUE(nss.isEmpty());
    ASSERT_EQ(nss.db_forTest(), StringData{});
}

TEST_F(NamespaceStringTest, EmptyDbWithColl) {
    NamespaceString nss = makeNamespaceString(boost::none, "", "coll");
    ASSERT_EQ(nss.db_forTest(), StringData{});
    ASSERT_EQ(nss.coll(), "coll");
    ASSERT_EQ(nss.dbName(), DatabaseName::kEmpty);
    ASSERT_EQ(nss.dbName().compare(DatabaseName::kEmpty), 0);
}

TEST_F(NamespaceStringTest, NSSWithTenantId) {
    TenantId tenantId(OID::gen());

    {
        std::string tenantNsStr = str::stream() << tenantId.toString() << "_foo.bar";
        NamespaceString nss = makeNamespaceString(tenantId, "foo.bar");
        DatabaseName db = DatabaseName::createDatabaseName_forTest(tenantId, "foo");

        ASSERT_EQ(nss.size(), 7);
        ASSERT_EQ(nss.ns_forTest(), "foo.bar");
        ASSERT_EQ(nss.toString_forTest(), "foo.bar");
        ASSERT_EQ(nss.toStringWithTenantId_forTest(), tenantNsStr);
        ASSERT_EQ(nss.db_forTest(), "foo");
        ASSERT_EQ(nss.coll(), "bar");
        ASSERT_EQ(nss.dbName(), db);
        ASSERT_EQ(nss.dbName().toString_forTest(), "foo");
        ASSERT_EQ(nss.dbName().size(), db.size());
        ASSERT_EQ(nss.size(), 7);
        ASSERT(nss.tenantId());
        ASSERT(nss.dbName().tenantId());
        ASSERT_EQ(*nss.tenantId(), tenantId);
        ASSERT_EQ(*nss.dbName().tenantId(), tenantId);
    }

    {
        std::string tenantNsStr = str::stream() << tenantId.toString() << "_foo";
        NamespaceString nss = makeNamespaceString(tenantId, "foo");
        ASSERT_EQ(nss.size(), 3);
        ASSERT_EQ(nss.ns_forTest(), "foo");
        ASSERT_EQ(nss.toString_forTest(), "foo");
        ASSERT_EQ(nss.toStringWithTenantId_forTest(), tenantNsStr);
        ASSERT_EQ(nss.db_forTest(), "foo");
        ASSERT_EQ(nss.coll(), "");
        ASSERT_EQ(nss.dbName().toString_forTest(), "foo");
        ASSERT_EQ(nss.size(), 3);
        ASSERT(nss.tenantId());
        ASSERT(nss.dbName().tenantId());
        ASSERT_EQ(*nss.tenantId(), tenantId);
        ASSERT_EQ(*nss.dbName().tenantId(), tenantId);
    }

    {
        std::string tenantNsStr = str::stream() << tenantId.toString() << "_foo.bar";
        DatabaseName dbName = DatabaseName::createDatabaseName_forTest(tenantId, "foo");
        NamespaceString nss2 = makeNamespaceString(dbName, "bar");
        ASSERT_EQ(nss2.size(), 7);
        ASSERT_EQ(nss2.ns_forTest(), "foo.bar");
        ASSERT_EQ(nss2.toString_forTest(), "foo.bar");
        ASSERT_EQ(nss2.toStringWithTenantId_forTest(), tenantNsStr);
        ASSERT_EQ(nss2.db_forTest(), "foo");
        ASSERT_EQ(nss2.coll(), "bar");
        ASSERT_EQ(nss2.dbName().toString_forTest(), "foo");
        ASSERT(nss2.tenantId());
        ASSERT(nss2.dbName().tenantId());
        ASSERT_EQ(*nss2.tenantId(), tenantId);
        ASSERT_EQ(*nss2.dbName().tenantId(), tenantId);
    }

    {
        std::string tenantNsStr = str::stream() << tenantId.toString() << "_foo.bar";
        NamespaceString nss3 = makeNamespaceString(tenantId, "foo", "bar");
        ASSERT_EQ(nss3.size(), 7);
        ASSERT_EQ(nss3.ns_forTest(), "foo.bar");
        ASSERT_EQ(nss3.toString_forTest(), "foo.bar");
        ASSERT_EQ(nss3.toStringWithTenantId_forTest(), tenantNsStr);
        ASSERT_EQ(nss3.db_forTest(), "foo");
        ASSERT_EQ(nss3.coll(), "bar");
        ASSERT_EQ(nss3.dbName().toString_forTest(), "foo");
        ASSERT(nss3.tenantId());
        ASSERT(nss3.dbName().tenantId());
        ASSERT_EQ(*nss3.tenantId(), tenantId);
        ASSERT_EQ(*nss3.dbName().tenantId(), tenantId);
    }

    {
        DatabaseName dbName = DatabaseName::createDatabaseName_forTest(tenantId, "foo");
        NamespaceString nss4(dbName);
        ASSERT_EQ(nss4.size(), 3);
        ASSERT_EQ(nss4.ns_forTest(), "foo");
        ASSERT_EQ(nss4.toString_forTest(), "foo");
        ASSERT_EQ(nss4.toStringWithTenantId_forTest(), "{}_foo"_format(tenantId.toString()));
        ASSERT_EQ(nss4.db_forTest(), "foo");
        ASSERT_EQ(nss4.coll(), "");
        ASSERT_EQ(nss4.dbName().toString_forTest(), "foo");
        ASSERT(nss4.tenantId());
        ASSERT(nss4.dbName().tenantId());
        ASSERT_EQ(*nss4.tenantId(), tenantId);
        ASSERT_EQ(*nss4.dbName().tenantId(), tenantId);
    }

    {
        NamespaceString multiNss = makeNamespaceString(tenantId, "config.system.change_collection");
        ASSERT(multiNss.isConfigDB());
        ASSERT_EQ(multiNss.size(), 31);
        ASSERT_EQ(multiNss.ns_forTest(), "config.system.change_collection");
        ASSERT_EQ(multiNss.toString_forTest(), "config.system.change_collection");
        ASSERT_EQ(multiNss.toStringWithTenantId_forTest(),
                  "{}_config.system.change_collection"_format(tenantId.toString()));
        ASSERT_EQ(multiNss.db_forTest(), "config");
        ASSERT_EQ(multiNss.coll(), "system.change_collection");
        ASSERT_EQ(multiNss.dbName().toString_forTest(), "config");
        ASSERT(multiNss.tenantId());
        ASSERT(multiNss.dbName().tenantId());
        ASSERT_EQ(*multiNss.tenantId(), tenantId);
        ASSERT_EQ(*multiNss.dbName().tenantId(), tenantId);
    }

    {
        NamespaceString empty{};
        ASSERT_EQ(empty.size(), 0);
        ASSERT_EQ(empty.coll(), "");
        ASSERT_EQ(empty.tenantId(), boost::none);
        ASSERT_EQ(empty.toString_forTest(), "");
        ASSERT_EQ(empty.toStringWithTenantId_forTest(), "");
        ASSERT_EQ(empty.dbName().tenantId(), boost::none);
        ASSERT_EQ(empty.dbName().toString_forTest(), "");
        ASSERT_EQ(empty.dbName().toStringWithTenantId_forTest(), "");
    }

    {
        NamespaceString emptyWithTenant = makeNamespaceString(tenantId, "");
        ASSERT_EQ(emptyWithTenant.size(), 0);
        ASSERT_EQ(emptyWithTenant.coll(), "");
        ASSERT(emptyWithTenant.tenantId());
        ASSERT_EQ(*emptyWithTenant.tenantId(), tenantId);
        ASSERT_EQ(emptyWithTenant.toString_forTest(), "");
        ASSERT_EQ(emptyWithTenant.toStringWithTenantId_forTest(),
                  "{}_"_format(tenantId.toString()));
        ASSERT(emptyWithTenant.dbName().tenantId());
        ASSERT_EQ(emptyWithTenant.dbName().tenantId(), tenantId);
        ASSERT_EQ(emptyWithTenant.dbName().toString_forTest(), "");
        ASSERT_EQ(emptyWithTenant.dbName().toStringWithTenantId_forTest(),
                  "{}_"_format(tenantId.toString()));
    }

    {
        NamespaceString dbWithoutColl = makeNamespaceString(boost::none, "foo");
        ASSERT_EQ(dbWithoutColl.size(), 3);
        ASSERT_EQ(dbWithoutColl.coll(), "");
        ASSERT_FALSE(dbWithoutColl.tenantId());
        ASSERT_EQ(dbWithoutColl.toString_forTest(), "foo");
        ASSERT_EQ(dbWithoutColl.toStringWithTenantId_forTest(), "foo");
        ASSERT_FALSE(dbWithoutColl.dbName().tenantId());
        ASSERT_EQ(dbWithoutColl.dbName().toString_forTest(), "foo");
        ASSERT_EQ(dbWithoutColl.dbName().toStringWithTenantId_forTest(), "foo");
    }

    {
        NamespaceString dbWithoutCollWithTenant = makeNamespaceString(tenantId, "foo");
        ASSERT_EQ(dbWithoutCollWithTenant.size(), 3);
        ASSERT_EQ(dbWithoutCollWithTenant.coll(), "");
        ASSERT(dbWithoutCollWithTenant.tenantId());
        ASSERT_EQ(*dbWithoutCollWithTenant.tenantId(), tenantId);
        ASSERT_EQ(dbWithoutCollWithTenant.toString_forTest(), "foo");
        ASSERT_EQ(dbWithoutCollWithTenant.toStringWithTenantId_forTest(),
                  fmt::format("{}_foo", tenantId.toString()));
        ASSERT(dbWithoutCollWithTenant.dbName().tenantId());
        ASSERT_EQ(dbWithoutCollWithTenant.dbName().tenantId(), tenantId);
        ASSERT_EQ(dbWithoutCollWithTenant.dbName().toString_forTest(), "foo");
        ASSERT_EQ(dbWithoutCollWithTenant.dbName().toStringWithTenantId_forTest(),
                  fmt::format("{}_foo", tenantId.toString()));
    }
}

TEST_F(NamespaceStringTest, NSSNoCollectionWithTenantId) {
    TenantId tenantId(OID::gen());
    std::string tenantNsStr = str::stream() << tenantId.toString() << "_foo";

    NamespaceString nss = makeNamespaceString(tenantId, "foo");

    ASSERT_EQ(nss.ns_forTest(), "foo");
    ASSERT_EQ(nss.toString_forTest(), "foo");
    ASSERT_EQ(nss.toStringWithTenantId_forTest(), tenantNsStr);
    ASSERT(nss.tenantId());
    ASSERT_EQ(*nss.tenantId(), tenantId);

    DatabaseName dbName = DatabaseName::createDatabaseName_forTest(tenantId, "foo");
    NamespaceString nss2 = makeNamespaceString(dbName, "");
    ASSERT(nss2.tenantId());
    ASSERT_EQ(*nss2.tenantId(), tenantId);

    NamespaceString nss3 = makeNamespaceString(tenantId, "foo", "");
    ASSERT(nss3.tenantId());
    ASSERT_EQ(*nss3.tenantId(), tenantId);
}

TEST_F(NamespaceStringTest, CompareNSSWithTenantId) {
    TenantId tenantIdMin(OID("000000000000000000000000"));
    TenantId tenantIdMax(OID::max());

    ASSERT(makeNamespaceString(tenantIdMin, "foo.bar") ==
           makeNamespaceString(tenantIdMin, "foo.bar"));

    ASSERT(makeNamespaceString(tenantIdMin, "foo.bar") !=
           makeNamespaceString(tenantIdMax, "foo.bar"));
    ASSERT(makeNamespaceString(tenantIdMin, "foo.bar") !=
           makeNamespaceString(tenantIdMin, "zoo.bar"));

    ASSERT(makeNamespaceString(tenantIdMin, "foo.bar") <
           makeNamespaceString(tenantIdMax, "foo.bar"));
    ASSERT(makeNamespaceString(tenantIdMin, "foo.bar") <
           makeNamespaceString(tenantIdMin, "zoo.bar"));
    ASSERT(makeNamespaceString(tenantIdMin, "zoo.bar") <
           makeNamespaceString(tenantIdMax, "foo.bar"));

    ASSERT(makeNamespaceString(tenantIdMax, "foo.bar") >
           makeNamespaceString(tenantIdMin, "foo.bar"));
    ASSERT(makeNamespaceString(tenantIdMin, "zoo.bar") >
           makeNamespaceString(tenantIdMin, "foo.bar"));
    ASSERT(makeNamespaceString(tenantIdMax, "foo.bar") >
           makeNamespaceString(tenantIdMin, "zoo.bar"));

    ASSERT(makeNamespaceString(tenantIdMin, "foo.bar") <=
           makeNamespaceString(tenantIdMin, "foo.bar"));
    ASSERT(makeNamespaceString(tenantIdMin, "foo.bar") >=
           makeNamespaceString(tenantIdMin, "foo.bar"));


    TenantId tenantId1(OID::gen());
    TenantId tenantId2(OID::gen());
    auto ns1 = makeNamespaceString(boost::none, "foo.bar");
    auto ns2 = makeNamespaceString(tenantId1, "foo.bar");
    auto ns3 = makeNamespaceString(tenantId2, "foo.bar");
    ASSERT_LT(ns1, ns2);
    ASSERT_LT(ns1, ns3);
    ASSERT_GT(ns3, ns2);
}

TEST_F(NamespaceStringTest, EmptyNamespaceString) {
    const auto emptyNss = NamespaceString();
    const auto kEmptyNss = NamespaceString::kEmpty;
    ASSERT_EQ(emptyNss, kEmptyNss);
    ASSERT_EQ(emptyNss.toStringForErrorMsg(), kEmptyNss.toStringForErrorMsg());
    ASSERT_EQ(emptyNss.dbName(), DatabaseName::kEmpty);
    ASSERT_EQ(emptyNss.coll().empty(), true);
    ASSERT_EQ(emptyNss.toStringForErrorMsg(), "");
}

TEST_F(NamespaceStringTest, NsFromDbOnly) {
    DatabaseName db = DatabaseName::createDatabaseName_forTest(boost::none, "testdb");
    NamespaceString ns(db);

    ASSERT_EQ(ns.size(), 6);
    ASSERT_EQ(ns, makeNamespaceString(boost::none, "testdb", ""));
}

TEST_F(NamespaceStringTest, ConstRefAssignmentOperator) {
    NamespaceString expected{makeNamespaceString("foo", "bar")};

    NamespaceString actual;
    actual = expected;

    ASSERT_EQ(actual, expected);
    ASSERT_EQ(actual.coll(), "bar");
    ASSERT_EQ(actual.sizeWithTenant(), 7);
    ASSERT_EQ(actual.size(), 7);
}

// Verify we can create a new NamespaceString with a DatabaseName created by ns.dbName(). We must
// ensure we discard the collection from `ns` and we don't end up with `db.collection.collection`.
TEST_F(NamespaceStringTest, NamespaceToDatabaseRoundtrip) {
    auto dbName = "test"_sd;
    auto collName = "foo"_sd;
    auto otherCollName = "othername"_sd;

    NamespaceString ns = makeNamespaceString(boost::none, dbName, collName);
    NamespaceString nsDbOnly =
        makeNamespaceString(DatabaseName::createDatabaseName_forTest(boost::none, dbName));
    NamespaceString nsWithOtherColl = makeNamespaceString(boost::none, dbName, otherCollName);

    auto runChecks = [&](const DatabaseName& db) {
        ASSERT_EQ(db.size(), 4);

        auto nsWithEmptyColl = makeNamespaceString(db);
        ASSERT_EQ(nsWithEmptyColl, nsDbOnly);
        ASSERT_EQ(nsWithEmptyColl.compare(nsDbOnly), 0);

        auto newNs = makeNamespaceString(db, collName);
        ASSERT_EQ(newNs.size(), 8);
        ASSERT_EQ(ns, newNs);
        ASSERT_EQ(ns.compare(newNs), 0);

        auto anotherColl = makeNamespaceString(db, otherCollName);
        ASSERT_EQ(anotherColl.size(), 14);
        ASSERT_EQ(anotherColl.coll(), otherCollName);
        ASSERT_EQ(anotherColl, nsWithOtherColl);
        ASSERT_EQ(anotherColl.compare(nsWithOtherColl), 0);
    };

    const DatabaseName& dbRef = ns.dbName();
    runChecks(dbRef);

    const DatabaseName dbVal(ns.dbName());
    runChecks(dbVal);
}

TEST_F(NamespaceStringTest, HashTest) {
    auto ns = NamespaceString::createNamespaceString_forTest(boost::none, "foo", "bar");
    auto ns2 = NamespaceString::createNamespaceString_forTest(boost::none, "foo", "bar");

    auto db = DatabaseName::createDatabaseName_forTest(boost::none, "foo");

    using NsHash = absl::Hash<NamespaceString>;
    using DbHash = absl::Hash<DatabaseName>;

    ASSERT_EQ(DbHash()(db), DbHash()(ns.dbName()));
    ASSERT_EQ(NsHash()(ns), NsHash()(ns2));
    ASSERT_EQ(NsHash()(ns2),
              NsHash()(NamespaceString::createNamespaceString_forTest(ns.dbName(), "bar")));

    ASSERT_EQ(NsHash()(NamespaceString(ns.dbName())),
              NsHash()(NamespaceString::createNamespaceString_forTest(boost::none, "foo", "")));
}

TEST_F(NamespaceStringTest, isDbOnly) {
    TenantId tenantId(OID::gen());

    // Namespaces that should succeed validation because of being db only.
    const std::vector<NamespaceString> dbOnlyNamespaces = {
        makeNamespaceString(boost::none, "foo"),
        makeNamespaceString(tenantId, "foo"),
        makeNamespaceString(boost::none, "foo."),  // ignores the period
        makeNamespaceString(tenantId, "foo."),     // ignores the period
        makeNamespaceString(boost::none, "."),     // ignores the period, empty db
        makeNamespaceString(tenantId, "."),        // ignores the period, `tenantId_`
        NamespaceString::createNamespaceString_forTest(
            DatabaseName::createDatabaseName_forTest(boost::none, "foo")),
        NamespaceString::createNamespaceString_forTest(
            DatabaseName::createDatabaseName_forTest(tenantId, "foo")),
        makeNamespaceString(DatabaseName::createDatabaseName_forTest(boost::none, "foo"), ""),
        makeNamespaceString(DatabaseName::createDatabaseName_forTest(tenantId, "foo"), ""),
        NamespaceString(),
        NamespaceString::kEmpty};

    // Namespaces that should fail validation because of the coll.
    const std::vector<NamespaceString> dbAndCollNamespaces = {
        makeNamespaceString(boost::none, "foo.a"),
        makeNamespaceString(tenantId, "foo.a"),
        makeNamespaceString(DatabaseName::createDatabaseName_forTest(boost::none, "foo"), "bar"),
        NamespaceString::createNamespaceString_forTest(
            DatabaseName::createDatabaseName_forTest(tenantId, "foo"), "bar")};

    for (const auto& nss : dbOnlyNamespaces) {
        ASSERT_TRUE(nss.isDbOnly());
    }

    for (const auto& nss : dbAndCollNamespaces) {
        ASSERT_FALSE(nss.isDbOnly());
    }
}

TEST_F(NamespaceStringTest, CheckFormatNamespaceEmptyColl) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", false);
    TenantId tenantId(OID::gen());
    DatabaseName dbName = DatabaseName::createDatabaseName_forTest(tenantId, "dbTest");
    auto nssInclColl = makeNamespaceString(dbName, "coll");
    ASSERT_EQ(nssInclColl.toString_forTest(), "dbTest.coll");

    auto nssEmptyColl = makeNamespaceString(dbName, "");
    ASSERT_EQ(nssEmptyColl.toString_forTest(), "dbTest");
}


TEST_F(NamespaceStringTest, CheckFormatNamespaceEmptyCollMultitenancy) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);
    TenantId tenantId(OID::gen());
    DatabaseName dbName = DatabaseName::createDatabaseName_forTest(tenantId, "dbTest");
    auto nssInclColl = makeNamespaceString(dbName, "coll");
    ASSERT_EQ(nssInclColl.toString_forTest(), "dbTest.coll");

    auto nssEmptyColl = makeNamespaceString(dbName, "");
    ASSERT_EQ(nssEmptyColl.toString_forTest(), "dbTest");
}


}  // namespace
}  // namespace mongo
