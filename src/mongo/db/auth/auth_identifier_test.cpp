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

#include "mongo/platform/basic.h"

#include <string>

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/role_name.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

template <typename T>
struct Traits {};

template <>
struct Traits<UserName> {
    static constexpr auto kFieldName = "user"_sd;
    static const std::string& getName(const UserName& obj) {
        return obj.getUser();
    }
};

template <>
struct Traits<RoleName> {
    static constexpr auto kFieldName = "role"_sd;
    static const std::string& getName(const RoleName& obj) {
        return obj.getRole();
    }
};

template <typename Stream, typename T>
std::string stream(const T& obj) {
    Stream sb;
    sb << obj;
    return sb.str();
}

template <typename T, typename Name, typename Db>
void checkValueAssertions(const T& obj, Name name, Db db) {
    const bool expectEmpty = StringData(name).empty() && StringData(db).empty();
    ASSERT_EQ(obj.empty(), expectEmpty);

    ASSERT_EQ(obj.getDB(), db);
    ASSERT_EQ(Traits<T>::getName(obj), name);

    std::string expectDisplay, expectUnique;
    if (!expectEmpty) {
        expectDisplay = str::stream() << name << '@' << db;
        expectUnique = str::stream() << db << '.' << name;
    }
    ASSERT_EQ(obj.getDisplayName(), expectDisplay);
    ASSERT_EQ(stream<StringBuilder>(obj), expectDisplay);
    ASSERT_EQ(stream<std::ostringstream>(obj), expectDisplay);
    ASSERT_EQ(obj.getUnambiguousName(), expectUnique);
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
    auto obj = BSON(Traits<T>::kFieldName << name << "db" << db);
    checkValueAssertions(T::parseFromBSON(BSON("" << obj).firstElement()), name, db);

    // RoleName doesn't support parseFromBSONObj()
    if constexpr (std::is_same_v<T, UserName>) {
        checkValueAssertions(T::parseFromBSONObj(obj), name, db);
    }
}

template <typename T, typename Name, typename Db>
void doBSONParseFailure(Name name, Db db) {
    auto obj = BSON(Traits<T>::kFieldName << name << "db" << db);
    ASSERT_THROWS(T::parseFromBSON(BSON("" << obj).firstElement()), AssertionException);

    // RoleName doesn't support parseFromBSONObj()
    if constexpr (std::is_same_v<T, UserName>) {
        ASSERT_THROWS(T::parseFromBSONObj(obj), AssertionException);
    }
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
    // RoleName doesn't support parse(StringData)
}

}  // namespace
}  // namespace mongo
