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

#include <boost/optional.hpp>

#include "mongo/platform/basic.h"

#include "mongo/db/multitenancy_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/optime.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/namespace_string_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {
namespace {

using namespace fmt::literals;

TEST(NamespaceStringTest, CheckNamespaceStringLogAttrs) {
    TenantId tenantId(OID::gen());
    DatabaseName dbName = DatabaseName::createDatabaseName_forTest(tenantId, "foo");
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(dbName, "bar");

    startCapturingLogMessages();
    LOGV2(7311500, "Msg nss:", logAttrs(nss));

    std::string nssAsString = str::stream() << *(nss.tenantId()) << '_' << nss.ns();

    ASSERT_EQUALS(
        1, countBSONFormatLogLinesIsSubset(BSON("attr" << BSON("namespace" << nssAsString))));
    stopCapturingLogMessages();
}

TEST(NamespaceStringTest, Oplog) {
    ASSERT(!NamespaceString::oplog("a"));
    ASSERT(!NamespaceString::oplog("a.b"));

    ASSERT(NamespaceString::oplog("local.oplog.rs"));
    ASSERT(NamespaceString::oplog("local.oplog.foo"));
    ASSERT(NamespaceString::oplog("local.oplog.$main"));
    ASSERT(NamespaceString::oplog("local.oplog.$foo"));
}

TEST(NamespaceStringTest, DatabaseValidNames) {
    ASSERT(NamespaceString::validDBName("foo", NamespaceString::DollarInDbNameBehavior::Allow));
    ASSERT(NamespaceString::validDBName("foo$bar", NamespaceString::DollarInDbNameBehavior::Allow));
    ASSERT(
        !NamespaceString::validDBName("foo/bar", NamespaceString::DollarInDbNameBehavior::Allow));
    ASSERT(
        !NamespaceString::validDBName("foo bar", NamespaceString::DollarInDbNameBehavior::Allow));
    ASSERT(
        !NamespaceString::validDBName("foo.bar", NamespaceString::DollarInDbNameBehavior::Allow));
    ASSERT(
        !NamespaceString::validDBName("foo\\bar", NamespaceString::DollarInDbNameBehavior::Allow));
    ASSERT(
        !NamespaceString::validDBName("foo\"bar", NamespaceString::DollarInDbNameBehavior::Allow));
    ASSERT(
        !NamespaceString::validDBName("a\0b"_sd, NamespaceString::DollarInDbNameBehavior::Allow));
#ifdef _WIN32
    ASSERT(
        !NamespaceString::validDBName("foo*bar", NamespaceString::DollarInDbNameBehavior::Allow));
    ASSERT(
        !NamespaceString::validDBName("foo<bar", NamespaceString::DollarInDbNameBehavior::Allow));
    ASSERT(
        !NamespaceString::validDBName("foo>bar", NamespaceString::DollarInDbNameBehavior::Allow));
    ASSERT(
        !NamespaceString::validDBName("foo:bar", NamespaceString::DollarInDbNameBehavior::Allow));
    ASSERT(
        !NamespaceString::validDBName("foo|bar", NamespaceString::DollarInDbNameBehavior::Allow));
    ASSERT(
        !NamespaceString::validDBName("foo?bar", NamespaceString::DollarInDbNameBehavior::Allow));
#endif

    ASSERT(NamespaceString::validDBName("foo"));
    ASSERT(!NamespaceString::validDBName("foo$bar"));
    ASSERT(!NamespaceString::validDBName("foo/bar"));
    ASSERT(!NamespaceString::validDBName("foo bar"));
    ASSERT(!NamespaceString::validDBName("foo.bar"));
    ASSERT(!NamespaceString::validDBName("foo\\bar"));
    ASSERT(!NamespaceString::validDBName("foo\"bar"));
    ASSERT(!NamespaceString::validDBName("a\0b"_sd));
#ifdef _WIN32
    ASSERT(!NamespaceString::validDBName("foo*bar"));
    ASSERT(!NamespaceString::validDBName("foo<bar"));
    ASSERT(!NamespaceString::validDBName("foo>bar"));
    ASSERT(!NamespaceString::validDBName("foo:bar"));
    ASSERT(!NamespaceString::validDBName("foo|bar"));
    ASSERT(!NamespaceString::validDBName("foo?bar"));
#endif

    ASSERT(NamespaceString::validDBName(
        "ThisIsADatabaseNameThatBrokeAllRecordsForValidLengthForDBName63"));
    ASSERT(!NamespaceString::validDBName(
        "WhileThisDatabaseNameExceedsTheMaximumLengthForDatabaseNamesof63"));
}

TEST(NamespaceStringTest, ListCollectionsCursorNS) {
    ASSERT(NamespaceString("test.$cmd.listCollections").isListCollectionsCursorNS());

    ASSERT(!NamespaceString("test.foo").isListCollectionsCursorNS());
    ASSERT(!NamespaceString("test.foo.$cmd.listCollections").isListCollectionsCursorNS());
    ASSERT(!NamespaceString("test.$cmd.").isListCollectionsCursorNS());
    ASSERT(!NamespaceString("test.$cmd.foo.").isListCollectionsCursorNS());
    ASSERT(!NamespaceString("test.$cmd.listCollections.").isListCollectionsCursorNS());
    ASSERT(!NamespaceString("test.$cmd.listIndexes").isListCollectionsCursorNS());
    ASSERT(!NamespaceString("test.$cmd.listIndexes.foo").isListCollectionsCursorNS());
}

TEST(NamespaceStringTest, IsCollectionlessCursorNamespace) {
    ASSERT_TRUE(NamespaceString{"test.$cmd.aggregate.foo"}.isCollectionlessCursorNamespace());
    ASSERT_TRUE(NamespaceString{"test.$cmd.listIndexes.foo"}.isCollectionlessCursorNamespace());
    ASSERT_TRUE(NamespaceString{"test.$cmd.otherCommand.foo"}.isCollectionlessCursorNamespace());
    ASSERT_TRUE(NamespaceString{"test.$cmd.listCollections"}.isCollectionlessCursorNamespace());
    ASSERT_TRUE(NamespaceString{"test.$cmd.otherCommand"}.isCollectionlessCursorNamespace());
    ASSERT_TRUE(NamespaceString{"test.$cmd.aggregate"}.isCollectionlessCursorNamespace());
    ASSERT_TRUE(NamespaceString{"test.$cmd.listIndexes"}.isCollectionlessCursorNamespace());

    ASSERT_FALSE(NamespaceString{"test.foo"}.isCollectionlessCursorNamespace());
    ASSERT_FALSE(NamespaceString{"test.$cmd"}.isCollectionlessCursorNamespace());

    ASSERT_FALSE(NamespaceString{"$cmd.aggregate.foo"}.isCollectionlessCursorNamespace());
    ASSERT_FALSE(NamespaceString{"$cmd.listCollections"}.isCollectionlessCursorNamespace());
}

TEST(NamespaceStringTest, IsLegalClientSystemNamespace) {
    const auto& currentFCV = serverGlobalParams.featureCompatibility;
    ASSERT_TRUE(NamespaceString{"test.system.buckets.1234"}.isLegalClientSystemNS(currentFCV));
    ASSERT_TRUE(NamespaceString{"test.system.buckets.abcde"}.isLegalClientSystemNS(currentFCV));
    ASSERT_FALSE(NamespaceString{"test.system.buckets..1234"}.isLegalClientSystemNS(currentFCV));
    ASSERT_FALSE(NamespaceString{"test.system.buckets.a234$"}.isLegalClientSystemNS(currentFCV));
    ASSERT_FALSE(NamespaceString{"test.system.buckets."}.isLegalClientSystemNS(currentFCV));
}

TEST(NamespaceStringTest, IsDropPendingNamespace) {
    ASSERT_TRUE(NamespaceString{"test.system.drop.0i0t-1.foo"}.isDropPendingNamespace());
    ASSERT_TRUE(NamespaceString{"test.system.drop.1234567i8t9.foo"}.isDropPendingNamespace());
    ASSERT_TRUE(NamespaceString{"test.system.drop.1234.foo"}.isDropPendingNamespace());
    ASSERT_TRUE(NamespaceString{"test.system.drop.foo"}.isDropPendingNamespace());

    ASSERT_FALSE(NamespaceString{"test.system.drop"}.isDropPendingNamespace());
    ASSERT_FALSE(NamespaceString{"test.drop.1234.foo"}.isDropPendingNamespace());
    ASSERT_FALSE(NamespaceString{"test.drop.foo"}.isDropPendingNamespace());
    ASSERT_FALSE(NamespaceString{"test.foo"}.isDropPendingNamespace());
    ASSERT_FALSE(NamespaceString{"test.$cmd"}.isDropPendingNamespace());

    ASSERT_FALSE(NamespaceString{"$cmd.aggregate.foo"}.isDropPendingNamespace());
    ASSERT_FALSE(NamespaceString{"$cmd.listCollections"}.isDropPendingNamespace());
}

TEST(NamespaceStringTest, MakeDropPendingNamespace) {
    ASSERT_EQUALS(NamespaceString{"test.system.drop.0i0t-1.foo"},
                  NamespaceString{"test.foo"}.makeDropPendingNamespace(repl::OpTime()));
    ASSERT_EQUALS(NamespaceString{"test.system.drop.1234567i8t9.foo"},
                  NamespaceString{"test.foo"}.makeDropPendingNamespace(
                      repl::OpTime(Timestamp(Seconds(1234567), 8U), 9LL)));

    std::string collName(NamespaceString::MaxNsCollectionLen, 't');
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", collName);
    ASSERT_EQUALS(NamespaceString{"test.system.drop.1234567i8t9." + collName},
                  nss.makeDropPendingNamespace(repl::OpTime(Timestamp(Seconds(1234567), 8U), 9LL)));
}

TEST(NamespaceStringTest, GetDropPendingNamespaceOpTime) {
    // Null optime is acceptable.
    ASSERT_EQUALS(
        repl::OpTime(),
        unittest::assertGet(
            NamespaceString{"test.system.drop.0i0t-1.foo"}.getDropPendingNamespaceOpTime()));

    // Valid optime.
    ASSERT_EQUALS(
        repl::OpTime(Timestamp(Seconds(1234567), 8U), 9LL),
        unittest::assertGet(
            NamespaceString{"test.system.drop.1234567i8t9.foo"}.getDropPendingNamespaceOpTime()));

    // Original collection name is optional.
    ASSERT_EQUALS(
        repl::OpTime(Timestamp(Seconds(1234567), 8U), 9LL),
        unittest::assertGet(
            NamespaceString{"test.system.drop.1234567i8t9"}.getDropPendingNamespaceOpTime()));

    // No system.drop. prefix.
    ASSERT_EQUALS(ErrorCodes::BadValue,
                  NamespaceString{"test.1234.foo"}.getDropPendingNamespaceOpTime());

    // Missing 'i' separator.
    ASSERT_EQUALS(ErrorCodes::FailedToParse,
                  NamespaceString{"test.system.drop.1234t8.foo"}.getDropPendingNamespaceOpTime());

    // Missing 't' separator.
    ASSERT_EQUALS(ErrorCodes::FailedToParse,
                  NamespaceString{"test.system.drop.1234i56.foo"}.getDropPendingNamespaceOpTime());

    // Timestamp seconds is not a number.
    ASSERT_EQUALS(
        ErrorCodes::FailedToParse,
        NamespaceString{"test.system.drop.wwwi56t123.foo"}.getDropPendingNamespaceOpTime());

    // Timestamp increment is not a number.
    ASSERT_EQUALS(
        ErrorCodes::FailedToParse,
        NamespaceString{"test.system.drop.1234iaaat123.foo"}.getDropPendingNamespaceOpTime());

    // Timestamp increment must be an unsigned number.
    ASSERT_EQUALS(
        ErrorCodes::FailedToParse,
        NamespaceString{"test.system.drop.1234i-100t123.foo"}.getDropPendingNamespaceOpTime());

    // Term is not a number.
    ASSERT_EQUALS(
        ErrorCodes::FailedToParse,
        NamespaceString{"test.system.drop.1234i111taaa.foo"}.getDropPendingNamespaceOpTime());
}

TEST(NamespaceStringTest, CollectionComponentValidNames) {
    ASSERT(NamespaceString::validCollectionComponent("a.b"));
    ASSERT(NamespaceString::validCollectionComponent("a.b"));
    ASSERT(!NamespaceString::validCollectionComponent("a."));
    ASSERT(!NamespaceString::validCollectionComponent("a..foo"));
    ASSERT(NamespaceString::validCollectionComponent("a.b."));  // TODO: should this change?
}

TEST(NamespaceStringTest, CollectionValidNames) {
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

TEST(NamespaceStringTest, nsToDatabase1) {
    ASSERT_EQUALS("foo", nsToDatabaseSubstring("foo.bar"));
    ASSERT_EQUALS("foo", nsToDatabaseSubstring("foo"));
    ASSERT_EQUALS("foo", nsToDatabase("foo.bar"));
    ASSERT_EQUALS("foo", nsToDatabase("foo"));
    ASSERT_EQUALS("foo", nsToDatabase(std::string("foo.bar")));
    ASSERT_EQUALS("foo", nsToDatabase(std::string("foo")));
}

TEST(NamespaceStringTest, NamespaceStringParse1) {
    NamespaceString ns = NamespaceString::createNamespaceString_forTest("a.b");
    ASSERT_EQUALS(std::string("a"), ns.db());
    ASSERT_EQUALS(std::string("b"), ns.coll());
}

TEST(NamespaceStringTest, NamespaceStringParse2) {
    NamespaceString ns = NamespaceString::createNamespaceString_forTest("a.b.c");
    ASSERT_EQUALS(std::string("a"), ns.db());
    ASSERT_EQUALS(std::string("b.c"), ns.coll());
}

TEST(NamespaceStringTest, NamespaceStringParse3) {
    NamespaceString ns = NamespaceString::createNamespaceString_forTest("abc");
    ASSERT_EQUALS(std::string("abc"), ns.db());
    ASSERT_EQUALS(std::string(""), ns.coll());
}

TEST(NamespaceStringTest, NamespaceStringParse4) {
    NamespaceString ns = NamespaceString::createNamespaceString_forTest("abc.");
    ASSERT_EQUALS(std::string("abc"), ns.db());
    ASSERT(ns.coll().empty());
}

TEST(NamespaceStringTest, NamespaceStringParse5) {
    NamespaceString ns = NamespaceString::createNamespaceString_forTest("abc", "");
    ASSERT_EQUALS(std::string("abc"), ns.db());
    ASSERT(ns.coll().empty());
}

TEST(NamespaceStringTest, makeListCollectionsNSIsCorrect) {
    NamespaceString ns = NamespaceString::makeListCollectionsNSS(
        DatabaseName::createDatabaseName_forTest(boost::none, "DB"));
    ASSERT_EQUALS("DB", ns.db());
    ASSERT_EQUALS("$cmd.listCollections", ns.coll());
    ASSERT(ns.isValid());
    ASSERT(ns.isListCollectionsCursorNS());
}

TEST(NamespaceStringTest, EmptyNSStringReturnsEmptyColl) {
    NamespaceString nss{};
    ASSERT_TRUE(nss.isEmpty());
    ASSERT_EQ(nss.coll(), StringData{});
}

TEST(NamespaceStringTest, EmptyNSStringReturnsEmptyDb) {
    NamespaceString nss{};
    ASSERT_TRUE(nss.isEmpty());
    ASSERT_EQ(nss.db(), StringData{});
}

TEST(NamespaceStringTest, EmptyDbWithColl) {
    NamespaceString nss{"", "coll"};
    ASSERT_EQ(nss.db(), StringData{});
    ASSERT_EQ(nss.coll(), "coll");
}

TEST(NamespaceStringTest, NSSWithTenantId) {
    TenantId tenantId(OID::gen());

    {
        std::string tenantNsStr = str::stream() << tenantId.toString() << "_foo.bar";
        NamespaceString nss = NamespaceString::createNamespaceString_forTest(tenantId, "foo.bar");
        ASSERT_EQ(nss.size(), 7);
        ASSERT_EQ(nss.ns(), "foo.bar");
        ASSERT_EQ(nss.toString(), "foo.bar");
        ASSERT_EQ(nss.toStringWithTenantId(), tenantNsStr);
        ASSERT_EQ(nss.db(), "foo");
        ASSERT_EQ(nss.coll(), "bar");
        ASSERT_EQ(nss.dbName().db(), "foo");
        ASSERT_EQ(nss.size(), 7);
        ASSERT(nss.tenantId());
        ASSERT(nss.dbName().tenantId());
        ASSERT_EQ(*nss.tenantId(), tenantId);
        ASSERT_EQ(*nss.dbName().tenantId(), tenantId);
    }

    {
        std::string tenantNsStr = str::stream() << tenantId.toString() << "_foo";
        NamespaceString nss = NamespaceString::createNamespaceString_forTest(tenantId, "foo");
        ASSERT_EQ(nss.size(), 3);
        ASSERT_EQ(nss.ns(), "foo");
        ASSERT_EQ(nss.toString(), "foo");
        ASSERT_EQ(nss.toStringWithTenantId(), tenantNsStr);
        ASSERT_EQ(nss.db(), "foo");
        ASSERT_EQ(nss.coll(), "");
        ASSERT_EQ(nss.dbName().db(), "foo");
        ASSERT_EQ(nss.size(), 3);
        ASSERT(nss.tenantId());
        ASSERT(nss.dbName().tenantId());
        ASSERT_EQ(*nss.tenantId(), tenantId);
        ASSERT_EQ(*nss.dbName().tenantId(), tenantId);
    }

    {
        std::string tenantNsStr = str::stream() << tenantId.toString() << "_foo.bar";
        DatabaseName dbName = DatabaseName::createDatabaseName_forTest(tenantId, "foo");
        NamespaceString nss2 = NamespaceString::createNamespaceString_forTest(dbName, "bar");
        ASSERT_EQ(nss2.size(), 7);
        ASSERT_EQ(nss2.ns(), "foo.bar");
        ASSERT_EQ(nss2.toString(), "foo.bar");
        ASSERT_EQ(nss2.toStringWithTenantId(), tenantNsStr);
        ASSERT_EQ(nss2.db(), "foo");
        ASSERT_EQ(nss2.coll(), "bar");
        ASSERT_EQ(nss2.dbName().db(), "foo");
        ASSERT(nss2.tenantId());
        ASSERT(nss2.dbName().tenantId());
        ASSERT_EQ(*nss2.tenantId(), tenantId);
        ASSERT_EQ(*nss2.dbName().tenantId(), tenantId);
    }

    {
        std::string tenantNsStr = str::stream() << tenantId.toString() << "_foo.bar";
        NamespaceString nss3("foo", "bar", tenantId);
        ASSERT_EQ(nss3.size(), 7);
        ASSERT_EQ(nss3.ns(), "foo.bar");
        ASSERT_EQ(nss3.toString(), "foo.bar");
        ASSERT_EQ(nss3.toStringWithTenantId(), tenantNsStr);
        ASSERT_EQ(nss3.db(), "foo");
        ASSERT_EQ(nss3.coll(), "bar");
        ASSERT_EQ(nss3.dbName().db(), "foo");
        ASSERT(nss3.tenantId());
        ASSERT(nss3.dbName().tenantId());
        ASSERT_EQ(*nss3.tenantId(), tenantId);
        ASSERT_EQ(*nss3.dbName().tenantId(), tenantId);
    }

    {
        DatabaseName dbName = DatabaseName::createDatabaseName_forTest(tenantId, "foo");
        NamespaceString nss4(dbName);
        ASSERT_EQ(nss4.size(), 3);
        ASSERT_EQ(nss4.ns(), "foo");
        ASSERT_EQ(nss4.toString(), "foo");
        ASSERT_EQ(nss4.toStringWithTenantId(), "{}_foo"_format(tenantId.toString()));
        ASSERT_EQ(nss4.db(), "foo");
        ASSERT_EQ(nss4.coll(), "");
        ASSERT_EQ(nss4.dbName().db(), "foo");
        ASSERT(nss4.tenantId());
        ASSERT(nss4.dbName().tenantId());
        ASSERT_EQ(*nss4.tenantId(), tenantId);
        ASSERT_EQ(*nss4.dbName().tenantId(), tenantId);
    }

    {
        NamespaceString multiNss("config.system.change_collection", tenantId);
        ASSERT(multiNss.isConfigDB());
        ASSERT_EQ(multiNss.size(), 31);
        ASSERT_EQ(multiNss.ns(), "config.system.change_collection");
        ASSERT_EQ(multiNss.toString(), "config.system.change_collection");
        ASSERT_EQ(multiNss.toStringWithTenantId(),
                  "{}_config.system.change_collection"_format(tenantId.toString()));
        ASSERT_EQ(multiNss.db(), "config");
        ASSERT_EQ(multiNss.coll(), "system.change_collection");
        ASSERT_EQ(multiNss.dbName().db(), "config");
        ASSERT(multiNss.tenantId());
        ASSERT(multiNss.dbName().tenantId());
        ASSERT_EQ(*multiNss.tenantId(), tenantId);
        ASSERT_EQ(*multiNss.dbName().tenantId(), tenantId);
    }

    {
        NamespaceString empty{};
        ASSERT_EQ(empty.size(), 0);
        ASSERT_EQ(empty.db(), "");
        ASSERT_EQ(empty.coll(), "");
        ASSERT_EQ(empty.tenantId(), boost::none);
        ASSERT_EQ(empty.toString(), "");
        ASSERT_EQ(empty.toStringWithTenantId(), "");
        ASSERT_EQ(empty.dbName().db(), "");
        ASSERT_EQ(empty.dbName().tenantId(), boost::none);
        ASSERT_EQ(empty.dbName().toString(), "");
        ASSERT_EQ(empty.dbName().toStringWithTenantId(), "");
    }

    {
        NamespaceString emptyWithTenant{"", tenantId};
        ASSERT_EQ(emptyWithTenant.size(), 0);
        ASSERT_EQ(emptyWithTenant.db(), "");
        ASSERT_EQ(emptyWithTenant.coll(), "");
        ASSERT(emptyWithTenant.tenantId());
        ASSERT_EQ(*emptyWithTenant.tenantId(), tenantId);
        ASSERT_EQ(emptyWithTenant.toString(), "");
        ASSERT_EQ(emptyWithTenant.toStringWithTenantId(), "{}_"_format(tenantId.toString()));
        ASSERT_EQ(emptyWithTenant.dbName().db(), "");
        ASSERT(emptyWithTenant.dbName().tenantId());
        ASSERT_EQ(emptyWithTenant.dbName().tenantId(), tenantId);
        ASSERT_EQ(emptyWithTenant.dbName().toString(), "");
        ASSERT_EQ(emptyWithTenant.dbName().toStringWithTenantId(),
                  "{}_"_format(tenantId.toString()));
    }

    {
        NamespaceString dbWithoutColl{"foo"};
        ASSERT_EQ(dbWithoutColl.size(), 3);
        ASSERT_EQ(dbWithoutColl.db(), "foo");
        ASSERT_EQ(dbWithoutColl.coll(), "");
        ASSERT_FALSE(dbWithoutColl.tenantId());
        ASSERT_EQ(dbWithoutColl.toString(), "foo");
        ASSERT_EQ(dbWithoutColl.toStringWithTenantId(), "foo");
        ASSERT_EQ(dbWithoutColl.dbName().db(), "foo");
        ASSERT_FALSE(dbWithoutColl.dbName().tenantId());
        ASSERT_EQ(dbWithoutColl.dbName().toString(), "foo");
        ASSERT_EQ(dbWithoutColl.dbName().toStringWithTenantId(), "foo");
    }

    {
        NamespaceString dbWithoutCollWithTenant{"foo", tenantId};
        ASSERT_EQ(dbWithoutCollWithTenant.size(), 3);
        ASSERT_EQ(dbWithoutCollWithTenant.db(), "foo");
        ASSERT_EQ(dbWithoutCollWithTenant.coll(), "");
        ASSERT(dbWithoutCollWithTenant.tenantId());
        ASSERT_EQ(*dbWithoutCollWithTenant.tenantId(), tenantId);
        ASSERT_EQ(dbWithoutCollWithTenant.toString(), "foo");
        ASSERT_EQ(dbWithoutCollWithTenant.toStringWithTenantId(),
                  fmt::format("{}_foo", tenantId.toString()));
        ASSERT_EQ(dbWithoutCollWithTenant.dbName().db(), "foo");
        ASSERT(dbWithoutCollWithTenant.dbName().tenantId());
        ASSERT_EQ(dbWithoutCollWithTenant.dbName().tenantId(), tenantId);
        ASSERT_EQ(dbWithoutCollWithTenant.dbName().toString(), "foo");
        ASSERT_EQ(dbWithoutCollWithTenant.dbName().toStringWithTenantId(),
                  fmt::format("{}_foo", tenantId.toString()));
    }
}

TEST(NamespaceStringTest, NSSNoCollectionWithTenantId) {
    TenantId tenantId(OID::gen());
    std::string tenantNsStr = str::stream() << tenantId.toString() << "_foo";

    NamespaceString nss = NamespaceString::createNamespaceString_forTest(tenantId, "foo");
    ASSERT_EQ(nss.ns(), "foo");
    ASSERT_EQ(nss.toString(), "foo");
    ASSERT_EQ(nss.toStringWithTenantId(), tenantNsStr);
    ASSERT(nss.tenantId());
    ASSERT_EQ(*nss.tenantId(), tenantId);

    DatabaseName dbName = DatabaseName::createDatabaseName_forTest(tenantId, "foo");
    NamespaceString nss2 = NamespaceString::createNamespaceString_forTest(dbName, "");
    ASSERT(nss2.tenantId());
    ASSERT_EQ(*nss2.tenantId(), tenantId);

    NamespaceString nss3("foo", "", tenantId);
    ASSERT(nss3.tenantId());
    ASSERT_EQ(*nss3.tenantId(), tenantId);
}

TEST(NamespaceStringTest, ParseNSSWithTenantId) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);

