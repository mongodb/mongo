/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

/**
 * Unit tests of the UserName and RoleName types.
 */

// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/auth/auth_name.h"
#include "mongo/db/auth/role_name.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/tenant_id.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

namespace mongo {
namespace {

const std::string& getName(const UserName& obj) {
    return obj.getUser();
}

const std::string& getName(const RoleName& obj) {
    return obj.getRole();
}

template <typename Stream, typename T>
std::string stream(const T& obj) {
    Stream sb;
    sb << obj;
    return sb.str();
}

template <typename T, typename Name, typename Db>
void checkValueAssertions(const T& obj,
                          Name name,
                          Db db,
                          const boost::optional<TenantId>& tenant = boost::none) {
    const bool expectEmpty = StringData(name).empty() && StringData(db).empty() && !tenant;
    ASSERT_EQ(obj.empty(), expectEmpty);

    ASSERT_EQ(obj.getDB(), db);
    ASSERT_EQ(obj.getName(), name);
    ASSERT_EQ(getName(obj), name);
    ASSERT_EQ(obj.tenantId(), tenant);

    std::string expectDisplay, expectUnique;
    if (!expectEmpty) {
        expectDisplay = str::stream() << name << '@' << db;
        expectUnique = str::stream() << db << '.' << name;
    }
    ASSERT_EQ(obj.getDisplayName(), expectDisplay);
    ASSERT_EQ(stream<StringBuilder>(obj), expectDisplay);
    ASSERT_EQ(stream<std::ostringstream>(obj), expectDisplay);
    ASSERT_EQ(obj.getUnambiguousName(), expectUnique);

    T same(name, db, tenant);
    ASSERT_EQ(obj, same);

    T bigger("zzzz", "zzzz", tenant);
    ASSERT_LT(obj, bigger);
}

template <typename T>
void doConstructorTest() {
    checkValueAssertions(T(), "", "");

    checkValueAssertions(T("", ""), "", "");
    checkValueAssertions(T(std::string(), std::string()), "", "");
    checkValueAssertions(T(StringData(), StringData()), "", "");
    checkValueAssertions(T(std::string(), StringData()), "", "");

    checkValueAssertions(T("name1", "db1"), "name1", "db1");
    checkValueAssertions(T("name1", ""), "name1", "");
    checkValueAssertions(T("", "db1"), "", "db1");
}

TEST(AuthName, ConstructorTest) {
    doConstructorTest<UserName>();
    doConstructorTest<RoleName>();
}

template <typename T, typename Name, typename Db>
void doBSONParseTest(Name name, Db db) {
    // Without TenantId.
    auto obj = BSON(T::kFieldName << name << "db" << db);
    checkValueAssertions(T::parseFromBSON(BSON("" << obj).firstElement()), name, db);
    checkValueAssertions(T::parseFromBSONObj(obj), name, db);

    // With TenantId.
    auto oid = OID::gen();
    auto tenant = TenantId(oid);
    auto tobj = BSON(T::kFieldName << name << "db" << db << "tenant" << oid);
    checkValueAssertions(T::parseFromBSON(BSON("" << tobj).firstElement()), name, db, tenant);
    checkValueAssertions(T::parseFromBSONObj(tobj), name, db, tenant);
}

template <typename T, typename Name, typename Db>
void doBSONParseFailure(Name name, Db db) {
    auto obj = BSON(T::kFieldName << name << "db" << db);
    ASSERT_THROWS(T::parseFromBSON(BSON("" << obj).firstElement()), AssertionException);

    ASSERT_THROWS(T::parseFromBSONObj(obj), AssertionException);
}

template <typename T>
void doBSONParseTests() {
    doBSONParseTest<T>("", "");
    doBSONParseTest<T>("name", "");
    doBSONParseTest<T>("", "db");
    doBSONParseTest<T>("name", "db");

    doBSONParseFailure<T>(123, "db");
    doBSONParseFailure<T>("name", 123);
    doBSONParseFailure<T>(OID(), "db");
}

TEST(AuthName, BSONParseTests) {
    doBSONParseTests<UserName>();
    doBSONParseTests<RoleName>();
}

template <typename T>
void doStringParseTests() {
    checkValueAssertions(uassertStatusOK(T::parse("db.name")), "name", "db");
    checkValueAssertions(uassertStatusOK(T::parse("db.")), "", "db");
    checkValueAssertions(uassertStatusOK(T::parse(".name")), "name", "");
    checkValueAssertions(uassertStatusOK(T::parse("db.name.str")), "name.str", "db");
    checkValueAssertions(uassertStatusOK(T::parse(".")), "", "");

    ASSERT_NOT_OK(T::parse(""));
}

TEST(AuthName, StringParseTests) {
    doStringParseTests<UserName>();
    doStringParseTests<RoleName>();
}

// Tests explicitly using UserName/RoleName specializations directly with iteration.

TEST(AuthName, UserName) {
    const std::vector<UserName> userNames = {
        UserName(std::string("alice"), "db1"_sd),
        UserName("bob"_sd, std::string("db2")),
        uassertStatusOK(UserName::parse("db3.claire")),
    };

    auto it = makeUserNameIteratorForContainer(userNames);

    ASSERT_EQ(it.more(), true);
    auto first = it.next();
    ASSERT_EQ(first.getDisplayName(), "alice@db1");
    ASSERT_EQ(first.getUnambiguousName(), "db1.alice");
    ASSERT_EQ(first.getUser(), "alice");
    ASSERT_EQ(first.getDB(), "db1");
    ASSERT(first == userNames[0]);

    ASSERT_EQ(it.more(), true);
    auto second = it.next();
    ASSERT_EQ(second.getDisplayName(), "bob@db2");
    ASSERT_EQ(second.getUnambiguousName(), "db2.bob");
    ASSERT_EQ(second.getUser(), "bob");
    ASSERT_EQ(second.getDB(), "db2");
    ASSERT(second == userNames[1]);

    ASSERT_EQ(it.more(), true);
    auto third = it.next();
    ASSERT_EQ(third.getDisplayName(), "claire@db3");
    ASSERT_EQ(third.getUnambiguousName(), "db3.claire");
    ASSERT_EQ(third.getUser(), "claire");
    ASSERT_EQ(third.getDB(), "db3");
    ASSERT(third == userNames[2]);

    ASSERT(first != second);
    ASSERT(second != third);
    ASSERT(third != first);

    ASSERT_EQ(it.more(), false);
}

TEST(AuthName, RoleName) {
    const std::vector<RoleName> roleNames = {
        RoleName(std::string("alice"), "db1"_sd),
        RoleName("bob"_sd, std::string("db2")),
        uassertStatusOK(RoleName::parse("db3.claire")),
    };

    auto it = makeRoleNameIteratorForContainer(roleNames);

    ASSERT_EQ(it.more(), true);
    auto first = it.next();
    ASSERT_EQ(first.getDisplayName(), "alice@db1");
    ASSERT_EQ(first.getUnambiguousName(), "db1.alice");
    ASSERT_EQ(first.getRole(), "alice");
    ASSERT_EQ(first.getDB(), "db1");
    ASSERT(first == roleNames[0]);

    ASSERT_EQ(it.more(), true);
    auto second = it.next();
    ASSERT_EQ(second.getDisplayName(), "bob@db2");
    ASSERT_EQ(second.getUnambiguousName(), "db2.bob");
    ASSERT_EQ(second.getRole(), "bob");
    ASSERT_EQ(second.getDB(), "db2");
    ASSERT(second == roleNames[1]);

    ASSERT_EQ(it.more(), true);
    auto third = it.next();
    ASSERT_EQ(third.getDisplayName(), "claire@db3");
    ASSERT_EQ(third.getUnambiguousName(), "db3.claire");
    ASSERT_EQ(third.getRole(), "claire");
    ASSERT_EQ(third.getDB(), "db3");
    ASSERT(third == roleNames[2]);

    ASSERT(first != second);
    ASSERT(second != third);
    ASSERT(third != first);

    ASSERT_EQ(it.more(), false);
}

}  // namespace
}  // namespace mongo
