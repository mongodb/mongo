// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

/**
 * Unit tests of the UserName and RoleName types.
 */

// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/base/status.h"
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
#include <string_view>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

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
    const bool expectEmpty =
        std::string_view(name).empty() && std::string_view(db).empty() && !tenant;
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
    checkValueAssertions(T(std::string_view(), std::string_view()), "", "");
    checkValueAssertions(T(std::string(), std::string_view()), "", "");

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
        UserName(std::string("alice"), "db1"sv),
        UserName("bob"sv, std::string("db2")),
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
        RoleName(std::string("alice"), "db1"sv),
        RoleName("bob"sv, std::string("db2")),
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