    TenantId tenantId(OID::gen());
    std::string tenantNsStr = str::stream() << tenantId.toString() << "_foo.bar";

    NamespaceString nss =
        NamespaceString::parseFromStringExpectTenantIdInMultitenancyMode(tenantNsStr);
    ASSERT_EQ(nss.ns(), "foo.bar");
    ASSERT_EQ(nss.toStringWithTenantId(), tenantNsStr);
    ASSERT(nss.tenantId());
    ASSERT_EQ(*nss.tenantId(), tenantId);
}

TEST(NamespaceStringTest, CompareNSSWithTenantId) {
    TenantId tenantIdMin(OID("000000000000000000000000"));
    TenantId tenantIdMax(OID::max());

    ASSERT(NamespaceString::createNamespaceString_forTest(tenantIdMin, "foo.bar") ==
           NamespaceString::createNamespaceString_forTest(tenantIdMin, "foo.bar"));

    ASSERT(NamespaceString::createNamespaceString_forTest(tenantIdMin, "foo.bar") !=
           NamespaceString::createNamespaceString_forTest(tenantIdMax, "foo.bar"));
    ASSERT(NamespaceString::createNamespaceString_forTest(tenantIdMin, "foo.bar") !=
           NamespaceString::createNamespaceString_forTest(tenantIdMin, "zoo.bar"));

    ASSERT(NamespaceString::createNamespaceString_forTest(tenantIdMin, "foo.bar") <
           NamespaceString::createNamespaceString_forTest(tenantIdMax, "foo.bar"));
    ASSERT(NamespaceString::createNamespaceString_forTest(tenantIdMin, "foo.bar") <
           NamespaceString::createNamespaceString_forTest(tenantIdMin, "zoo.bar"));
    ASSERT(NamespaceString::createNamespaceString_forTest(tenantIdMin, "zoo.bar") <
           NamespaceString::createNamespaceString_forTest(tenantIdMax, "foo.bar"));

    ASSERT(NamespaceString::createNamespaceString_forTest(tenantIdMax, "foo.bar") >
           NamespaceString::createNamespaceString_forTest(tenantIdMin, "foo.bar"));
    ASSERT(NamespaceString::createNamespaceString_forTest(tenantIdMin, "zoo.bar") >
           NamespaceString::createNamespaceString_forTest(tenantIdMin, "foo.bar"));
    ASSERT(NamespaceString::createNamespaceString_forTest(tenantIdMax, "foo.bar") >
           NamespaceString::createNamespaceString_forTest(tenantIdMin, "zoo.bar"));

    ASSERT(NamespaceString::createNamespaceString_forTest(tenantIdMin, "foo.bar") <=
           NamespaceString::createNamespaceString_forTest(tenantIdMin, "foo.bar"));
    ASSERT(NamespaceString::createNamespaceString_forTest(tenantIdMin, "foo.bar") >=
           NamespaceString::createNamespaceString_forTest(tenantIdMin, "foo.bar"));


    TenantId tenantId1(OID::gen());
    TenantId tenantId2(OID::gen());
    auto ns1 = NamespaceString::createNamespaceString_forTest(boost::none, "foo.bar");
    auto ns2 = NamespaceString::createNamespaceString_forTest(tenantId1, "foo.bar");
    auto ns3 = NamespaceString::createNamespaceString_forTest(tenantId2, "foo.bar");
    ASSERT_LT(ns1, ns2);
    ASSERT_LT(ns1, ns3);
    ASSERT_GT(ns3, ns2);
}

}  // namespace
}  // namespace mongo
